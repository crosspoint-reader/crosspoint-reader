#pragma once

#include <string>
#include <vector>

enum class SdThemeRendererHint { Lyra, Carousel };

struct SdThemeDeviceConstraints {
  int screenWidth = 0;
  int screenHeight = 0;
  int frontButtons = 0;
  std::string sideButtons;
};

struct SdCardThemeInfo {
  std::string id;
  std::string name;
  std::string path;
  std::string inherits;
  std::string deviceId;
  SdThemeDeviceConstraints constraints;
  SdThemeRendererHint rendererHint = SdThemeRendererHint::Lyra;
};

class SdCardThemeRegistry {
 public:
  static constexpr int MAX_SD_THEMES = 64;
  static constexpr const char* THEMES_DIR_HIDDEN = "/.themes";
  static constexpr const char* THEMES_DIR_VISIBLE = "/themes";

  bool discover();

  const std::vector<SdCardThemeInfo>& getThemes() const { return themes_; }
  const SdCardThemeInfo* findTheme(const std::string& id) const;
  int getThemeCount() const { return static_cast<int>(themes_.size()); }

 private:
  std::vector<SdCardThemeInfo> themes_;

  static const char* activeDeviceId();
  static bool parseThemeJson(const char* themeDirPath, SdCardThemeInfo& out);
  static bool isSafeId(const char* value);
  static SdThemeRendererHint rendererHintFor(const char* id, const char* componentModule);
  static void scanRoot(const char* rootPath, std::vector<SdCardThemeInfo>& out);
};
