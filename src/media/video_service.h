#pragma once

#include "core/poll_source.h"
#include "render/core/texture_handle.h"
#include "ui/signal.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class GlSharedContext;
struct wl_display;

// One playing video file, decoded and rendered by libmpv into a GL texture in the shell's shared
// context group, so any render context in the shell can sample it. Playback is silent and loops.
// Every consumer of the same path shares one stream; it stops when the last reference is dropped.
class VideoStream {
public:
  virtual ~VideoStream() = default;

  // Invalid until the first frame has been rendered.
  virtual TextureHandle texture() const noexcept = 0;
  virtual const std::string& path() const noexcept = 0;

  // Emitted on the main thread after a new frame was rendered into texture(). The texture id
  // changes when the video's dimensions change, so consumers re-read texture() on every emit.
  Signal<>& frameReady() noexcept { return m_frameReady; }

protected:
  Signal<> m_frameReady;
};

// A consumer's hold on a stream plus its frame subscription. Both end when the handle is reset
// or destroyed, so a consumer only has to keep one of these alive while it shows the video.
class VideoHandle {
public:
  using FrameCallback = std::function<void()>;

  VideoHandle() = default;
  VideoHandle(VideoHandle&&) noexcept = default;
  VideoHandle& operator=(VideoHandle&&) noexcept = default;
  VideoHandle(const VideoHandle&) = delete;
  VideoHandle& operator=(const VideoHandle&) = delete;

  // Starts (or joins) playback of path through VideoService::current(). onFrame runs on the main
  // thread for every new frame. Returns false and leaves the handle empty when playback could
  // not start; the previous stream is released either way.
  bool open(const std::string& path, FrameCallback onFrame);
  void reset() noexcept;

  bool active() const noexcept { return m_stream != nullptr; }
  // Invalid while inactive and before the first frame.
  TextureHandle texture() const noexcept;
  const std::string& path() const noexcept;

private:
  std::shared_ptr<VideoStream> m_stream;
  Signal<>::ScopedConnection m_frameConn;
};

// Owns libmpv playback for the shell. mpv's own threads only signal an eventfd; event handling
// and frame rendering run on the main thread from dispatch().
class VideoService final : public PollSource {
public:
  VideoService();
  ~VideoService() override;

  VideoService(const VideoService&) = delete;
  VideoService& operator=(const VideoService&) = delete;

  // Null before the application creates the service and after it is destroyed.
  static VideoService* current() noexcept;

  // shared must have a shared root context: frames are rendered in a stream-owned context and
  // sampled from the shell's render contexts.
  void initialize(wl_display* display, GlSharedContext* shared);

  // False when built without libmpv (-Dvideo=disabled).
  static bool supported() noexcept;
  // Common video containers and animated images (lowercase, with dot), for file pickers.
  static std::vector<std::string> fileExtensionFilter();

  // Returns the existing stream for path or starts a new one. Null when unsupported or when
  // playback could not be set up. Prefer VideoHandle in consumers.
  std::shared_ptr<VideoStream> acquire(const std::string& path);

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  class Stream;

  static VideoService* s_instance;

  wl_display* m_display = nullptr;
  GlSharedContext* m_shared = nullptr;
  int m_wakeFd = -1;
  std::unordered_map<std::string, std::weak_ptr<Stream>> m_streams;
};
