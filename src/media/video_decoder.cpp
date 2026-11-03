#include "media/video_decoder.h"

#include "core/log.h"

#include <cerrno>
#include <cstring>
#include <format>
#include <string_view>
#include <unistd.h>
#include <utility>

#if NOCTALIA_HAVE_VIDEO
#include "core/deferred_call.h"

#include <array>
#include <atomic>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <string>
#include <sys/eventfd.h>
#include <thread>
#include <vector>
#endif

#if NOCTALIA_HAVE_VIDEO_VA
// The va helper library is flagged unstable, but the buffer/surface accessors used here have been
// unchanged since it was made public in GStreamer 1.22.
#define GST_USE_UNSTABLE_API
#include <fcntl.h>
#include <gst/va/gstva.h>
#include <mutex>
#include <unordered_map>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#endif

namespace {

  constexpr Logger kLog("video");

} // namespace

#if NOCTALIA_HAVE_VIDEO

namespace {

  // decodebin's autoplug-select result. GStreamer does not install the header that declares it.
  enum AutoplugSelectResult : gint { kAutoplugTry = 0, kAutoplugSkip = 2 };

  constexpr const char* kUnknownTypeMessage = "noctalia-video-unknown-type";

#if NOCTALIA_HAVE_VIDEO_VA
  // Real dmabufs first. VA decoders that only advertise system memory still output VA surfaces,
  // which vaFrame() exports; formats are limited to those it can describe.
  constexpr const char* kZeroCopyCaps =
      "video/x-raw(memory:DMABuf),format=DMA_DRM; video/x-raw,format={NV12,P010_10LE}";
#else
  constexpr const char* kZeroCopyCaps = "video/x-raw(memory:DMABuf),format=DMA_DRM";
#endif

  enum class GstreamerState { Idle, Loading, Ready, Failed };

  // Main-thread state of the one-time GStreamer initialization.
  struct GstreamerInit {
    GstreamerState state = GstreamerState::Idle;
    // Wake fds of decoders waiting for initialization to finish.
    std::vector<int> waiters;
  };

  GstreamerInit& gstreamerInit() {
    static GstreamerInit init;
    return init;
  }

  void wake(int fd);

  // Starts GStreamer initialization on first use and returns its state. Main thread only.
  //
  // Initialization runs on a worker thread because it can take over a second: a registry rescan
  // (whenever another GStreamer program changed it) and loading the va plugin, which initializes
  // every VA driver, both block. Completion is posted back to the main thread, which wakes the
  // waiting decoders.
  GstreamerState gstreamerState() {
    GstreamerInit& init = gstreamerInit();
    if (init.state != GstreamerState::Idle) {
      return init.state;
    }
    init.state = GstreamerState::Loading;

    bool setAllDrivers = false;
#if NOCTALIA_HAVE_VIDEO_VA
    // The va plugin only registers decoders for VA drivers GStreamer has validated (Intel and
    // Mesa) unless GST_VA_ALL_DRIVERS is set while the registry is scanned and while the plugin is
    // loaded. Others, notably nvidia-vaapi-driver, decode fine (see acquireVaDisplay()). The
    // variable is set and cleared on the main thread, like the shell's other environment changes,
    // and only for the duration of initialization so launched apps do not inherit it.
    setAllDrivers = g_getenv("GST_VA_ALL_DRIVERS") == nullptr;
    if (setAllDrivers) {
      g_setenv("GST_VA_ALL_DRIVERS", "1", TRUE);
    }
#endif

    std::thread([setAllDrivers] {
      GError* error = nullptr;
      const bool ok = gst_init_check(nullptr, nullptr, &error) != FALSE;
      std::string message = error != nullptr ? error->message : "unknown error";
      g_clear_error(&error);
#if NOCTALIA_HAVE_VIDEO_VA
      if (ok) {
        if (GstPlugin* va = gst_plugin_load_by_name("va"); va != nullptr) {
          gst_object_unref(va);
        }
      }
#endif
      DeferredCall::callLater([ok, setAllDrivers, message = std::move(message)] {
        if (setAllDrivers) {
          g_unsetenv("GST_VA_ALL_DRIVERS");
        }
        GstreamerInit& done = gstreamerInit();
        if (ok) {
          kLog.debug("initialized {}", gst_version_string());
          done.state = GstreamerState::Ready;
        } else {
          kLog.warn("GStreamer initialization failed: {}", message);
          done.state = GstreamerState::Failed;
        }
        for (const int fd : std::exchange(done.waiters, {})) {
          wake(fd);
        }
      });
    }).detach();
    return init.state;
  }

  std::string_view capsName(const GstCaps* caps) {
    if (caps == nullptr || gst_caps_is_empty(caps) || gst_caps_is_any(caps)) {
      return {};
    }
    return gst_structure_get_name(gst_caps_get_structure(caps, 0));
  }

  // Streams the shell never shows. decodebin exposes them undecoded and they drain into a fakesink.
  bool isIgnoredStream(std::string_view name) {
    for (std::string_view prefix :
         {"audio/", "text/", "subpicture/", "subtitle/", "closedcaption/", "application/x-subtitle",
          "application/x-ssa", "application/x-ass"}) {
      if (name.starts_with(prefix)) {
        return true;
      }
    }
    return false;
  }

