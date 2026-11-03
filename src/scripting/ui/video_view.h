#pragma once

#include "media/video_decoder.h"
#include "render/core/texture_handle.h"
#include "render/scene/custom_draw_node.h"
#include "ui/controls/image.h"

#include <cstdint>
#include <memory>
#include <string>

class Renderer;
class VideoService;

// Plays a video file fitted inside its box, looping. Decoding runs in GStreamer and is driven by
// VideoService on the main thread. Hardware-decoded dmabuf frames are drawn straight from the
// decoder's buffers (VideoProgram); system memory frames, the fallback, are uploaded in layout.
class VideoView final : public CustomDrawNode {
public:
  VideoView();
  ~VideoView() override;
  VideoView(const VideoView&) = delete;
  VideoView& operator=(const VideoView&) = delete;

  void setPath(const std::string& path);
  void setRadius(float radius);
  void setFit(ImageFit fit);

  void draw(RenderBackend& backend, const CustomDrawContext& context) const override;

private:
  void onFrameChanged();
  void dropTexture();
  void doLayout(Renderer& renderer) override;
  void doInvalidateGpuResources(Renderer& renderer) override;
  void doRebindRenderer(Renderer& renderer) override { m_renderer = &renderer; }

  std::unique_ptr<VideoDecoder> m_decoder;
  VideoService* m_service = nullptr;
  Renderer* m_renderer = nullptr;
  // Identifies this view's dmabuf imports in every backend's VideoProgram.
  std::uint64_t m_owner = 0;
  ImageFit m_fit = ImageFit::Contain;
  float m_radius = 0.0F;
  // System memory frames only.
  TextureHandle m_texture;
  std::uint64_t m_uploadedSerial = 0;
};
