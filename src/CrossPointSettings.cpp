#include "CrossPointSettings.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include <cstring>

#include "fontIds.h"

// Initialize the static instance
CrossPointSettings CrossPointSettings::instance;

// SettingDescriptor implementations
bool SettingDescriptor::validate(const CrossPointSettings& settings) const {
  if (type == SettingType::STRING) {
    return true;  // Strings are always valid
  }
  if (!validator) {
    return true;  // No validator means always valid
  }
  const uint8_t value = settings.*(memberPtr);
  return validator(value);
}

uint8_t SettingDescriptor::getValue(const CrossPointSettings& settings) const { return settings.*(memberPtr); }

void SettingDescriptor::setValue(CrossPointSettings& settings, uint8_t value) const { settings.*(memberPtr) = value; }

void SettingDescriptor::resetToDefault(CrossPointSettings& settings) const {
  if (type == SettingType::STRING) {
    strncpy(stringPtr, stringData.defaultString, stringData.maxSize - 1);
    stringPtr[stringData.maxSize - 1] = '\0';
    return;
  }
  setValue(settings, defaultValue);
}

void SettingDescriptor::save(FsFile& file, const CrossPointSettings& settings) const {
  if (type == SettingType::STRING) {
    serialization::writeString(file, std::string(stringPtr));
    return;
  }
  serialization::writePod(file, settings.*(memberPtr));
}

void SettingDescriptor::load(FsFile& file, CrossPointSettings& settings) const {
  if (type == SettingType::STRING) {
    serialization::readString(file, stringPtr, stringData.maxSize);
    return;
  }
  uint8_t value;
  serialization::readPod(file, value);
  settings.*(memberPtr) = value;
}

namespace {
constexpr uint8_t SETTINGS_FILE_VERSION = 1;
constexpr char SETTINGS_FILE[] = "/.crosspoint/settings.bin";
}  // namespace

// Define enum value arrays
namespace {
constexpr const char* sleepScreenValues[] = {"Dark", "Light", "Custom", "Cover", "None"};
constexpr const char* shortPwrBtnValues[] = {"Ignore", "Sleep", "Page Turn"};
constexpr const char* statusBarValues[] = {"None", "No Progress", "Full"};
constexpr const char* orientationValues[] = {"Portrait", "Landscape CW", "Inverted", "Landscape CCW"};
constexpr const char* frontButtonLayoutValues[] = {"Bck, Cnfrm, Lft, Rght", "Lft, Rght, Bck, Cnfrm",
                                                   "Lft, Bck, Cnfrm, Rght", "Bck, Cnfrm, Rght, Lft"};
constexpr const char* sideButtonLayoutValues[] = {"Prev/Next", "Next/Prev"};
constexpr const char* fontFamilyValues[] = {"Bookerly", "Noto Sans", "Open Dyslexic"};
constexpr const char* fontSizeValues[] = {"Small", "Medium", "Large", "X Large"};
constexpr const char* lineSpacingValues[] = {"Tight", "Normal", "Wide"};
constexpr const char* paragraphAlignmentValues[] = {"Justify", "Left", "Center", "Right"};
constexpr const char* sleepTimeoutValues[] = {"1 min", "5 min", "10 min", "15 min", "30 min"};
constexpr const char* refreshFrequencyValues[] = {"1 page", "5 pages", "10 pages", "15 pages", "30 pages"};
constexpr const char* sleepScreenCoverModeValues[] = {"Fit", "Crop"};
constexpr const char* hideBatteryPercentageValues[] = {"Never", "In Reader", "Always"};
constexpr const char* sleepScreenCoverFilterValues[] = {"None", "Contrast", "Inverted"};

// Helper function template to deduce array size automatically
template <size_t N>
constexpr SettingDescriptor makeEnumDescriptor(const char* name, uint8_t CrossPointSettings::* ptr,
                                               uint8_t defaultValue, const char* const (&enumValues)[N]) {
  return SettingDescriptor(name, SettingType::ENUM, ptr, defaultValue, validateEnum<N>, enumValues, N);
}

// Helper macro to create STRING descriptors without repetition
#define makeStringDescriptor(name, member, defStr)                                          \
  SettingDescriptor(name, SettingType::STRING, CrossPointSettings::instance.member, defStr, \
                    sizeof(CrossPointSettings::member))
}  // namespace

