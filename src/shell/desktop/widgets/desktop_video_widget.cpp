#include "shell/desktop/widgets/desktop_video_widget.h"

#include "render/core/renderer.h"
#include "scripting/ui/video_view.h"
#include "ui/builders.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

namespace {

  constexpr float kMinNaturalSize = 16.0F;

  float numericValue(const WidgetSettingValue& value, float fallback) {
    if (const auto* v = std::get_if<double>(&value)) {
      return static_cast<float>(*v);
    }
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
      return static_cast<float>(*v);
    }
    return fallback;
  }

} // namespace

DesktopVideoWidget::DesktopVideoWidget(Options options) : m_options(std::move(options)) {
  m_options.opacity = std::clamp(m_options.opacity, 0.0F, 1.0F);
  m_options.radius = std::max(0.0F, m_options.radius);
  m_options.width = std::max(kMinNaturalSize, m_options.width);
  m_options.height = std::max(kMinNaturalSize, m_options.height);
}

ImageFit DesktopVideoWidget::fitFromString(std::string_view value) {
  if (value == "contain") {
    return ImageFit::Contain;
  }
  if (value == "stretch") {
    return ImageFit::Stretch;
  }
  return ImageFit::Cover;
}

std::vector<std::string> DesktopVideoWidget::fileExtensions() {
  return {".mp4", ".m4v", ".mkv", ".webm", ".mov", ".avi", ".ogv"};
}

void DesktopVideoWidget::create() {
  auto rootNode = ui::node({});
  rootNode->setOpacity(m_options.opacity);

  auto video = std::make_unique<VideoView>();
  video->setFit(m_options.fit);
  video->setPath(m_options.path);
  m_video = video.get();
  rootNode->addChild(std::move(video));
  setRoot(std::move(rootNode));
}

bool DesktopVideoWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (m_video == nullptr) {
    return DesktopWidget::applySetting(key, value, allSettings, renderer);
  }

  if (key == "video_path") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_options.path = *v;
      m_video->setPath(m_options.path);
      return true;
    }
    return false;
  }
  if (key == "fit") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_options.fit = fitFromString(*v);
      m_video->setFit(m_options.fit);
      return true;
    }
    return false;
  }
  if (key == "radius") {
    m_options.radius = std::max(0.0F, numericValue(value, m_options.radius));
    requestLayout();
    return true;
  }
  if (key == "width" || key == "height") {
    const float size =
        std::max(kMinNaturalSize, numericValue(value, key == "width" ? m_options.width : m_options.height));
    (key == "width" ? m_options.width : m_options.height) = size;
    // The box fit tracks the largest natural size seen; a smaller configured size must not be
    // fitted against the stale maximum.
    m_maxNaturalWidth = 0.0F;
    m_maxNaturalHeight = 0.0F;
    requestLayout();
    return true;
  }
  if (key == "opacity") {
    if (const auto* v = std::get_if<double>(&value)) {
      m_options.opacity = std::clamp(static_cast<float>(*v), 0.0F, 1.0F);
      if (root() != nullptr) {
        root()->setOpacity(m_options.opacity);
      }
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopVideoWidget::doLayout(Renderer& renderer) {
  if (m_video == nullptr || root() == nullptr) {
    return;
  }

  const float scale = contentScale();
  const float width = std::round(m_options.width * scale);
  const float height = std::round(m_options.height * scale);

  // Corner radius stays in logical pixels when the tile is resized, like the background radius.
  m_video->setRadius(m_options.radius * m_baseScale);
  m_video->setSize(width, height);
  m_video->layout(renderer);
  root()->setSize(width, height);
}
