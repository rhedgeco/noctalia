#include "media/video_service.h"

#include "media/video_decoder.h"

#include <algorithm>

VideoService* VideoService::s_instance = nullptr;

VideoService::VideoService() { s_instance = this; }

VideoService::~VideoService() {
  if (s_instance == this) {
    s_instance = nullptr;
  }
}

VideoService* VideoService::current() noexcept { return s_instance; }

void VideoService::add(VideoDecoder& decoder) {
  if (!std::ranges::contains(m_decoders, &decoder)) {
    m_decoders.push_back(&decoder);
  }
}

void VideoService::remove(VideoDecoder& decoder) {
  std::erase(m_decoders, &decoder);
  std::ranges::replace(m_polled, &decoder, nullptr);
}

void VideoService::doAddPollFds(std::vector<pollfd>& fds) {
  m_polled.clear();
  for (VideoDecoder* decoder : m_decoders) {
    for (const int fd : {decoder->wakeFd(), decoder->busFd()}) {
      if (fd >= 0) {
        fds.push_back({.fd = fd, .events = POLLIN, .revents = 0});
        m_polled.push_back(decoder);
      }
    }
  }
}

void VideoService::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  // A decoder owns up to two consecutive fds but drains both in one dispatch() call. Dispatch can
  // rebuild the pipeline (replacing its bus fd) or let a callback remove a decoder, so each step
  // re-checks registration instead of trusting the snapshot.
  std::vector<VideoDecoder*> ready;
  for (std::size_t i = 0; i < m_polled.size() && startIdx + i < fds.size(); ++i) {
    VideoDecoder* decoder = m_polled[i];
    if (decoder != nullptr && fds[startIdx + i].revents != 0 && !std::ranges::contains(ready, decoder)) {
      ready.push_back(decoder);
    }
  }
  m_polled.clear();

  for (VideoDecoder* decoder : ready) {
    if (std::ranges::contains(m_decoders, decoder)) {
      decoder->dispatch();
    }
  }
}