// Define static constexpr members (required in C++14 and earlier)
constexpr size_t CrossPointSettings::DESCRIPTOR_COUNT;

// Define the static constexpr array of all setting descriptors
// Order must match current serialization order for file format compatibility!
const std::array<SettingDescriptor, CrossPointSettings::DESCRIPTOR_COUNT> CrossPointSettings::descriptors = {{
    makeEnumDescriptor("Sleep Screen", &CrossPointSettings::sleepScreen, CrossPointSettings::DARK, sleepScreenValues),
    {"Extra Paragraph Spacing", SettingType::TOGGLE, &CrossPointSettings::extraParagraphSpacing, 1, validateToggle,
     nullptr, 0},
    makeEnumDescriptor("Short Power Button Click", &CrossPointSettings::shortPwrBtn, CrossPointSettings::IGNORE,
                       shortPwrBtnValues),
    makeEnumDescriptor("Status Bar", &CrossPointSettings::statusBar, CrossPointSettings::FULL, statusBarValues),
    makeEnumDescriptor("Reading Orientation", &CrossPointSettings::orientation, CrossPointSettings::PORTRAIT,
                       orientationValues),
    makeEnumDescriptor("Front Button Layout", &CrossPointSettings::frontButtonLayout,
                       CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT, frontButtonLayoutValues),
    makeEnumDescriptor("Side Button Layout", &CrossPointSettings::sideButtonLayout, CrossPointSettings::PREV_NEXT,
                       sideButtonLayoutValues),
    makeEnumDescriptor("Reader Font Family", &CrossPointSettings::fontFamily, CrossPointSettings::BOOKERLY,
                       fontFamilyValues),
    makeEnumDescriptor("Reader Font Size", &CrossPointSettings::fontSize, CrossPointSettings::MEDIUM, fontSizeValues),
    makeEnumDescriptor("Reader Line Spacing", &CrossPointSettings::lineSpacing, CrossPointSettings::NORMAL,
                       lineSpacingValues),
    makeEnumDescriptor("Reader Paragraph Alignment", &CrossPointSettings::paragraphAlignment,
                       CrossPointSettings::JUSTIFIED, paragraphAlignmentValues),
    makeEnumDescriptor("Time to Sleep", &CrossPointSettings::sleepTimeout, CrossPointSettings::SLEEP_10_MIN,
                       sleepTimeoutValues),
    makeEnumDescriptor("Refresh Frequency", &CrossPointSettings::refreshFrequency, CrossPointSettings::REFRESH_15,
                       refreshFrequencyValues),
    {"Reader Screen Margin", SettingType::VALUE, &CrossPointSettings::screenMargin, 5, validateRange<5, 40>,
     ValueRange{5, 40, 5}},
    makeEnumDescriptor("Sleep Screen Cover Mode", &CrossPointSettings::sleepScreenCoverMode, CrossPointSettings::FIT,
                       sleepScreenCoverModeValues),
    makeStringDescriptor("OPDS Server URL", opdsServerUrl, ""),
    {"Text Anti-Aliasing", SettingType::TOGGLE, &CrossPointSettings::textAntiAliasing, 1, validateToggle, nullptr, 0},
    makeEnumDescriptor("Hide Battery %", &CrossPointSettings::hideBatteryPercentage, CrossPointSettings::HIDE_NEVER,
                       hideBatteryPercentageValues),
    {"Long-press Chapter Skip", SettingType::TOGGLE, &CrossPointSettings::longPressChapterSkip, 1, validateToggle,
     nullptr, 0},
    {"Hyphenation", SettingType::TOGGLE, &CrossPointSettings::hyphenationEnabled, 0, validateToggle, nullptr, 0},
    makeStringDescriptor("Username", opdsUsername, ""),
    makeStringDescriptor("Password", opdsPassword, ""),
    makeEnumDescriptor("Sleep Screen Cover Filter", &CrossPointSettings::sleepScreenCoverFilter,
                       CrossPointSettings::NO_FILTER, sleepScreenCoverFilterValues),
}};

