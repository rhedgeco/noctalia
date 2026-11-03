#include "media/video_program.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("video");

  constexpr char kVertexShaderSource[] = R"(
precision highp float;

attribute vec2 a_position;
attribute vec2 a_texcoord;
uniform vec2 u_surface_size;
uniform vec2 u_size;
uniform vec2 u_tex_size;
uniform int u_fit_mode;
uniform mat3 u_transform;
varying vec2 v_texcoord;
varying vec2 v_local;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    vec2 local = a_position * u_size;
    vec3 pixel = u_transform * vec3(local, 1.0);

    vec2 scale = vec2(1.0);
    if (u_fit_mode != 0 && u_tex_size.x > 0.0 && u_tex_size.y > 0.0 && u_size.x > 0.0 && u_size.y > 0.0) {
        float rect_aspect = u_size.x / u_size.y;
        float tex_aspect = u_tex_size.x / u_tex_size.y;
        if (u_fit_mode == 1) { // Cover
            if (tex_aspect > rect_aspect) {
                scale.x = rect_aspect / tex_aspect;
            } else {
                scale.y = tex_aspect / rect_aspect;
            }
        } else { // Contain
            if (tex_aspect > rect_aspect) {
                scale.y = tex_aspect / rect_aspect;
            } else {
                scale.x = rect_aspect / tex_aspect;
            }
        }
    }
    vec2 offset = (vec2(1.0) - scale) * 0.5;
    v_texcoord = offset + a_texcoord * scale;
    v_local = local;
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  constexpr char kFragmentShaderSource[] = R"(#extension GL_OES_EGL_image_external : require
precision highp float;

uniform samplerExternalOES u_texture;
uniform float u_opacity;
uniform vec2 u_size;
uniform float u_radius;
varying vec2 v_texcoord;
varying vec2 v_local;

float rounded_rect_distance(vec2 centered, vec2 half_size, float radius) {
    vec2 q = abs(centered) - (half_size - vec2(radius));
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

void main() {
    float aa = 0.85;
    float radius = min(u_radius, min(u_size.x, u_size.y) * 0.5);
    vec2 centered = v_local - u_size * 0.5;
    float coverage = 1.0 - smoothstep(-aa, aa, rounded_rect_distance(centered, u_size * 0.5, radius));
    // Contain leaves the letterbox transparent.
    if (coverage <= 0.0 || v_texcoord.x < 0.0 || v_texcoord.x > 1.0 || v_texcoord.y < 0.0 || v_texcoord.y > 1.0) {
        discard;
    }
    vec4 texel = texture2D(u_texture, v_texcoord);
    float a = texel.a * u_opacity * coverage;
    gl_FragColor = vec4(texel.rgb * a, a);
}
)";

  constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(a)
        | (static_cast<std::uint32_t>(b) << 8U)
        | (static_cast<std::uint32_t>(c) << 16U)
        | (static_cast<std::uint32_t>(d) << 24U);
  }

  // YUV formats hardware decoders emit. Color space and range hints only apply to these.
  bool isYuvFourcc(std::uint32_t format) {
    static constexpr std::array kYuv = {
        fourcc('N', 'V', '1', '2'), fourcc('N', 'V', '2', '1'), fourcc('N', 'V', '1', '6'), fourcc('N', 'V', '6', '1'),
        fourcc('N', 'V', '2', '4'), fourcc('N', 'V', '4', '2'), fourcc('N', 'V', '1', '5'), fourcc('N', 'V', '2', '0'),
        fourcc('P', '0', '1', '0'), fourcc('P', '0', '1', '2'), fourcc('P', '0', '1', '6'), fourcc('P', '2', '1', '0'),
        fourcc('Y', 'U', '1', '2'), fourcc('Y', 'V', '1', '2'), fourcc('Y', 'U', '1', '6'), fourcc('Y', 'V', '1', '6'),
        fourcc('Y', 'U', '2', '4'), fourcc('Y', 'V', '2', '4'), fourcc('Y', 'U', 'Y', 'V'), fourcc('Y', 'V', 'Y', 'U'),
        fourcc('U', 'Y', 'V', 'Y'), fourcc('V', 'Y', 'U', 'Y'), fourcc('A', 'Y', 'U', 'V'), fourcc('X', 'Y', 'U', 'V'),
        fourcc('Y', '2', '1', '0'), fourcc('Y', '2', '1', '2'), fourcc('Y', '2', '1', '6'), fourcc('Y', '4', '1', '0'),
        fourcc('Y', '4', '1', '2'), fourcc('Y', '4', '1', '6'),
    };
    return std::ranges::contains(kYuv, format);
  }

  std::string fourccName(std::uint32_t format) {
    std::string name(4, ' ');
    for (std::size_t i = 0; i < 4; ++i) {
      const auto c = static_cast<char>((format >> (8U * i)) & 0xffU);
      name[i] = c >= 0x20 && c < 0x7f ? c : '?';
    }
    return name;
  }

  bool hasExtension(const char* list, std::string_view name) {
    if (list == nullptr) {
      return false;
    }
    std::string_view rest(list);
    while (!rest.empty()) {
      const auto end = rest.find(' ');
      if (rest.substr(0, end) == name) {
        return true;
      }
      if (end == std::string_view::npos) {
        break;
      }
      rest.remove_prefix(end + 1);
    }
    return false;
  }

  std::vector<VideoProgram*>& livePrograms() {
    static std::vector<VideoProgram*> programs;
    return programs;
  }

} // namespace