  // Decoders from GStreamer's va plugin. Their frames are VA surfaces whatever the negotiated caps
  // say, so vaFrame() can export them even when the driver advertises only system memory.
  bool isVaDecoder([[maybe_unused]] GstElementFactory* factory) {
#if NOCTALIA_HAVE_VIDEO_VA
    const gchar* plugin = gst_plugin_feature_get_plugin_name(GST_PLUGIN_FEATURE(factory));
    return plugin != nullptr && std::string_view(plugin) == "va";
#else
    return false;
#endif
  }

  bool decoderOutputsDmaBuf(GstElementFactory* factory) {
    for (const GList* item = gst_element_factory_get_static_pad_templates(factory); item != nullptr;
         item = item->next) {
      auto* templ = static_cast<GstStaticPadTemplate*>(item->data);
      if (templ->direction != GST_PAD_SRC) {
        continue;
      }
      GstCaps* caps = gst_static_pad_template_get_caps(templ);
      bool found = false;
      for (guint i = 0; caps != nullptr && i < gst_caps_get_size(caps) && !found; ++i) {
        const GstCapsFeatures* features = gst_caps_get_features(caps, i);
        found = features != nullptr && gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_DMABUF);
      }
      if (caps != nullptr) {
        gst_caps_unref(caps);
      }
      if (found) {
        return true;
      }
    }
    return false;
  }

  VideoFrame::YuvMatrix yuvMatrix(const GstVideoColorimetry& colorimetry, int height) {
    switch (colorimetry.matrix) {
    case GST_VIDEO_COLOR_MATRIX_BT601:
    case GST_VIDEO_COLOR_MATRIX_SMPTE240M:
    case GST_VIDEO_COLOR_MATRIX_FCC:
      return VideoFrame::YuvMatrix::Bt601;
    case GST_VIDEO_COLOR_MATRIX_BT709:
      return VideoFrame::YuvMatrix::Bt709;
    case GST_VIDEO_COLOR_MATRIX_BT2020:
      return VideoFrame::YuvMatrix::Bt2020;
    default:
      // Same guess players make for untagged streams.
      return height >= 720 ? VideoFrame::YuvMatrix::Bt709 : VideoFrame::YuvMatrix::Bt601;
    }
  }

  void wake(int fd) {
    if (fd >= 0) {
      const std::uint64_t one = 1;
      [[maybe_unused]] const auto written = ::write(fd, &one, sizeof(one));
    }
  }

  std::shared_ptr<void> sampleStorage(GstSample* sample) {
    return {sample, [](void* owned) { gst_sample_unref(static_cast<GstSample*>(owned)); }};
  }

  // Takes ownership of `sample`.
  VideoFramePtr dmabufFrame(GstSample* sample) {
    auto frame = std::make_shared<VideoFrame>();
    frame->kind = VideoFrame::Kind::DmaBuf;
    frame->storage = sampleStorage(sample);

    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstVideoInfoDmaDrm drm;
    if (caps == nullptr || buffer == nullptr || gst_video_info_dma_drm_from_caps(&drm, caps) == FALSE) {
      return nullptr;
    }
    frame->drmFourcc = drm.drm_fourcc;
    frame->drmModifier = drm.drm_modifier;
    frame->width = static_cast<int>(GST_VIDEO_INFO_WIDTH(&drm.vinfo));
    frame->height = static_cast<int>(GST_VIDEO_INFO_HEIGHT(&drm.vinfo));
    frame->matrix = yuvMatrix(drm.vinfo.colorimetry, frame->height);
    frame->fullRange = drm.vinfo.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;

    // Hardware decoders describe their (often tiled) layout in a video meta. Without one only a
    // linear layout can be derived from the caps.
    std::size_t planeCount = 0;
    std::array<gsize, VideoFrame::kMaxPlanes> offsets{};
    std::array<gint, VideoFrame::kMaxPlanes> strides{};
    if (const GstVideoMeta* meta = gst_buffer_get_video_meta(buffer); meta != nullptr) {
      planeCount = meta->n_planes;
      for (std::size_t i = 0; i < planeCount && i < VideoFrame::kMaxPlanes; ++i) {
        offsets[i] = meta->offset[i];
        strides[i] = meta->stride[i];
      }
      frame->width = static_cast<int>(meta->width);
      frame->height = static_cast<int>(meta->height);
    } else {
      GstVideoInfo linear;
      if (gst_video_info_dma_drm_to_video_info(&drm, &linear) == FALSE) {
        return nullptr;
      }
      planeCount = GST_VIDEO_INFO_N_PLANES(&linear);
      for (std::size_t i = 0; i < planeCount && i < VideoFrame::kMaxPlanes; ++i) {
        offsets[i] = GST_VIDEO_INFO_PLANE_OFFSET(&linear, i);
        strides[i] = GST_VIDEO_INFO_PLANE_STRIDE(&linear, i);
      }
    }
    if (planeCount == 0 || planeCount > VideoFrame::kMaxPlanes || frame->width <= 0 || frame->height <= 0) {
      return nullptr;
    }

    for (std::size_t i = 0; i < planeCount; ++i) {
      guint index = 0;
      guint length = 0;
      gsize skip = 0;
      if (gst_buffer_find_memory(buffer, offsets[i], 1, &index, &length, &skip) == FALSE) {
        return nullptr;
      }
      GstMemory* memory = gst_buffer_peek_memory(buffer, index);
      if (memory == nullptr || gst_is_dmabuf_memory(memory) == FALSE || strides[i] <= 0) {
        return nullptr;
      }
      frame->planes[i] = VideoFrame::Plane{
          .fd = gst_dmabuf_memory_get_fd(memory),
          .offset = static_cast<std::uint32_t>(memory->offset + skip),
          .pitch = static_cast<std::uint32_t>(strides[i]),
      };
    }
    frame->planeCount = planeCount;
    return frame;
  }

  // Takes ownership of `sample`.
  VideoFramePtr rgbaFrame(GstSample* sample) {
    struct Mapped {
      GstSample* sample = nullptr;
      GstVideoFrame frame{};
      bool mapped = false;
      std::vector<std::uint8_t> packed;
    };
    std::shared_ptr<Mapped> mapped(new Mapped{.sample = sample}, [](Mapped* owned) {
      if (owned->mapped) {
        gst_video_frame_unmap(&owned->frame);
      }
      gst_sample_unref(owned->sample);
      delete owned;
    });

    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstVideoInfo info;
    if (caps == nullptr || buffer == nullptr || gst_video_info_from_caps(&info, caps) == FALSE) {
      return nullptr;
    }
    if (gst_video_frame_map(&mapped->frame, &info, buffer, GST_MAP_READ) == FALSE) {
      return nullptr;
    }
    mapped->mapped = true;

    auto frame = std::make_shared<VideoFrame>();
    frame->kind = VideoFrame::Kind::Rgba;
    frame->width = static_cast<int>(GST_VIDEO_FRAME_WIDTH(&mapped->frame));
    frame->height = static_cast<int>(GST_VIDEO_FRAME_HEIGHT(&mapped->frame));
    if (frame->width <= 0 || frame->height <= 0) {
      return nullptr;
    }
    const auto* data = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&mapped->frame, 0));
    const auto stride = static_cast<std::size_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&mapped->frame, 0));
    const auto rowBytes = static_cast<std::size_t>(frame->width) * 4U;
    if (stride == rowBytes) {
      frame->pixels = data;
    } else {
      // Texture uploads expect tightly packed rows.
      mapped->packed.resize(rowBytes * static_cast<std::size_t>(frame->height));
      for (int y = 0; y < frame->height; ++y) {
        std::memcpy(
            mapped->packed.data() + (rowBytes * static_cast<std::size_t>(y)),
            data + (stride * static_cast<std::size_t>(y)), rowBytes
        );
      }
      frame->pixels = mapped->packed.data();
    }
    frame->storage = std::move(mapped);
    return frame;
  }

