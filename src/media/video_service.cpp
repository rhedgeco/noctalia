#include "media/video_service.h"

#include "core/log.h"
#include "render/gl_shared_context.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <sys/eventfd.h>
#include <unistd.h>

#if NOCTALIA_HAVE_VIDEO
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#endif

namespace {

  constexpr Logger kLog("video");

} // namespace

#if NOCTALIA_HAVE_VIDEO

namespace {

  void* getProcAddress(void* /*ctx*/, const char* name) {
    void* proc = reinterpret_cast<void*>(eglGetProcAddress(name));
    if (proc == nullptr) {
      proc = dlsym(RTLD_DEFAULT, name);
    }
    return proc;
  }

  // Binds a stream's context surfacelessly and restores whatever was current on exit, so the
  // shell's render contexts never observe the switch.
  class ScopedEglContext {
  public:
    ScopedEglContext(EGLDisplay display, EGLContext context)
        : m_display(display), m_prevDisplay(eglGetCurrentDisplay()), m_prevContext(eglGetCurrentContext()),
          m_prevDraw(eglGetCurrentSurface(EGL_DRAW)), m_prevRead(eglGetCurrentSurface(EGL_READ)) {
      m_ok = eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) == EGL_TRUE;
    }
    ~ScopedEglContext() {
      if (m_prevContext != EGL_NO_CONTEXT) {
        eglMakeCurrent(m_prevDisplay, m_prevDraw, m_prevRead, m_prevContext);
      } else {
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      }
    }
    ScopedEglContext(const ScopedEglContext&) = delete;
    ScopedEglContext& operator=(const ScopedEglContext&) = delete;

    bool ok() const noexcept { return m_ok; }

  private:
    EGLDisplay m_display;
    EGLDisplay m_prevDisplay;
    EGLContext m_prevContext;
    EGLSurface m_prevDraw;
    EGLSurface m_prevRead;
    bool m_ok = false;
  };

} // namespace

class VideoService::Stream final : public VideoStream {
public:
  Stream(std::string path, int wakeFd) : m_path(std::move(path)), m_wakeFd(wakeFd) {}

  ~Stream() override {
    if (m_render != nullptr) {
      mpv_render_context_set_update_callback(m_render, nullptr, nullptr);
      ScopedEglContext current(m_eglDisplay, m_context);
      mpv_render_context_free(m_render);
      destroyTarget();
    }
    if (m_mpv != nullptr) {
      mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
      mpv_terminate_destroy(m_mpv);
    }
    if (m_context != EGL_NO_CONTEXT) {
      eglDestroyContext(m_eglDisplay, m_context);
    }
  }

  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;

  bool start(wl_display* display, GlSharedContext& shared) {
    m_mpv = mpv_create();
    if (m_mpv == nullptr) {
      kLog.warn("mpv_create failed");
      return false;
    }

    // Silent, looping, input-free playback.
    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "hwdec", "auto-safe");
    mpv_set_option_string(m_mpv, "aid", "no");
    mpv_set_option_string(m_mpv, "sid", "no");
    mpv_set_option_string(m_mpv, "loop-file", "inf");
    mpv_set_option_string(m_mpv, "terminal", "no");
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
    mpv_set_option_string(m_mpv, "load-scripts", "no");
    mpv_set_option_string(m_mpv, "ytdl", "no");
    mpv_request_log_messages(m_mpv, "warn");

    if (const int err = mpv_initialize(m_mpv); err < 0) {
      kLog.warn("mpv_initialize failed: {}", mpv_error_string(err));
      return false;
    }

    m_eglDisplay = shared.display();
    try {
      m_context = shared.createContext(shared.rootContext(), "video");
    } catch (const std::exception& e) {
      kLog.warn("{}", e.what());
      return false;
    }

    {
      ScopedEglContext current(m_eglDisplay, m_context);
      if (!current.ok()) {
        kLog.warn("could not make video context current");
        return false;
      }
      mpv_opengl_init_params glParams{.get_proc_address = &getProcAddress, .get_proc_address_ctx = nullptr};
      std::array<mpv_render_param, 4> params = {{
          {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
          {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glParams},
          {MPV_RENDER_PARAM_WL_DISPLAY, display},
          {MPV_RENDER_PARAM_INVALID, nullptr},
      }};
      if (const int err = mpv_render_context_create(&m_render, m_mpv, params.data()); err < 0) {
        kLog.warn("mpv_render_context_create failed: {}", mpv_error_string(err));
        m_render = nullptr;
        return false;
      }
    }

    mpv_set_wakeup_callback(m_mpv, &Stream::wake, this);
    mpv_render_context_set_update_callback(m_render, &Stream::wake, this);

    std::array<const char*, 3> command = {"loadfile", m_path.c_str(), nullptr};
    if (const int err = mpv_command(m_mpv, command.data()); err < 0) {
      kLog.warn("loadfile {} failed: {}", m_path, mpv_error_string(err));
      return false;
    }
    kLog.info("playing {}", m_path);
    return true;
  }

