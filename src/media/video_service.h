#pragma once

#include "core/poll_source.h"

#include <vector>

class VideoDecoder;

// Polls the wakeup and bus fds of every live VideoDecoder so pipeline events and new frames are
// handled on the main thread. Decoders register while their owning control exists.
class VideoService final : public PollSource {
public:
  VideoService();
  ~VideoService() override;
  VideoService(const VideoService&) = delete;
  VideoService& operator=(const VideoService&) = delete;

  // Null before the application creates the service and after it is destroyed.
  static VideoService* current() noexcept;

  void add(VideoDecoder& decoder);
  void remove(VideoDecoder& decoder);

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  static VideoService* s_instance;

  std::vector<VideoDecoder*> m_decoders;
  // Owner of each fd appended by the last doAddPollFds, in fd order. A decoder removed before
  // dispatch is nulled rather than erased so the indices stay aligned.
  std::vector<VideoDecoder*> m_polled;
};
