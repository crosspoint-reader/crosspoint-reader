#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string>

class TrmnlService {
 public:
  struct Config {
    bool enabled = false;
    std::string serverUrl = "http://192.168.1.100:8080"; // Default BYOS URL
    std::string apiKey = "";
  };

  static void loadConfig();
  static void saveConfig();
  static Config& getConfig();

  // Register device with the BYOS server
  static bool registerDevice();

  // Download the screen image to /sleep.bmp
  static bool refreshScreen();

  // Helper to get MAC address formatted for TRMNL (no colons, lowercase/uppercase as needed)
  static std::string getMacAddress();

 private:
  static Config config;
  static bool configLoaded;
  static const char* CONFIG_PATH;
};
