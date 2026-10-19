#include <string.h>

#include "softusb.h"

static const uint16_t SU_TABLE crc16_table[256] = {
    0x0000, 0xc0c1, 0xc181, 0x0140, 0xc301, 0x03c0, 0x0280, 0xc241, 0xc601, 0x06c0, 0x0780, 0xc741, 0x0500, 0xc5c1,
    0xc481, 0x0440, 0xcc01, 0x0cc0, 0x0d80, 0xcd41, 0x0f00, 0xcfc1, 0xce81, 0x0e40, 0x0a00, 0xcac1, 0xcb81, 0x0b40,
    0xc901, 0x09c0, 0x0880, 0xc841, 0xd801, 0x18c0, 0x1980, 0xd941, 0x1b00, 0xdbc1, 0xda81, 0x1a40, 0x1e00, 0xdec1,
    0xdf81, 0x1f40, 0xdd01, 0x1dc0, 0x1c80, 0xdc41, 0x1400, 0xd4c1, 0xd581, 0x1540, 0xd701, 0x17c0, 0x1680, 0xd641,
    0xd201, 0x12c0, 0x1380, 0xd341, 0x1100, 0xd1c1, 0xd081, 0x1040, 0xf001, 0x30c0, 0x3180, 0xf141, 0x3300, 0xf3c1,
    0xf281, 0x3240, 0x3600, 0xf6c1, 0xf781, 0x3740, 0xf501, 0x35c0, 0x3480, 0xf441, 0x3c00, 0xfcc1, 0xfd81, 0x3d40,
    0xff01, 0x3fc0, 0x3e80, 0xfe41, 0xfa01, 0x3ac0, 0x3b80, 0xfb41, 0x3900, 0xf9c1, 0xf881, 0x3840, 0x2800, 0xe8c1,
    0xe981, 0x2940, 0xeb01, 0x2bc0, 0x2a80, 0xea41, 0xee01, 0x2ec0, 0x2f80, 0xef41, 0x2d00, 0xedc1, 0xec81, 0x2c40,
    0xe401, 0x24c0, 0x2580, 0xe541, 0x2700, 0xe7c1, 0xe681, 0x2640, 0x2200, 0xe2c1, 0xe381, 0x2340, 0xe101, 0x21c0,
    0x2080, 0xe041, 0xa001, 0x60c0, 0x6180, 0xa141, 0x6300, 0xa3c1, 0xa281, 0x6240, 0x6600, 0xa6c1, 0xa781, 0x6740,
    0xa501, 0x65c0, 0x6480, 0xa441, 0x6c00, 0xacc1, 0xad81, 0x6d40, 0xaf01, 0x6fc0, 0x6e80, 0xae41, 0xaa01, 0x6ac0,
    0x6b80, 0xab41, 0x6900, 0xa9c1, 0xa881, 0x6840, 0x7800, 0xb8c1, 0xb981, 0x7940, 0xbb01, 0x7bc0, 0x7a80, 0xba41,
    0xbe01, 0x7ec0, 0x7f80, 0xbf41, 0x7d00, 0xbdc1, 0xbc81, 0x7c40, 0xb401, 0x74c0, 0x7580, 0xb541, 0x7700, 0xb7c1,
    0xb681, 0x7640, 0x7200, 0xb2c1, 0xb381, 0x7340, 0xb101, 0x71c0, 0x7080, 0xb041, 0x5000, 0x90c1, 0x9181, 0x5140,
    0x9301, 0x53c0, 0x5280, 0x9241, 0x9601, 0x56c0, 0x5780, 0x9741, 0x5500, 0x95c1, 0x9481, 0x5440, 0x9c01, 0x5cc0,
    0x5d80, 0x9d41, 0x5f00, 0x9fc1, 0x9e81, 0x5e40, 0x5a00, 0x9ac1, 0x9b81, 0x5b40, 0x9901, 0x59c0, 0x5880, 0x9841,
    0x8801, 0x48c0, 0x4980, 0x8941, 0x4b00, 0x8bc1, 0x8a81, 0x4a40, 0x4e00, 0x8ec1, 0x8f81, 0x4f40, 0x8d01, 0x4dc0,
    0x4c80, 0x8c41, 0x4400, 0x84c1, 0x8581, 0x4540, 0x8701, 0x47c0, 0x4680, 0x8641, 0x8201, 0x42c0, 0x4380, 0x8341,
    0x4100, 0x81c1, 0x8081, 0x4040,
};