VideoProgram::VideoProgram() { livePrograms().push_back(this); }

VideoProgram::~VideoProgram() { std::erase(livePrograms(), this); }

std::uint64_t VideoProgram::createOwner() noexcept {
  static std::uint64_t next = 0;
  return ++next;
}

void VideoProgram::releaseOwner(std::uint64_t owner) noexcept {
  for (VideoProgram* program : livePrograms()) {
    program->forget(owner);
  }
}

bool VideoProgram::ensureInitialized() {
  if (m_initialized) {
    return m_supported;
  }
  m_initialized = true;
  m_supported = false;

  m_display = eglGetCurrentDisplay();
  if (m_display == EGL_NO_DISPLAY) {
    return false;
  }
  const char* eglExtensions = eglQueryString(m_display, EGL_EXTENSIONS);
  const auto* glExtensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  if (!hasExtension(eglExtensions, "EGL_EXT_image_dma_buf_import")
      || !hasExtension(eglExtensions, "EGL_KHR_image_base")
      || !hasExtension(glExtensions, "GL_OES_EGL_image_external")) {
    kLog.info("GPU driver cannot import dmabufs into GL; video falls back to system memory frames");
    return false;
  }
  m_modifiersSupported = hasExtension(eglExtensions, "EGL_EXT_image_dma_buf_import_modifiers");
  m_createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  m_destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  m_imageTargetTexture =
      reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if (m_createImage == nullptr || m_destroyImage == nullptr || m_imageTargetTexture == nullptr) {
    kLog.info("EGL image entry points missing; video falls back to system memory frames");
    return false;
  }

  try {
    m_program.create(kVertexShaderSource, kFragmentShaderSource);
  } catch (const std::exception& e) {
    kLog.warn("external video shader unavailable ({}); video falls back to system memory frames", e.what());
    return false;
  }
  const GLuint id = m_program.id();
  m_positionLoc = glGetAttribLocation(id, "a_position");
  m_texCoordLoc = glGetAttribLocation(id, "a_texcoord");
  m_surfaceSizeLoc = glGetUniformLocation(id, "u_surface_size");
  m_sizeLoc = glGetUniformLocation(id, "u_size");
  m_texSizeLoc = glGetUniformLocation(id, "u_tex_size");
  m_fitModeLoc = glGetUniformLocation(id, "u_fit_mode");
  m_transformLoc = glGetUniformLocation(id, "u_transform");
  m_samplerLoc = glGetUniformLocation(id, "u_texture");
  m_opacityLoc = glGetUniformLocation(id, "u_opacity");
  m_radiusLoc = glGetUniformLocation(id, "u_radius");
  if (m_positionLoc < 0
      || m_texCoordLoc < 0
      || m_surfaceSizeLoc < 0
      || m_sizeLoc < 0
      || m_texSizeLoc < 0
      || m_fitModeLoc < 0
      || m_transformLoc < 0
      || m_samplerLoc < 0
      || m_opacityLoc < 0
      || m_radiusLoc < 0) {
    kLog.warn("failed to query video shader locations; video falls back to system memory frames");
    m_program.destroy();
    return false;
  }

  m_supported = true;
  return true;
}

