#pragma once
inline bool textSettingsSaveFails = false;
inline unsigned textSettingsDefaultWrites = 0;
template <typename T>
class PersistableStore {
 public:
  static T& getInstance() {
    static T value;
    return value;
  }
  bool saveToFile() const {
    ++textSettingsDefaultWrites;
    return !textSettingsSaveFails;
  }
};