#if NOCTALIA_HAVE_VIDEO_VA

  // A VA display the shell opened itself. GStreamer never terminates a wrapped (foreign) display,
  // so this is released when the GstVaDisplay wrapping it is finalized.
  struct OwnedVaDisplay {
    int fd = -1;
    VADisplay display = nullptr;

    static void release(gpointer data) {
      auto* owned = static_cast<OwnedVaDisplay*>(data);
      vaTerminate(owned->display);
      ::close(owned->fd);
      delete owned;
    }
  };

  // Returns a new reference to the shell's VA display for a render node, opening it on first use.
  // va elements only accept the VA drivers GStreamer has validated (Intel and Mesa) when they open
  // a display themselves; displays handed to them by the application skip that check. Others,
  // notably nvidia-vaapi-driver, decode fine and their surfaces export as dmabufs. Displays are
  // shared by every pipeline and closed once GStreamer drops its last reference (the last buffer
  // from them included). Called from streaming threads.
  GstVaDisplay* acquireVaDisplay(const std::string& path) {
    static auto* mutex = new std::mutex;
    // Leaked on purpose: the weak references must outlive any display finalized during exit.
    static auto* displays = new std::unordered_map<std::string, GWeakRef>;
    const std::lock_guard lock(*mutex);
    auto [entry, inserted] = displays->try_emplace(path);
    if (inserted) {
      g_weak_ref_init(&entry->second, nullptr);
    } else if (gpointer existing = g_weak_ref_get(&entry->second); existing != nullptr) {
      return static_cast<GstVaDisplay*>(existing);
    }

    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      kLog.debug("cannot open {}: {}", path, std::strerror(errno));
      return nullptr;
    }
    VADisplay vaDisplay = vaGetDisplayDRM(fd);
    int major = 0;
    int minor = 0;
    if (vaDisplay == nullptr || vaInitialize(vaDisplay, &major, &minor) != VA_STATUS_SUCCESS) {
      kLog.debug("cannot initialize VA-API on {}", path);
      if (vaDisplay != nullptr) {
        vaTerminate(vaDisplay);
      }
      ::close(fd);
      return nullptr;
    }
    GstVaDisplay* display = gst_va_display_wrapped_new(vaDisplay);
    if (display == nullptr) {
      vaTerminate(vaDisplay);
      ::close(fd);
      return nullptr;
    }
    if (g_object_is_floating(display)) {
      gst_object_ref_sink(display);
    }
    g_object_set_data_full(
        G_OBJECT(display), "noctalia-va-display", new OwnedVaDisplay{.fd = fd, .display = vaDisplay},
        &OwnedVaDisplay::release
    );
    g_weak_ref_set(&entry->second, display);
    kLog.debug("opened VA-API {}.{} on {}: {}", major, minor, path, vaQueryVendorString(vaDisplay));
    return display;
  }

  // Answers a va element's display request with the shell's display for its render node.
  void provideVaDisplay(GstElement* element) {
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(element), "device-path") == nullptr) {
      return;
    }
    gchar* path = nullptr;
    g_object_get(element, "device-path", &path, nullptr);
    GstVaDisplay* display = path != nullptr ? acquireVaDisplay(path) : nullptr;
    g_free(path);
    if (display == nullptr) {
      // The element opens its own display instead.
      return;
    }
    GstContext* context = gst_context_new(GST_VA_DISPLAY_HANDLE_CONTEXT_TYPE_STR, TRUE);
    gst_context_set_va_display(context, display);
    gst_element_set_context(element, context);
    gst_context_unref(context);
    gst_object_unref(display);
  }

  GstBusSyncReply onBusSync(GstBus* /*bus*/, GstMessage* message, gpointer /*data*/) {
    const gchar* type = nullptr;
    if (GST_MESSAGE_TYPE(message) != GST_MESSAGE_NEED_CONTEXT
        || gst_message_parse_context_type(message, &type) == FALSE
        || std::string_view(type) != GST_VA_DISPLAY_HANDLE_CONTEXT_TYPE_STR) {
      return GST_BUS_PASS;
    }
    provideVaDisplay(GST_ELEMENT(GST_MESSAGE_SRC(message)));
    gst_message_unref(message);
    return GST_BUS_DROP;
  }

  // The VA surface behind a buffer from a va decoder, or VA_INVALID_SURFACE.
  VASurfaceID vaSurface(GstBuffer* buffer, VADisplay& display) {
    GstMemory* memory = gst_buffer_n_memory(buffer) > 0 ? gst_buffer_peek_memory(buffer, 0) : nullptr;
    if (memory == nullptr || memory->allocator == nullptr || !GST_IS_VA_ALLOCATOR(memory->allocator)) {
      return VA_INVALID_SURFACE;
    }
    GstVaDisplay* gstDisplay = gst_va_memory_peek_display(memory);
    display = gstDisplay != nullptr ? gst_va_display_get_va_dpy(gstDisplay) : nullptr;
    return display != nullptr ? gst_va_memory_get_surface(memory) : VA_INVALID_SURFACE;
  }

  // Waits for the decoder (and, for drivers like nvidia-vaapi-driver, the copy into the surface's
  // exportable backing image) to finish. Runs on the streaming thread before the appsink queues the
  // buffer, so the main thread never blocks on the GPU.
  void syncVaBuffer(GstBuffer* buffer) {
    VADisplay display = nullptr;
    if (const VASurfaceID surface = vaSurface(buffer, display); surface != VA_INVALID_SURFACE) {
      vaSyncSurface(display, surface);
    }
  }

  constexpr std::uint32_t drmFourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(a)
        | (static_cast<std::uint32_t>(b) << 8U)
        | (static_cast<std::uint32_t>(c) << 16U)
        | (static_cast<std::uint32_t>(d) << 24U);
  }

  // Multi-planar formats whose VA and DRM fourccs agree, so separately exported layers (one per
  // plane) can be recombined into a single image the renderer converts to RGB.
  bool composableFourcc(std::uint32_t fourcc) {
    return fourcc == drmFourcc('N', 'V', '1', '2')
        || fourcc == drmFourcc('P', '0', '1', '0')
        || fourcc == drmFourcc('P', '0', '1', '2')
        || fourcc == drmFourcc('P', '0', '1', '6');
  }

  // Takes ownership of `sample`. Exports the frame's VA surface as dmabufs (zero copy).
  VideoFramePtr vaFrame(GstSample* sample) {
    struct Exported {
      GstSample* sample = nullptr;
      std::array<int, VideoFrame::kMaxPlanes> fds{-1, -1, -1, -1};
    };
    std::shared_ptr<Exported> exported(new Exported{.sample = sample}, [](Exported* owned) {
      for (const int fd : owned->fds) {
        if (fd >= 0) {
          ::close(fd);
        }
      }
      gst_sample_unref(owned->sample);
      delete owned;
    });

    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstVideoInfo info;
    if (caps == nullptr || buffer == nullptr || gst_video_info_from_caps(&info, caps) == FALSE) {
      return nullptr;
    }
    VADisplay display = nullptr;
    const VASurfaceID surface = vaSurface(buffer, display);
    if (surface == VA_INVALID_SURFACE) {
      return nullptr;
    }

    // Composed layers describe the whole frame as one image. Some drivers (nvidia-vaapi-driver)
    // only export one layer per plane, which is recombined below.
    VADRMPRIMESurfaceDescriptor desc{};
    constexpr std::uint32_t kExportFlags = VA_EXPORT_SURFACE_READ_ONLY;
    if (vaExportSurfaceHandle(
            display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, kExportFlags | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
            &desc
        ) != VA_STATUS_SUCCESS
        && vaExportSurfaceHandle(
               display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
               kExportFlags | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc
           ) != VA_STATUS_SUCCESS) {
      return nullptr;
    }
    const std::size_t objectCount = std::min<std::size_t>(desc.num_objects, std::size(desc.objects));
    // Every exported descriptor is owned from here, including any beyond what fits a frame.
    for (std::size_t i = 0; i < objectCount; ++i) {
      if (i < VideoFrame::kMaxPlanes) {
        exported->fds[i] = desc.objects[i].fd;
      } else if (desc.objects[i].fd >= 0) {
        ::close(desc.objects[i].fd);
      }
    }
    if (objectCount == 0 || objectCount > VideoFrame::kMaxPlanes || desc.num_layers == 0) {
      return nullptr;
    }

    auto frame = std::make_shared<VideoFrame>();
    frame->kind = VideoFrame::Kind::DmaBuf;
    frame->width = static_cast<int>(GST_VIDEO_INFO_WIDTH(&info));
    frame->height = static_cast<int>(GST_VIDEO_INFO_HEIGHT(&info));
    frame->matrix = yuvMatrix(info.colorimetry, frame->height);
    frame->fullRange = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;

    std::uint32_t fourcc = 0;
    if (desc.num_layers == 1) {
      fourcc = desc.layers[0].drm_format;
    } else if (composableFourcc(desc.fourcc)) {
      fourcc = desc.fourcc;
    } else {
      return nullptr;
    }
    const std::size_t layerCount = std::min<std::size_t>(desc.num_layers, std::size(desc.layers));
    std::size_t planeCount = 0;
    bool haveModifier = false;
    for (std::size_t layer = 0; layer < layerCount; ++layer) {
      const auto& source = desc.layers[layer];
      // A recombined frame takes exactly one plane from each layer.
      if (desc.num_layers > 1 && source.num_planes != 1) {
        return nullptr;
      }
      for (std::size_t p = 0; p < source.num_planes && p < std::size(source.object_index); ++p) {
        const std::uint32_t object = source.object_index[p];
        if (planeCount >= VideoFrame::kMaxPlanes || object >= objectCount) {
          return nullptr;
        }
        // EGL takes one modifier per plane, but VideoFrame carries one per frame.
        const std::uint64_t modifier = desc.objects[object].drm_format_modifier;
        if (haveModifier && modifier != frame->drmModifier) {
          return nullptr;
        }
        frame->drmModifier = modifier;
        haveModifier = true;
        frame->planes[planeCount++] = VideoFrame::Plane{
            .fd = desc.objects[object].fd,
            .offset = source.offset[p],
            .pitch = source.pitch[p],
        };
      }
    }
    if (planeCount == 0 || frame->width <= 0 || frame->height <= 0) {
      return nullptr;
    }
    frame->drmFourcc = fourcc;
    frame->planeCount = planeCount;
    frame->storage = std::move(exported);
    return frame;
  }