bool VideoProgram::importFrame(Import& import, const VideoFramePtr& frame) {
  const bool explicitModifier = frame->drmModifier != VideoFrame::kModifierInvalid;
  // A linear layout imports without the modifiers extension; anything else needs it.
  if (explicitModifier && frame->drmModifier != 0 && !m_modifiersSupported) {
    if (!std::exchange(m_importFailureLogged, true)) {
      kLog.info("driver lacks EGL_EXT_image_dma_buf_import_modifiers for a tiled video frame");
    }
    return false;
  }

  static constexpr std::array<EGLint, VideoFrame::kMaxPlanes> kFd = {
      EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT
  };
  static constexpr std::array<EGLint, VideoFrame::kMaxPlanes> kOffset = {
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT,
      EGL_DMA_BUF_PLANE3_OFFSET_EXT
  };
  static constexpr std::array<EGLint, VideoFrame::kMaxPlanes> kPitch = {
      EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
      EGL_DMA_BUF_PLANE3_PITCH_EXT
  };
  static constexpr std::array<EGLint, VideoFrame::kMaxPlanes> kModifierLo = {
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
      EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT
  };
  static constexpr std::array<EGLint, VideoFrame::kMaxPlanes> kModifierHi = {
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
      EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT
  };

  std::vector<EGLint> attribs;
  attribs.reserve(64);
  attribs.insert(
      attribs.end(),
      {EGL_WIDTH, frame->width, EGL_HEIGHT, frame->height, EGL_LINUX_DRM_FOURCC_EXT,
       static_cast<EGLint>(frame->drmFourcc)}
  );
  for (std::size_t i = 0; i < frame->planeCount; ++i) {
    const auto& plane = frame->planes[i];
    attribs.insert(
        attribs.end(),
        {kFd[i], plane.fd, kOffset[i], static_cast<EGLint>(plane.offset), kPitch[i], static_cast<EGLint>(plane.pitch)}
    );
    if (explicitModifier && m_modifiersSupported) {
      attribs.insert(
          attribs.end(),
          {kModifierLo[i], static_cast<EGLint>(frame->drmModifier & 0xffffffffULL), kModifierHi[i],
           static_cast<EGLint>(frame->drmModifier >> 32U)}
      );
    }
  }
  if (isYuvFourcc(frame->drmFourcc)) {
    EGLint colorSpace = EGL_ITU_REC709_EXT;
    switch (frame->matrix) {
    case VideoFrame::YuvMatrix::Bt601:
      colorSpace = EGL_ITU_REC601_EXT;
      break;
    case VideoFrame::YuvMatrix::Bt709:
      colorSpace = EGL_ITU_REC709_EXT;
      break;
    case VideoFrame::YuvMatrix::Bt2020:
      colorSpace = EGL_ITU_REC2020_EXT;
      break;
    }
    attribs.insert(
        attribs.end(),
        {EGL_YUV_COLOR_SPACE_HINT_EXT, colorSpace, EGL_SAMPLE_RANGE_HINT_EXT,
         frame->fullRange ? EGL_YUV_FULL_RANGE_EXT : EGL_YUV_NARROW_RANGE_EXT}
    );
  }
  attribs.push_back(EGL_NONE);

  EGLImageKHR image = m_createImage(m_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs.data());
  if (image == EGL_NO_IMAGE_KHR) {
    if (!std::exchange(m_importFailureLogged, true)) {
      kLog.info(
          "cannot import {}x{} {} (modifier {:#x}) video frame: EGL error {:#x}", frame->width, frame->height,
          fourccName(frame->drmFourcc), frame->drmModifier, static_cast<unsigned>(eglGetError())
      );
    }
    return false;
  }

  // Bounded: after a context loss GetError keeps reporting without other calls clearing it.
  for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {
  }
  if (import.texture == 0) {
    glGenTextures(1, &import.texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, import.texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, import.texture);
  }
  m_imageTargetTexture(GL_TEXTURE_EXTERNAL_OES, static_cast<GLeglImageOES>(image));
  const GLenum error = glGetError();
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
  if (error != GL_NO_ERROR) {
    m_destroyImage(m_display, image);
    if (!std::exchange(m_importFailureLogged, true)) {
      kLog.info(
          "cannot bind {} video frame to an external texture: GL error {:#x}", fourccName(frame->drmFourcc),
          static_cast<unsigned>(error)
      );
    }
    return false;
  }

  // The texture now holds the new image; the previous one (and its decoder buffer) can go.
  releaseImage(import);
  import.image = image;
  import.frame = frame;
  return true;
}

