#pragma once
class SPIClass {
public:
  void begin(int sck = -1, int miso = -1, int mosi = -1, int ss = -1) {}
  void end() {}
};
extern SPIClass SPI;