#else

  GstBusSyncReply onBusSync(GstBus* /*bus*/, GstMessage* /*message*/, gpointer /*data*/) { return GST_BUS_PASS; }
  void syncVaBuffer(GstBuffer* /*buffer*/) {}
  VideoFramePtr vaFrame(GstSample* sample) {
    gst_sample_unref(sample);
    return nullptr;
  }

#endif

  bool hasDmaBufFeature(const GstCaps* caps) {
    const GstCapsFeatures* features =
        caps != nullptr && !gst_caps_is_empty(caps) ? gst_caps_get_features(caps, 0) : nullptr;
    return features != nullptr && gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_DMABUF);
  }

  // Takes ownership of `sample`. Real dmabuf caps win; otherwise the buffer must be a VA surface.
  VideoFramePtr zeroCopyFrame(GstSample* sample) {
    return hasDmaBufFeature(gst_sample_get_caps(sample)) ? dmabufFrame(sample) : vaFrame(sample);
  }

} // namespace

struct VideoDecoder::Pipeline {
  GstElement* pipeline = nullptr;
  GstElement* sink = nullptr;
  // Sink pad the first decoded video stream links to: the appsink, or videoconvert for RGBA.
  GstPad* videoTarget = nullptr;
  GstBus* bus = nullptr;
  int busFd = -1;
  int wakeFd = -1;
  bool zeroCopy = false;
  bool gotFrame = false;
  std::atomic<bool> videoLinked{false};