  TextureHandle texture() const noexcept override {
    if (!m_hasFrame) {
      return {};
    }
    return TextureHandle{
        .id = TextureId{m_texture},
        .width = m_width,
        .height = m_height,
        .sourceWidth = m_width,
        .sourceHeight = m_height,
        .opaque = true,
    };
  }

  const std::string& path() const noexcept override { return m_path; }

  void dispatch() {
    handleEvents();
    if ((mpv_render_context_update(m_render) & MPV_RENDER_UPDATE_FRAME) == 0) {
      return;
    }
    if (render()) {
      m_frameReady.emit();
    }
  }

private:
  // Called on mpv threads; only wakes the main loop.
  static void wake(void* ctx) {
    const std::uint64_t one = 1;
    (void)::write(static_cast<Stream*>(ctx)->m_wakeFd, &one, sizeof(one));
  }

  void handleEvents() {
    while (true) {
      mpv_event* event = mpv_wait_event(m_mpv, 0);
      if (event->event_id == MPV_EVENT_NONE) {
        return;
      }
      switch (event->event_id) {
      case MPV_EVENT_LOG_MESSAGE: {
        const auto* msg = static_cast<mpv_event_log_message*>(event->data);
        std::string_view text = msg->text;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
          text.remove_suffix(1);
        }
        kLog.warn("mpv [{}] {}", msg->prefix, text);
        break;
      }
      case MPV_EVENT_VIDEO_RECONFIG: {
        std::int64_t width = 0;
        std::int64_t height = 0;
        mpv_get_property(m_mpv, "dwidth", MPV_FORMAT_INT64, &width);
        mpv_get_property(m_mpv, "dheight", MPV_FORMAT_INT64, &height);
        ensureTarget(static_cast<int>(width), static_cast<int>(height));
        break;
      }
      case MPV_EVENT_END_FILE: {
        const auto* end = static_cast<mpv_event_end_file*>(event->data);
        if (end->reason == MPV_END_FILE_REASON_ERROR) {
          kLog.warn("failed to play {}: {}", m_path, mpv_error_string(end->error));
        }
        break;
      }
      default:
        break;
      }
    }
  }

  bool render() {
    ScopedEglContext current(m_eglDisplay, m_context);
    if (!current.ok()) {
      return false;
    }
    if (m_fbo == 0) {
      // No target until the first reconfig; consume the frame so playback keeps advancing.
      int skip = 1;
      std::array<mpv_render_param, 2> params = {{
          {MPV_RENDER_PARAM_SKIP_RENDERING, &skip},
          {MPV_RENDER_PARAM_INVALID, nullptr},
      }};
      mpv_render_context_render(m_render, params.data());
      return false;
    }

    mpv_opengl_fbo fbo{.fbo = static_cast<int>(m_fbo), .w = m_width, .h = m_height, .internal_format = 0};
    int flipY = 0;
    int block = 0;
    std::array<mpv_render_param, 4> params = {{
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    }};
    mpv_render_context_render(m_render, params.data());
    // The texture is sampled from other contexts in the share group; finishing here is the
    // simplest way to guarantee they see the completed frame.
    glFinish();
    m_hasFrame = true;
    return true;
  }

  void ensureTarget(int width, int height) {
    if (width <= 0 || height <= 0 || (width == m_width && height == m_height && m_fbo != 0)) {
      return;
    }
    ScopedEglContext current(m_eglDisplay, m_context);
    if (!current.ok()) {
      return;
    }
    destroyTarget();

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!complete) {
      kLog.warn("video framebuffer {}x{} incomplete", width, height);
      destroyTarget();
      return;
    }
    m_width = width;
    m_height = height;
  }

  // Requires the stream's context to be current.
  void destroyTarget() {
    if (m_fbo != 0) {
      glDeleteFramebuffers(1, &m_fbo);
      m_fbo = 0;
    }
    if (m_texture != 0) {
      glDeleteTextures(1, &m_texture);
      m_texture = 0;
    }
    m_width = 0;
    m_height = 0;
    m_hasFrame = false;
  }

  std::string m_path;
  int m_wakeFd = -1;
  mpv_handle* m_mpv = nullptr;
  mpv_render_context* m_render = nullptr;
  EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
  EGLContext m_context = EGL_NO_CONTEXT;
  GLuint m_fbo = 0;
  GLuint m_texture = 0;
  int m_width = 0;
  int m_height = 0;
  bool m_hasFrame = false;
};

