#pragma once
#include <Print.h>

#include <string>

#include "expat.h"

class ContainerParser final : public Print {
  enum ParserState {
    START,
    IN_CONTAINER,
    IN_ROOTFILES,
  };

  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void endElement(void* userData, const XML_Char* name);

 public:
  std::string fullPath;

  explicit ContainerParser(const size_t xmlSize) : remainingSize(xmlSize) {}
  ~ContainerParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
