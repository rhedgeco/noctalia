#include "scripting/ui/video_view.h"

#include "media/video_program.h"
#include "media/video_service.h"
#include "render/backend/render_backend.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"

#include <algorithm>

namespace {

  RenderImageFitMode renderFitMode(ImageFit fit) {
    switch (fit) {
    case ImageFit::Cover:
      return RenderImageFitMode::Cover;
    case ImageFit::Stretch:
      return RenderImageFitMode::Stretch;
    case ImageFit::Contain:
      break;
    }
    return RenderImageFitMode::Contain;
  }

} // namespace

VideoView::VideoView() : m_decoder(std::make_unique<VideoDecoder>()), m_owner(VideoProgram::createOwner()) {
  setClipChildren(true);
  m_decoder->setChangeCallback([this] { onFrameChanged(); });
  m_service = VideoService::current();
  if (m_service != nullptr) {
    m_service->add(*m_decoder);
  }
}

VideoView::~VideoView() {
  m_decoder->setChangeCallback({});
  if (m_service != nullptr && VideoService::current() == m_service) {
    m_service->remove(*m_decoder);
  }
  VideoProgram::releaseOwner(m_owner);
  dropTexture();
}

void VideoView::setPath(const std::string& path) {
  if (m_decoder->setPath(path)) {
    markLayoutDirty();
  }
}

void VideoView::setRadius(float radius) {
  radius = std::max(0.0F, radius);
  if (radius != m_radius) {
    m_radius = radius;
    markPaintDirty();
  }
}

void VideoView::setFit(ImageFit fit) {
  if (fit != m_fit) {
    m_fit = fit;
    markPaintDirty();
  }
}

void VideoView::onFrameChanged() {
  const VideoFramePtr frame = m_decoder->frame();
  if (frame != nullptr && frame->kind == VideoFrame::Kind::DmaBuf) {
    // Drawn straight from the decoder's buffer; nothing to upload.
    markPaintDirty();
    return;
  }
  // Playback stopped or switched to system memory: let go of any imported decoder buffer.
  VideoProgram::releaseOwner(m_owner);
  markLayoutDirty();
}

void VideoView::dropTexture() {
  if (m_renderer != nullptr && m_texture.valid()) {
    m_renderer->textureManager().unload(m_texture);
  }
  m_texture = {};
  m_uploadedSerial = 0;
}

void VideoView::doLayout(Renderer& renderer) {
  m_renderer = &renderer;
  const VideoFramePtr frame = m_decoder->frame();
  if (frame == nullptr || frame->kind != VideoFrame::Kind::Rgba) {
    if (m_texture.valid()) {
      dropTexture();
      markPaintDirty();
    }
  } else if (frame->serial != m_uploadedSerial) {
    auto& textures = renderer.textureManager();
    const bool sameSize = m_texture.valid() && m_texture.width == frame->width && m_texture.height == frame->height;
    const bool uploaded = (sameSize
                           && textures.updateSubImage(
                               m_texture, frame->pixels, 0, 0, frame->width, frame->height, TextureDataFormat::Rgba
                           ))
        || textures.replace(m_texture, frame->pixels, frame->width, frame->height, TextureDataFormat::Rgba);
    if (uploaded) {
      m_uploadedSerial = frame->serial;
      markPaintDirty();
    }
  }
  CustomDrawNode::doLayout(renderer);
}

void VideoView::doInvalidateGpuResources(Renderer& renderer) {
  m_renderer = &renderer;
  // The texture died with the context. Dmabuf imports are redone by VideoProgram on the next
  // draw; the current system memory frame is still held by the decoder and re-uploads in layout.
  m_texture = {};
  m_uploadedSerial = 0;
  markLayoutDirty();
}

void VideoView::draw(RenderBackend& backend, const CustomDrawContext& context) const {
  const VideoFramePtr frame = m_decoder->frame();
  if (frame == nullptr || context.width <= 0.0F || context.height <= 0.0F) {
    return;
  }
  if (frame->kind == VideoFrame::Kind::DmaBuf) {
    auto& program = backend.extension<VideoProgram>();
    const bool drawn = program.draw(
        m_owner, frame,
        VideoProgram::DrawParams{
            .surfaceWidth = context.surfaceWidth,
            .surfaceHeight = context.surfaceHeight,
            .width = context.width,
            .height = context.height,
            .opacity = context.opacity,
            .radius = m_radius,
            .fitMode = renderFitMode(m_fit),
            .transform = context.transform,
        }
    );
    if (!drawn) {
      // This context cannot sample the decoder's buffers; decode to system memory instead.
      m_decoder->requestCpuFallback();
    }
    return;
  }
  if (!m_texture.valid() || m_uploadedSerial == 0) {
    return;
  }
  backend.drawImage(
      RenderImageDraw{
          .texture = m_texture.id,
          .surfaceWidth = context.surfaceWidth,
          .surfaceHeight = context.surfaceHeight,
          .width = context.width,
          .height = context.height,
          .opacity = context.opacity,
          .radius = m_radius,
          .fitMode = renderFitMode(m_fit),
          .textureWidth = static_cast<float>(m_texture.width),
          .textureHeight = static_cast<float>(m_texture.height),
          .transform = context.transform,
      }
  );
}
