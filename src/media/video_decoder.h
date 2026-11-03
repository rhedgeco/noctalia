#pragma once

#include "media/video_frame.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// Plays one video file through GStreamer and exposes its newest frame. The pipeline's streaming
// threads only signal an eventfd; bus handling, frame pulls, looping, and pipeline rebuilds all
// happen on the main thread in dispatch(), driven by VideoService through the shell poll loop.
//
// Zero copy first: hardware (VA-API) decoders hand out dmabufs that the renderer imports into its
// own GL context, so no decoded pixel is touched by the CPU. VA drivers that only advertise system
// memory (nvidia-vaapi-driver) still decode into VA surfaces, which are exported as dmabufs
// directly. When none of that is available (no VA decoder, unsupported format, or the renderer
// rejects the import) the decoder rebuilds itself to produce RGBA in system memory instead.
//
// GStreamer is initialized on a worker thread the first time a decoder is given a path; playback
// starts once that finishes.
class VideoDecoder {
public:
  using ChangeCallback = std::function<void()>;

  VideoDecoder();
  ~VideoDecoder();
  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;

  // Called on the main thread after a new frame arrived or playback stopped.
  void setChangeCallback(ChangeCallback callback) { m_onChange = std::move(callback); }

  // Returns false when the path is unchanged. A new path rebuilds the pipeline, trying zero copy
  // again. An empty path stops playback.
  bool setPath(std::string path);
  const std::string& path() const noexcept { return m_path; }

  // The newest decoded frame, or null before the first frame and after a failure.
  VideoFramePtr frame() const noexcept { return m_frame; }

  // The renderer could not import a dmabuf frame. Schedules a rebuild that decodes to RGBA in
  // system memory. Safe to call while rendering: the rebuild runs on the next dispatch().
  void requestCpuFallback();

  // Poll integration (see VideoService). Up to two fds: the frame wakeup and the pipeline bus.
  int wakeFd() const noexcept;
  int busFd() const noexcept;
  void dispatch();

private:
  struct Pipeline;

  void rebuild(bool zeroCopy);
  void stop();
  void fail(std::string_view reason);
  void handleBus();
  void pullFrames();
  void notify();

  std::string m_path;
  std::unique_ptr<Pipeline> m_pipeline;
  VideoFramePtr m_frame;
  std::uint64_t m_serial = 0;
  int m_wakeFd = -1;
  bool m_fallbackPending = false;
  bool m_waitingForInit = false;
  ChangeCallback m_onChange;
};