  Pipeline() = default;
  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  ~Pipeline() {
    if (pipeline != nullptr) {
      // Joins every streaming thread, so no callback can touch this object afterwards.
      gst_element_set_state(pipeline, GST_STATE_NULL);
    }
    if (videoTarget != nullptr) {
      gst_object_unref(videoTarget);
    }
    if (bus != nullptr) {
      gst_object_unref(bus);
    }
    if (pipeline != nullptr) {
      gst_object_unref(pipeline);
    }
  }

  // ── Streaming-thread callbacks: they only link pads, post bus messages, and poke wakeFd ──

  static gboolean onAutoplugContinue(GstElement* /*bin*/, GstPad* /*pad*/, GstCaps* caps, gpointer /*self*/) {
    return isIgnoredStream(capsName(caps)) ? FALSE : TRUE;
  }

  static gint
  onAutoplugSelect(GstElement* /*bin*/, GstPad* /*pad*/, GstCaps* /*caps*/, GstElementFactory* factory, gpointer data) {
    const auto* self = static_cast<const Pipeline*>(data);
    if (!self->zeroCopy
        || gst_element_factory_list_is_type(
               factory, GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO
           ) == FALSE) {
      return kAutoplugTry;
    }
    // Only decoders that can hand out dmabufs (or VA surfaces exportable as dmabufs) keep frames
    // off the CPU.
    if (decoderOutputsDmaBuf(factory) || isVaDecoder(factory)) {
      kLog.debug("zero-copy decoder candidate {}", gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)));
      return kAutoplugTry;
    }
    return kAutoplugSkip;
  }

  static void onUnknownType(GstElement* bin, GstPad* /*pad*/, GstCaps* caps, gpointer /*self*/) {
    gchar* description = gst_caps_to_string(caps);
    gst_element_post_message(
        bin,
        gst_message_new_application(
            GST_OBJECT(bin),
            gst_structure_new(
                kUnknownTypeMessage, "caps", G_TYPE_STRING, description != nullptr ? description : "unknown", nullptr
            )
        )
    );
    g_free(description);
  }

  static void onPadAdded(GstElement* /*bin*/, GstPad* pad, gpointer data) {
    auto* self = static_cast<Pipeline*>(data);
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (caps == nullptr) {
      caps = gst_pad_query_caps(pad, nullptr);
    }
    const bool video = capsName(caps).starts_with("video/x-raw");
    if (caps != nullptr) {
      gst_caps_unref(caps);
    }

    if (video && !self->videoLinked.exchange(true)) {
      if (gst_pad_link(pad, self->videoTarget) != GST_PAD_LINK_OK) {
        GST_ELEMENT_ERROR(self->pipeline, CORE, NEGOTIATION, ("cannot link decoded video"), (nullptr));
      }
      return;
    }

    // Audio, subtitles, and extra video tracks are discarded.
    GstElement* discard = gst_element_factory_make("fakesink", nullptr);
    if (discard == nullptr) {
      return;
    }
    g_object_set(discard, "sync", FALSE, "async", FALSE, nullptr);
    gst_bin_add(GST_BIN(self->pipeline), discard);
    GstPad* sinkPad = gst_element_get_static_pad(discard, "sink");
    gst_pad_link(pad, sinkPad);
    gst_object_unref(sinkPad);
    gst_element_sync_state_with_parent(discard);
  }

  static GstFlowReturn onNewSample(GstAppSink* /*sink*/, gpointer data) {
    wake(static_cast<const Pipeline*>(data)->wakeFd);
    return GST_FLOW_OK;
  }

  // Zero-copy sink pad probe. Advertising video meta support lets va decoders hand their own
  // surfaces downstream even with system-memory caps (otherwise they copy into plain buffers), and
  // each surface is synced before the appsink queues it.
  static GstPadProbeReturn onSinkPadProbe(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer /*self*/) {
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
      syncVaBuffer(GST_PAD_PROBE_INFO_BUFFER(info));
    } else if (
        GstQuery* query = GST_PAD_PROBE_INFO_QUERY(info);
        query != nullptr && GST_QUERY_TYPE(query) == GST_QUERY_ALLOCATION
    ) {
      gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr);
    }
    return GST_PAD_PROBE_OK;
  }

  bool build(const std::string& path) {
    pipeline = gst_pipeline_new("noctalia-video");
    GstElement* source = gst_element_factory_make("filesrc", nullptr);
    GstElement* decode = gst_element_factory_make("decodebin", nullptr);
    sink = gst_element_factory_make("appsink", nullptr);
    GstElement* convert = zeroCopy ? nullptr : gst_element_factory_make("videoconvert", nullptr);
    if (pipeline == nullptr
        || source == nullptr
        || decode == nullptr
        || sink == nullptr
        || (!zeroCopy && convert == nullptr)) {
      for (GstElement* element : {source, decode, sink, convert}) {
        if (element != nullptr) {
          gst_object_unref(gst_object_ref_sink(element));
        }
      }
      sink = nullptr;
      kLog.warn("missing GStreamer elements (filesrc, decodebin, appsink, videoconvert); install gst-plugins-base");
      return false;
    }

    g_object_set(source, "location", path.c_str(), nullptr);

    GstCaps* sinkCaps = gst_caps_from_string(zeroCopy ? kZeroCopyCaps : "video/x-raw,format=RGBA");
    // sync paces frames to the stream clock; one dropping slot keeps only the newest frame queued.
    g_object_set(
        sink, "caps", sinkCaps, "sync", TRUE, "max-buffers", 1U, "drop", TRUE, "enable-last-sample", FALSE, nullptr
    );
    gst_caps_unref(sinkCaps);
    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = &Pipeline::onNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, this, nullptr);
    if (zeroCopy) {
      GstPad* sinkPad = gst_element_get_static_pad(sink, "sink");
      gst_pad_add_probe(
          sinkPad, static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM),
          &Pipeline::onSinkPadProbe, this, nullptr
      );
      gst_object_unref(sinkPad);
    }

    g_signal_connect(decode, "autoplug-continue", G_CALLBACK(&Pipeline::onAutoplugContinue), this);
    g_signal_connect(decode, "autoplug-select", G_CALLBACK(&Pipeline::onAutoplugSelect), this);
    g_signal_connect(decode, "unknown-type", G_CALLBACK(&Pipeline::onUnknownType), this);
    g_signal_connect(decode, "pad-added", G_CALLBACK(&Pipeline::onPadAdded), this);

    gst_bin_add_many(GST_BIN(pipeline), source, decode, sink, nullptr);
    if (convert != nullptr) {
      if (g_object_class_find_property(G_OBJECT_GET_CLASS(convert), "n-threads") != nullptr) {
        g_object_set(convert, "n-threads", 0U, nullptr);
      }
      gst_bin_add(GST_BIN(pipeline), convert);
      if (gst_element_link(convert, sink) == FALSE) {
        return false;
      }
    }
    if (gst_element_link(source, decode) == FALSE) {
      return false;
    }
    videoTarget = gst_element_get_static_pad(convert != nullptr ? convert : sink, "sink");

    bus = gst_element_get_bus(pipeline);
    // Display requests arrive on streaming threads and must be answered before the element
    // continues, so they are handled synchronously rather than through the polled bus.
    gst_bus_set_sync_handler(bus, &onBusSync, nullptr, nullptr);
    GPollFD pollFd{};
    gst_bus_get_pollfd(bus, &pollFd);
    busFd = pollFd.fd;
    return true;
  }
};

