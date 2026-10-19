#ifndef X3_SOFTUSB_H
#define X3_SOFTUSB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include <esp_attr.h>
#define SU_HOT IRAM_ATTR
#define SU_TABLE DRAM_ATTR
#else
#define SU_HOT
#define SU_TABLE
#endif
#ifdef __cplusplus
extern "C" {
#endif

// Initial backend supports EP0 only, with 64-byte full-speed packets.
enum { SU_MPS = 64, SU_WIRE_MAX = 68, SU_SYMBOL_MAX = 640, SU_EVENT_COUNT = 4 };
enum { SU_SE0 = 0, SU_K = 1, SU_J = 2, SU_SE1 = 3 };
enum {
  SU_OUT = 0xe1,
  SU_IN = 0x69,
  SU_SOF = 0xa5,
  SU_SETUP = 0x2d,
  SU_DATA0 = 0xc3,
  SU_DATA1 = 0x4b,
  SU_ACK = 0xd2,
  SU_NAK = 0x5a,
  SU_STALL = 0x1e
};
typedef struct {
  uint8_t pid, len, data[SU_MPS];
  uint16_t token;
} su_packet;
typedef enum { SU_MORE, SU_COMPLETE, SU_INVALID } su_decode_result;
typedef struct {
  uint8_t wire[SU_WIRE_MAX], count, bit, current, previous, ones, se0;
  su_decode_result result;
} su_decoder;

SU_HOT uint16_t su_crc16(const uint8_t* data, size_t length);
SU_HOT uint8_t su_crc5(uint16_t token);
SU_HOT bool su_parse_wire(const uint8_t* wire, size_t length, su_packet* packet);
size_t su_wire(const su_packet* packet, uint8_t wire[SU_WIRE_MAX]);
size_t su_encode(const su_packet* packet, uint8_t symbols[SU_SYMBOL_MAX]);
void su_decoder_init(su_decoder* decoder);
su_decode_result su_decode_symbol(su_decoder* decoder, uint8_t symbol, su_packet* packet);

typedef enum { SU_EVENT_RESET, SU_EVENT_SETUP, SU_EVENT_XFER } su_event_type;
typedef struct {
  su_event_type type;
  uint8_t ep, len, setup[8];
} su_event;
typedef struct {
  bool queued, stalled, toggle, accepted;
  uint8_t* buffer;
  uint8_t length;
  su_packet packet;  // Stable IN copy retained until host ACK.
} su_endpoint;
typedef struct {
  su_endpoint ep[2];
  su_event events[SU_EVENT_COUNT];
  uint8_t event_read, event_count;
  uint8_t address, pending_address, token;
  bool address_pending, awaiting_ack;
} su_device;

void su_device_init(su_device* device);
void su_bus_reset(su_device* device);
SU_HOT bool su_submit(su_device* device, uint8_t ep, uint8_t* buffer, uint16_t length);
bool su_set_address(su_device* device, uint8_t address);
void su_stall(su_device* device, uint8_t ep);
SU_HOT bool su_next_event(su_device* device, su_event* event);
SU_HOT void su_reject_packet(su_device* device);
// Accepts only packets validated for CRC, stuffing and EOP by the physical
// backend. Returns true if response must be transmitted. No allocation or
// TinyUSB call occurs here. This reference implementation has no timing proof.
SU_HOT bool su_receive(su_device* device, const su_packet* packet, su_packet* response);
#ifdef __cplusplus
}
#endif
#endif
