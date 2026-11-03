#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

// One decoded video frame. Holds the decoder's buffer (and, for DmaBuf, its file descriptors)
// alive until the last reference is dropped, so whoever displays the frame keeps a copy of the
// pointer for as long as the GPU may read from it.
struct VideoFrame {
  enum class Kind : std::uint8_t {
    // Planes are dmabufs the shell's own GL context imports through EGL (zero copy).
    DmaBuf,
    // Tightly packed RGBA8 pixels in system memory, uploaded with TextureManager.
    Rgba,
  };
  enum class YuvMatrix : std::uint8_t { Bt601, Bt709, Bt2020 };

  struct Plane {
    int fd = -1;
    std::uint32_t offset = 0;
    std::uint32_t pitch = 0;
  };

  static constexpr std::size_t kMaxPlanes = 4;
  // DRM_FORMAT_MOD_INVALID: the dmabuf carries no explicit modifier.
  static constexpr std::uint64_t kModifierInvalid = 0x00ffffffffffffffULL;

  Kind kind = Kind::Rgba;
  int width = 0;
  int height = 0;
  // Increases with every frame a decoder produces, so consumers can tell frames apart cheaply.
  std::uint64_t serial = 0;

  // DmaBuf only.
  std::uint32_t drmFourcc = 0;
  std::uint64_t drmModifier = kModifierInvalid;
  std::size_t planeCount = 0;
  std::array<Plane, kMaxPlanes> planes{};
  YuvMatrix matrix = YuvMatrix::Bt709;
  bool fullRange = false;

  // Rgba only; width * 4 bytes per row.
  const std::uint8_t* pixels = nullptr;

  // Owns the decoder buffer backing the fields above.
  std::shared_ptr<void> storage;
};

using VideoFramePtr = std::shared_ptr<const VideoFrame>;