VideoDecoder::VideoDecoder() {
  m_wakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (m_wakeFd < 0) {
    kLog.warn("eventfd failed: {}", std::strerror(errno));
  }
}

VideoDecoder::~VideoDecoder() {
  m_onChange = {};
  stop();
  m_frame.reset();
  std::erase(gstreamerInit().waiters, m_wakeFd);
  if (m_wakeFd >= 0) {
    ::close(m_wakeFd);
  }
}

bool VideoDecoder::setPath(std::string path) {
  if (path == m_path) {
    return false;
  }
  m_path = std::move(path);
  rebuild(true);
  return true;
}

void VideoDecoder::requestCpuFallback() {
  if (m_pipeline == nullptr || !m_pipeline->zeroCopy || m_fallbackPending) {
    return;
  }
  m_fallbackPending = true;
  wake(m_wakeFd);
}

int VideoDecoder::wakeFd() const noexcept { return m_pipeline != nullptr || m_waitingForInit ? m_wakeFd : -1; }

int VideoDecoder::busFd() const noexcept { return m_pipeline != nullptr ? m_pipeline->busFd : -1; }

void VideoDecoder::dispatch() {
  if (m_wakeFd >= 0) {
    std::uint64_t count = 0;
    while (::read(m_wakeFd, &count, sizeof(count)) > 0) {
    }
  }
  if (m_waitingForInit) {
    if (gstreamerState() != GstreamerState::Loading) {
      m_waitingForInit = false;
      rebuild(true);
    }
    return;
  }
  if (std::exchange(m_fallbackPending, false)) {
    kLog.info("renderer cannot import the decoder's dmabufs for '{}'; decoding to system memory", m_path);
    rebuild(false);
    return;
  }
  handleBus();
  pullFrames();
}

