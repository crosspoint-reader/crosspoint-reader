#pragma once

#include <Print.h>
#include <XmlParserUtils.h>

#include <climits>

// Stream only the algorithm attributes; resource names stay in the ZIP.
class EncryptionManifestProbe : public Print {
 public:
  ~EncryptionManifestProbe() override { destroyXmlParser(parser); }

  bool setup() {
    parser = XML_ParserCreate(nullptr);
    if (!parser) return false;
    XML_SetUserData(parser, this);
    XML_SetStartElementHandler(parser, onStartElement);
    return true;
  }

  size_t write(uint8_t data) override { return write(&data, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    if (!parser || size > INT_MAX ||
        XML_Parse(parser, reinterpret_cast<const char*>(data), static_cast<int>(size), XML_FALSE) == XML_STATUS_ERROR) {
      return 0;
    }
    return size;
  }

  bool finish() { return parser && XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_OK; }
  bool needsProtectionCheck() const { return aes128; }

 private:
  XML_Parser parser = nullptr;
  bool aes128 = false;

  static void XMLCALL onStartElement(void* context, const XML_Char* name, const XML_Char** attributes) {
    if (!xmlLocalNameEquals(name, "EncryptionMethod")) return;
    for (size_t i = 0; attributes[i]; i += 2) {
      // Match the SDK's supported algorithm; it validates resource references.
      if (xmlLocalNameEquals(attributes[i], "Algorithm") && std::strstr(attributes[i + 1], "aes128-cbc")) {
        static_cast<EncryptionManifestProbe*>(context)->aes128 = true;
      }
    }
  }
};
