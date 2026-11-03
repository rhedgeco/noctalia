#pragma once

#include "shell/desktop/desktop_widget.h"
#include "ui/controls/image.h"

#include <string>
#include <string_view>
#include <vector>

class VideoView;

// Plays a looping video file on the desktop or lockscreen through VideoView (GStreamer, zero-copy
// when the hardware decoder allows it). The natural size is configured explicitly because the
// video's own dimensions are unknown until the pipeline prerolls; a resized tile scales it.
class DesktopVideoWidget : public DesktopWidget {
public:
  struct Options {
    std::string path;
    ImageFit fit = ImageFit::Cover;
    float radius = 0.0F;
    float width = 480.0F;
    float height = 270.0F;
    float opacity = 1.0F;
  };

  explicit DesktopVideoWidget(Options options);

  void create() override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;

  static ImageFit fitFromString(std::string_view value);
  // File picker filter for the `video_path` setting.
  static std::vector<std::string> fileExtensions();

private:
  void doLayout(Renderer& renderer) override;

  Options m_options;
  VideoView* m_video = nullptr;
};