void VideoDecoder::rebuild(bool zeroCopy) {
  stop();
  m_fallbackPending = false;
  const bool hadFrame = m_frame != nullptr;
  m_frame.reset();
  if (hadFrame) {
    notify();
  }
  if (m_path.empty() || m_wakeFd < 0) {
    return;
  }
  switch (gstreamerState()) {
  case GstreamerState::Ready:
    break;
  case GstreamerState::Loading:
    // dispatch() rebuilds once initialization finishes.
    if (!std::exchange(m_waitingForInit, true)) {
      gstreamerInit().waiters.push_back(m_wakeFd);
    }
    return;
  default:
    return;
  }
  if (::access(m_path.c_str(), R_OK) != 0) {
    fail(std::format("cannot read file: {}", std::strerror(errno)));
    return;
  }

  auto pipeline = std::make_unique<Pipeline>();
  pipeline->zeroCopy = zeroCopy;
  pipeline->wakeFd = m_wakeFd;
  if (!pipeline->build(m_path)) {
    if (zeroCopy) {
      rebuild(false);
    } else {
      fail("cannot build the decode pipeline");
    }
    return;
  }
  m_pipeline = std::move(pipeline);
  if (gst_element_set_state(m_pipeline->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    // The bus carries the reason; handleBus() reports it or falls back.
    handleBus();
    if (m_pipeline != nullptr && m_pipeline->zeroCopy == zeroCopy) {
      if (zeroCopy) {
        rebuild(false);
      } else {
        fail("cannot start playback");
      }
    }
    return;
  }
  kLog.debug("starting '{}' ({})", m_path, zeroCopy ? "zero copy" : "system memory");
}

void VideoDecoder::stop() { m_pipeline.reset(); }

void VideoDecoder::fail(std::string_view reason) {
  kLog.warn("video '{}': {}", m_path, reason);
  stop();
  m_frame.reset();
  notify();
}

void VideoDecoder::handleBus() {
  if (m_pipeline == nullptr) {
    return;
  }
  // Messages are handled one at a time because a failure replaces the pipeline (and its bus).
  while (m_pipeline != nullptr) {
    GstMessage* message = gst_bus_pop(m_pipeline->bus);
    if (message == nullptr) {
      return;
    }
    std::string failure;
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
      GError* error = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      failure = error != nullptr ? error->message : "unknown error";
      kLog.debug("pipeline error: {} ({})", failure, debug != nullptr ? debug : "");
      g_clear_error(&error);
      g_free(debug);
      break;
    }
    case GST_MESSAGE_APPLICATION: {
      const GstStructure* structure = gst_message_get_structure(message);
      if (structure != nullptr && gst_structure_has_name(structure, kUnknownTypeMessage)) {
        const gchar* caps = gst_structure_get_string(structure, "caps");
        const std::string_view name = caps != nullptr ? caps : "";
        // Only an undecodable video stream matters; containers report their own errors.
        if (name.starts_with("video/") && !isIgnoredStream(name)) {
          failure = std::format("no usable decoder for {}", name);
        }
      }
      break;
    }
    case GST_MESSAGE_EOS:
      // Loop: a flushing seek back to the start restarts the stream without rebuilding.
      if (gst_element_seek_simple(
              m_pipeline->pipeline, GST_FORMAT_TIME,
              static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0
          )
          == FALSE) {
        failure = "cannot seek back to the start to loop";
      }
      break;
    case GST_MESSAGE_WARNING: {
      GError* error = nullptr;
      gst_message_parse_warning(message, &error, nullptr);
      kLog.debug("pipeline warning: {}", error != nullptr ? error->message : "unknown");
      g_clear_error(&error);
      break;
    }
    default:
      break;
    }
    gst_message_unref(message);

    if (!failure.empty()) {
      if (m_pipeline->zeroCopy) {
        kLog.info("zero-copy playback unavailable for '{}' ({}); decoding to system memory", m_path, failure);
        rebuild(false);
      } else {
        fail(failure);
      }
    }
  }
}