#else

class VideoService::Stream final : public VideoStream {
public:
  TextureHandle texture() const noexcept override { return {}; }
  const std::string& path() const noexcept override { return m_path; }
  void dispatch() {}

private:
  std::string m_path;
};

#endif

VideoService* VideoService::s_instance = nullptr;

VideoService::VideoService() : m_wakeFd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) { s_instance = this; }

VideoService::~VideoService() {
  if (s_instance == this) {
    s_instance = nullptr;
  }
  if (m_wakeFd >= 0) {
    ::close(m_wakeFd);
  }
}

VideoService* VideoService::current() noexcept { return s_instance; }

void VideoService::initialize(wl_display* display, GlSharedContext* shared) {
  m_display = display;
  m_shared = shared;
}

bool VideoService::supported() noexcept { return NOCTALIA_HAVE_VIDEO != 0; }

std::vector<std::string> VideoService::fileExtensionFilter() {
  // Anything FFmpeg can demux plays; this only scopes file pickers to likely candidates.
  return {".mp4", ".m4v", ".mkv",  ".webm", ".mov",  ".avi", ".wmv", ".flv",
          ".ogv", ".mpg", ".mpeg", ".ts",   ".m2ts", ".3gp", ".gif"};
}

std::shared_ptr<VideoStream> VideoService::acquire(const std::string& path) {
  if (auto it = m_streams.find(path); it != m_streams.end()) {
    if (auto existing = it->second.lock()) {
      return existing;
    }
  }
#if NOCTALIA_HAVE_VIDEO
  if (m_shared == nullptr || !m_shared->hasSharedContext() || m_wakeFd < 0) {
    kLog.warn("video playback needs shell.shared_gl_context enabled; not playing {}", path);
    return nullptr;
  }
  auto stream = std::make_shared<Stream>(path, m_wakeFd);
  if (!stream->start(m_display, *m_shared)) {
    return nullptr;
  }
  m_streams[path] = stream;
  return stream;
#else
  kLog.warn("built without video support (-Dvideo); not playing {}", path);
  return nullptr;
#endif
}

void VideoService::doAddPollFds(std::vector<pollfd>& fds) {
  fds.push_back({.fd = m_wakeFd, .events = POLLIN, .revents = 0});
}

void VideoService::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (startIdx >= fds.size() || (fds[startIdx].revents & POLLIN) == 0) {
    return;
  }
  std::uint64_t count = 0;
  (void)::read(m_wakeFd, &count, sizeof(count));

  // Snapshot first: frame handlers may acquire or release streams.
  std::vector<std::shared_ptr<Stream>> live;
  std::erase_if(m_streams, [&live](const auto& entry) {
    auto stream = entry.second.lock();
    if (stream == nullptr) {
      return true;
    }
    live.push_back(std::move(stream));
    return false;
  });
  for (const auto& stream : live) {
    stream->dispatch();
  }
}

bool VideoHandle::open(const std::string& path, FrameCallback onFrame) {
  if (m_stream != nullptr && m_stream->path() == path) {
    m_frameConn = m_stream->frameReady().connect(std::move(onFrame));
    return true;
  }
  reset();
  VideoService* service = VideoService::current();
  if (service == nullptr) {
    kLog.warn("video service unavailable; not playing {}", path);
    return false;
  }
  m_stream = service->acquire(path);
  if (m_stream == nullptr) {
    return false;
  }
  m_frameConn = m_stream->frameReady().connect(std::move(onFrame));
  return true;
}

void VideoHandle::reset() noexcept {
  m_frameConn.disconnect();
  m_stream.reset();
}

TextureHandle VideoHandle::texture() const noexcept {
  return m_stream != nullptr ? m_stream->texture() : TextureHandle{};
}

const std::string& VideoHandle::path() const noexcept {
  static const std::string kEmpty;
  return m_stream != nullptr ? m_stream->path() : kEmpty;
}
