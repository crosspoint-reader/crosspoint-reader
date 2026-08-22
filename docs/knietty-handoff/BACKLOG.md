# knietty backlog after the terminal release

These items are intentionally outside the ordered terminal milestones.

## BLE keyboard input

Investigate only after the Wi-Fi terminal, Rust host, TLS pairing, display
scheduler, and safe release path are stable. Questions to resolve before code:

- Does the runtime X4/ESP32-C3 board expose usable BLE concurrently with Wi-Fi?
- What heap remains after Wi-Fi + TLS + display buffers?
- Which HID keyboard layouts and modifier/dead-key rules are required?
- Does Wi-Fi/BLE coexistence materially affect RSSI, latency, battery, or panel
  scheduling?
- Should input go directly into the local terminal model or relay as framed v3
  `TERMINAL_INPUT` to the host PTY?
- How are keyboard pairing, trust, removal, and reconnect represented on-device?

Start with an isolated BLE HID discovery/keypress proof, no automatic pairing,
and no concurrent driver experiment. Record heap and latency before merging.

## Other deferred work

- Larger Nerd Font coverage requires `wcwidth`, double-width cells, combining
  behavior, and a flash/RAM budget; it is not merely another bitmap table.
- Custom terminfo should follow a stable terminal capability set.
- USB CDC may be revisited only on hardware that actually exposes an application
  CDC node; it remains absent on the tested locked X4.
- Optical instrumentation with a photodiode can replace high-speed-video timing
  if a safe, non-invasive setup becomes available.