void VideoDecoder::pullFrames() {
  if (m_pipeline == nullptr) {
    return;
  }
  GstSample* latest = nullptr;
  while (GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(m_pipeline->sink), 0)) {
    if (latest != nullptr) {
      gst_sample_unref(latest);
    }
    latest = sample;
  }
  if (latest == nullptr) {
    return;
  }

  const bool dmabufCaps = hasDmaBufFeature(gst_sample_get_caps(latest));
  VideoFramePtr frame = m_pipeline->zeroCopy ? zeroCopyFrame(latest) : rgbaFrame(latest);
  if (frame == nullptr) {
    if (m_pipeline->zeroCopy) {
      kLog.info(
          "decoder returned a frame the shell cannot import without copying for '{}'; decoding to system memory", m_path
      );
      rebuild(false);
    } else {
      fail("cannot map a decoded frame");
    }
    return;
  }
  if (!m_pipeline->gotFrame) {
    m_pipeline->gotFrame = true;
    kLog.info(
        "playing '{}' at {}x{} ({})", m_path, frame->width, frame->height,
        !m_pipeline->zeroCopy ? "system memory"
            : dmabufCaps      ? "zero-copy dmabuf"
                              : "zero-copy VA-API surface export"
    );
  }
  std::const_pointer_cast<VideoFrame>(frame)->serial = ++m_serial;
  m_frame = std::move(frame);
  notify();
}

#else // !NOCTALIA_HAVE_VIDEO

struct VideoDecoder::Pipeline {};

VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() = default;

bool VideoDecoder::setPath(std::string path) {
  if (path == m_path) {
    return false;
  }
  m_path = std::move(path);
  if (!m_path.empty()) {
    static bool logged = false;
    if (!std::exchange(logged, true)) {
      kLog.warn("ui.video needs GStreamer, but this build has no video support (meson -Dvideo=enabled)");
    }
  }
  return true;
}

void VideoDecoder::requestCpuFallback() {}
int VideoDecoder::wakeFd() const noexcept { return -1; }
int VideoDecoder::busFd() const noexcept { return -1; }
void VideoDecoder::dispatch() {}
void VideoDecoder::rebuild(bool /*zeroCopy*/) {}
void VideoDecoder::stop() {}
void VideoDecoder::fail(std::string_view /*reason*/) {}
void VideoDecoder::handleBus() {}
void VideoDecoder::pullFrames() {}

#endif

void VideoDecoder::notify() {
  if (m_onChange) {
    m_onChange();
  }
}