bool VideoProgram::draw(std::uint64_t owner, const VideoFramePtr& frame, const DrawParams& params) {
  if (frame == nullptr || frame->kind != VideoFrame::Kind::DmaBuf || !ensureInitialized()) {
    return false;
  }
  deletePendingTextures();

  auto it = std::ranges::find(m_imports, owner, &Import::owner);
  if (it == m_imports.end()) {
    m_imports.push_back(Import{.owner = owner});
    it = std::prev(m_imports.end());
  }
  if (it->frame != frame && !importFrame(*it, frame)) {
    return false;
  }
  if (params.width <= 0.0F || params.height <= 0.0F || params.surfaceWidth <= 0.0F || params.surfaceHeight <= 0.0F) {
    return true;
  }

  static constexpr std::array<GLfloat, 12> kQuad = {
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F,
  };

  glUseProgram(m_program.id());
  glUniform2f(m_surfaceSizeLoc, params.surfaceWidth, params.surfaceHeight);
  glUniform2f(m_sizeLoc, params.width, params.height);
  glUniform2f(m_texSizeLoc, static_cast<float>(frame->width), static_cast<float>(frame->height));
  glUniform1i(m_fitModeLoc, static_cast<GLint>(params.fitMode));
  glUniformMatrix3fv(m_transformLoc, 1, GL_FALSE, params.transform.m.data());
  glUniform1f(m_opacityLoc, params.opacity);
  glUniform1f(m_radiusLoc, std::max(0.0F, params.radius));
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, it->texture);
  glUniform1i(m_samplerLoc, 0);
  const auto posAttr = static_cast<GLuint>(m_positionLoc);
  const auto texAttr = static_cast<GLuint>(m_texCoordLoc);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, kQuad.data());
  glVertexAttribPointer(texAttr, 2, GL_FLOAT, GL_FALSE, 0, kQuad.data());
  glEnableVertexAttribArray(posAttr);
  glEnableVertexAttribArray(texAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
  glDisableVertexAttribArray(texAttr);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
  return true;
}

void VideoProgram::releaseImage(Import& import) noexcept {
  if (import.image != EGL_NO_IMAGE_KHR && m_destroyImage != nullptr) {
    m_destroyImage(m_display, import.image);
  }
  import.image = EGL_NO_IMAGE_KHR;
  import.frame.reset();
}

void VideoProgram::forget(std::uint64_t owner) noexcept {
  const auto it = std::ranges::find(m_imports, owner, &Import::owner);
  if (it == m_imports.end()) {
    return;
  }
  // EGLImages belong to the display, so they (and the decoder buffers) go now. Deleting the
  // texture needs this backend's context, which may not be current here.
  releaseImage(*it);
  if (it->texture != 0) {
    m_pendingDelete.push_back(it->texture);
  }
  m_imports.erase(it);
}

void VideoProgram::deletePendingTextures() {
  if (!m_pendingDelete.empty()) {
    glDeleteTextures(static_cast<GLsizei>(m_pendingDelete.size()), m_pendingDelete.data());
    m_pendingDelete.clear();
  }
}

void VideoProgram::destroy() {
  for (Import& import : m_imports) {
    releaseImage(import);
    if (import.texture != 0) {
      m_pendingDelete.push_back(import.texture);
    }
  }
  m_imports.clear();
  deletePendingTextures();
  m_program.destroy();
  m_initialized = false;
  m_supported = false;
}

void VideoProgram::abandon() noexcept {
  for (Import& import : m_imports) {
    releaseImage(import);
  }
  m_imports.clear();
  m_pendingDelete.clear();
  m_program.abandon();
  m_initialized = false;
  m_supported = false;
}