bool CrossPointSettings::saveToFile() const {
  // Make sure the directory exists
  SdMan.mkdir("/.crosspoint");

  FsFile outputFile;
  if (!SdMan.openFileForWrite("CPS", SETTINGS_FILE, outputFile)) {
    return false;
  }

  serialization::writePod(outputFile, SETTINGS_FILE_VERSION);
  serialization::writePod(outputFile, static_cast<uint8_t>(CrossPointSettings::DESCRIPTOR_COUNT));

  // Use descriptors to automatically serialize all uint8_t settings
  uint8_t descriptorIndex = 0;
  for (const auto& desc : descriptors) {
    desc.save(outputFile, *this);
    descriptorIndex++;
  }

  outputFile.close();

  Serial.printf("[%lu] [CPS] Settings saved to file\n", millis());
  return true;
}

bool CrossPointSettings::loadFromFile() {
  Serial.printf("[%lu] [CPS] Loading settings from file\n", millis());
  FsFile inputFile;
  if (!SdMan.openFileForRead("CPS", SETTINGS_FILE, inputFile)) {
    Serial.printf("[%lu] [CPS] Deserialization failed: Could not open settings file\n", millis());
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version != SETTINGS_FILE_VERSION) {
    Serial.printf("[%lu] [CPS] Deserialization failed: Unknown version %u\n", millis(), version);
    inputFile.close();
    return false;
  }

  uint8_t fileSettingsCount = 0;
  serialization::readPod(inputFile, fileSettingsCount);

  // Use descriptors to automatically deserialize all uint8_t settings
  uint8_t descriptorIndex = 0;
  uint8_t filePosition = 0;

  for (const auto& desc : descriptors) {
    if (filePosition >= fileSettingsCount) {
      break;  // File has fewer settings than current version
    }

    desc.load(inputFile, *this);
    if (!desc.validate(*this)) {
      Serial.printf("[%lu] [CPS] Invalid value (0x%X) for %s, resetting to default\n", millis(), desc.getValue(*this),
                    desc.name);
      desc.resetToDefault(*this);
    }
    descriptorIndex++;
    filePosition++;
  }
  inputFile.close();

  Serial.printf("[%lu] [CPS] Settings loaded from file\n", millis());
  return true;
}

float CrossPointSettings::getReaderLineCompression() const {
  switch (fontFamily) {
    case BOOKERLY:
    default:
      switch (lineSpacing) {
        case TIGHT:
          return 0.95f;
        case NORMAL:
        default:
          return 1.0f;
        case WIDE:
          return 1.1f;
      }
    case NOTOSANS:
      switch (lineSpacing) {
        case TIGHT:
          return 0.90f;
        case NORMAL:
        default:
          return 0.95f;
        case WIDE:
          return 1.0f;
      }
    case OPENDYSLEXIC:
      switch (lineSpacing) {
        case TIGHT:
          return 0.90f;
        case NORMAL:
        default:
          return 0.95f;
        case WIDE:
          return 1.0f;
      }
  }
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  switch (sleepTimeout) {
    case SLEEP_1_MIN:
      return 1UL * 60 * 1000;
    case SLEEP_5_MIN:
      return 5UL * 60 * 1000;
    case SLEEP_10_MIN:
    default:
      return 10UL * 60 * 1000;
    case SLEEP_15_MIN:
      return 15UL * 60 * 1000;
    case SLEEP_30_MIN:
      return 30UL * 60 * 1000;
  }
}

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
  }
}

int CrossPointSettings::getReaderFontId() const {
  switch (fontFamily) {
    case BOOKERLY:
    default:
      switch (fontSize) {
        case SMALL:
          return BOOKERLY_12_FONT_ID;
        case MEDIUM:
        default:
          return BOOKERLY_14_FONT_ID;
        case LARGE:
          return BOOKERLY_16_FONT_ID;
        case EXTRA_LARGE:
          return BOOKERLY_18_FONT_ID;
      }
    case NOTOSANS:
      switch (fontSize) {
        case SMALL:
          return NOTOSANS_12_FONT_ID;
        case MEDIUM:
        default:
          return NOTOSANS_14_FONT_ID;
        case LARGE:
          return NOTOSANS_16_FONT_ID;
        case EXTRA_LARGE:
          return NOTOSANS_18_FONT_ID;
      }
    case OPENDYSLEXIC:
      switch (fontSize) {
        case SMALL:
          return OPENDYSLEXIC_8_FONT_ID;
        case MEDIUM:
        default:
          return OPENDYSLEXIC_10_FONT_ID;
        case LARGE:
          return OPENDYSLEXIC_12_FONT_ID;
        case EXTRA_LARGE:
          return OPENDYSLEXIC_14_FONT_ID;
      }
  }
}