uint16_t su_crc16(const uint8_t* data, size_t length) {
  uint16_t crc = 0xffff;
  for (size_t i = 0; i < length; i++) crc = (crc >> 8) ^ crc16_table[(crc ^ data[i]) & 255];
  return crc ^ 0xffff;
}
uint8_t su_crc5(uint16_t token) {
  uint8_t crc = 31;
  for (unsigned i = 0; i < 11; i++) {
    bool bit = (crc ^ (token >> i)) & 1;
    crc >>= 1;
    if (bit) crc ^= 0x14;
  }
  return crc ^ 31;
}
static bool is_token(uint8_t pid) { return pid == SU_IN || pid == SU_OUT || pid == SU_SETUP || pid == SU_SOF; }
static bool is_data(uint8_t pid) { return pid == SU_DATA0 || pid == SU_DATA1; }
static bool is_handshake(uint8_t pid) { return pid == SU_ACK || pid == SU_NAK || pid == SU_STALL; }
bool su_parse_wire(const uint8_t* wire, size_t length, su_packet* packet) {
  if (length < 2 || length > SU_WIRE_MAX || wire[0] != 0x80) return false;
  uint8_t pid = wire[1];
  if (((pid >> 4) ^ (pid & 15)) != 15) return false;
  su_packet p = {0};
  p.pid = pid;
  if (is_token(pid)) {
    if (length != 4) return false;
    uint16_t value = wire[2] | ((uint16_t)wire[3] << 8);
    p.token = value & 2047;
    if (su_crc5(p.token) != (value >> 11)) return false;
  } else if (is_data(pid)) {
    if (length < 4) return false;
    p.len = (uint8_t)(length - 4);
    if (su_crc16(wire + 2, p.len) != (wire[length - 2] | ((uint16_t)wire[length - 1] << 8))) return false;
    memcpy(p.data, wire + 2, p.len);
  } else if (!is_handshake(pid) || length != 2)
    return false;
  *packet = p;
  return true;
}
size_t su_wire(const su_packet* p, uint8_t wire[SU_WIRE_MAX]) {
  wire[0] = 0x80;
  wire[1] = p->pid;
  if (is_token(p->pid)) {
    if (p->token > 2047) return 0;
    uint16_t value = p->token | ((uint16_t)su_crc5(p->token) << 11);
    wire[2] = value & 255;
    wire[3] = value >> 8;
    return 4;
  }
  if (is_data(p->pid)) {
    if (p->len > SU_MPS) return 0;
    memcpy(wire + 2, p->data, p->len);
    uint16_t crc = su_crc16(p->data, p->len);
    wire[2 + p->len] = crc & 255;
    wire[3 + p->len] = crc >> 8;
    return p->len + 4;
  }
  return is_handshake(p->pid) ? 2 : 0;
}
size_t su_encode(const su_packet* p, uint8_t symbols[SU_SYMBOL_MAX]) {
  uint8_t wire[SU_WIRE_MAX];
  size_t bytes = su_wire(p, wire), count = 0;
  if (!bytes) return 0;
  uint8_t level = SU_J, ones = 0;
  for (size_t i = 0; i < bytes; i++)
    for (unsigned j = 0; j < 8; j++) {
      bool bit = (wire[i] >> j) & 1;
      if (!bit) level ^= 3;
      symbols[count++] = level;
      ones = bit ? ones + 1 : 0;
      if (ones == 6) {
        level ^= 3;
        symbols[count++] = level;
        ones = 0;
      }
    }
  symbols[count++] = SU_SE0;
  symbols[count++] = SU_SE0;
  symbols[count++] = SU_J;
  return count;
}
void su_decoder_init(su_decoder* d) {
  memset(d, 0, sizeof(*d));
  d->previous = SU_J;
}
su_decode_result su_decode_symbol(su_decoder* d, uint8_t symbol, su_packet* packet) {
  if (d->result != SU_MORE) return d->result;
  if (symbol > SU_J) goto bad;
  if (symbol == SU_SE0) {
    if (d->bit || d->ones == 6 || d->count < 2 || ++d->se0 > 2) goto bad;
    return SU_MORE;
  }
  if (d->se0) {
    if (d->se0 != 2 || symbol != SU_J || !su_parse_wire(d->wire, d->count, packet)) goto bad;
    return d->result = SU_COMPLETE;
  }
  bool bit = symbol == d->previous;
  d->previous = symbol;
  if (d->ones == 6) {
    if (bit) goto bad;
    d->ones = 0;
    return SU_MORE;
  }
  d->ones = bit ? d->ones + 1 : 0;
  d->current |= (uint8_t)(bit << d->bit);
  if (++d->bit == 8) {
    if (d->count == SU_WIRE_MAX) goto bad;
    d->wire[d->count++] = d->current;
    d->current = 0;
    d->bit = 0;
    if (d->count == 1 && d->wire[0] != 0x80) goto bad;
  }
  return SU_MORE;
bad:
  return d->result = SU_INVALID;
}
