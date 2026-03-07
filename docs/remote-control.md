# Remote Control API

This document describes the remote control features of CrossPoint Reader, allowing for page turns and button injection over WiFi (HTTP and WebSocket) and USB Serial.

Capability discovery:
- HTTP clients should read `remote_page_turn` from `GET /api/plugins`.
- USB clients should read `remote_page_turn` from the `plugins` command response.

## WiFi Remote Control

When WiFi is enabled and the web server is running, the device accepts remote button commands.

### HTTP API

**Endpoint:** `POST /api/remote/button`  
**Content-Type:** `application/json`

**Request Body:**
```json
{
  "button": "page_forward"
}
```

**Supported Button Values:**
- `page_forward`, `next`: Equivalent to a "Next Page" action.
- `page_back`, `prev`, `previous`: Equivalent to a "Previous Page" action.

**Example using curl:**
```bash
curl -X POST http://<device-ip>/api/remote/button 
     -H "Content-Type: application/json" 
     -d '{"button": "page_forward"}'
```

### WebSocket API

**Endpoint:** `ws://<device-ip>/ws`

The WebSocket interface accepts simple text commands for rapid page navigation.

**Commands:**
- `PAGE:NEXT` or `PAGE:FORWARD`: Turn to the next page.
- `PAGE:PREV` or `PAGE:BACK`: Turn to the previous page.

**Example using wscat:**
```bash
wscat -c ws://<device-ip>/ws
> PAGE:NEXT
```

---

## USB Serial Remote Control

The device accepts JSON commands over the USB Serial interface (CDC).

**Command Format:**
```json
{"cmd":"remote_button","arg":"page_forward"}
```

**Supported Arguments (`arg`):**
- `page_forward`, `next`: Turn to the next page.
- `page_back`, `prev`: Turn to the previous page.

**Example (sending via serial terminal):**
```bash
echo '{"cmd":"remote_button","arg":"page_forward"}' > /dev/ttyACM0
```

---

## Technical Implementation Details

For developers working on the firmware, the remote control mechanism uses a "Virtual Button" injection system:

1. **HalGPIO**: Maintains a `virtualButtonMask`. The `injectVirtualButton(index)` method sets a bit in this mask. Standard methods like `wasPressed()` check both physical hardware state and this virtual mask.
2. **MappedInputManager**: Provides `injectVirtualActivation(Button button)` which maps a logical button (like `PageForward`) to the correct physical button index based on current user settings (orientation and side-button layout) and then calls the HAL injection.
3. **Command Handling**: WiFi and USB tasks set a `pendingPageTurn` flag in `CrossPointState`.
4. **Main Loop**: The main loop in `src/main.cpp` checks `pendingPageTurn` every tick and triggers the corresponding virtual activation in `MappedInputManager`.
