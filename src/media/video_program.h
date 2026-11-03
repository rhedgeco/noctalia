#pragma once

#include "media/video_frame.h"
#include "render/backend/render_backend.h"
#include "render/backend/render_extension.h"
#include "render/core/mat3.h"
#include "render/core/shader_program.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <cstdint>
#include <vector>

// Draws dmabuf video frames with no CPU copy: each frame's planes are imported into the backend's
// own context as an EGLImage bound to a GL_TEXTURE_EXTERNAL_OES texture, and the driver converts
// YUV to RGB while sampling. One import is cached per owner (a video view) and replaced when the
// owner's frame changes.
class VideoProgram final : public RenderExtension {
public:
  struct DrawParams {
    float surfaceWidth = 0.0F;
    float surfaceHeight = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    float opacity = 1.0F;
    float radius = 0.0F;
    RenderImageFitMode fitMode = RenderImageFitMode::Contain;
    Mat3 transform = Mat3::identity();
  };

  VideoProgram();
  ~VideoProgram() override;

  // Owners identify the imports of one view across every backend. Release an owner when its view
  // goes away so each program drops the frame (and its decoder buffer) without waiting for a draw.
  static std::uint64_t createOwner() noexcept;
  static void releaseOwner(std::uint64_t owner) noexcept;

  // Imports (if needed) and draws `frame`. Returns false when this context cannot import it; the
  // caller should then switch the decoder to system memory frames.
  bool draw(std::uint64_t owner, const VideoFramePtr& frame, const DrawParams& params);

  void destroy() override;
  void abandon() noexcept override;

private:
  struct Import {
    std::uint64_t owner = 0;
    VideoFramePtr frame;
    GLuint texture = 0;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
  };

  bool ensureInitialized();
  bool importFrame(Import& import, const VideoFramePtr& frame);
  void releaseImage(Import& import) noexcept;
  void forget(std::uint64_t owner) noexcept;
  void deletePendingTextures();

  ShaderProgram m_program;
  GLint m_positionLoc = -1;
  GLint m_texCoordLoc = -1;
  GLint m_surfaceSizeLoc = -1;
  GLint m_sizeLoc = -1;
  GLint m_texSizeLoc = -1;
  GLint m_fitModeLoc = -1;
  GLint m_transformLoc = -1;
  GLint m_samplerLoc = -1;
  GLint m_opacityLoc = -1;
  GLint m_radiusLoc = -1;

  bool m_initialized = false;
  bool m_supported = false;
  bool m_modifiersSupported = false;
  bool m_importFailureLogged = false;
  EGLDisplay m_display = EGL_NO_DISPLAY;
  PFNEGLCREATEIMAGEKHRPROC m_createImage = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC m_destroyImage = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_imageTargetTexture = nullptr;

  std::vector<Import> m_imports;
  // Textures of released owners, deleted on the next call made with the context current.
  std::vector<GLuint> m_pendingDelete;
};
