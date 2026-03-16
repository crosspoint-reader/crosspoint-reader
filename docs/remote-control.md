# Remote Control API

This document describes the remote control features of CrossPoint Reader, including page turns, button injection, and remote keyboard input over WiFi and USB Serial.

Capability discovery:
- HTTP clients should read `remote_page_turn` from `GET /api/plugins`.
- HTTP clients should read `remote_keyboard_input` from `GET /api/plugins`.
- USB clients should read `remote_page_turn` from the `plugins` command response.
- USB clients should read `remote_keyboard_input` from the `plugins` command response.

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

## WiFi Remote Keyboard Input

When the on-device keyboard opens and `remote_keyboard_input` is enabled, the device can hand text entry off to a browser or the Android app.

### Browser Fallback

- **Page:** `GET /remote-input`
- **Session lookup:** `GET /api/remote-keyboard/session`
- **Claim:** `POST /api/remote-keyboard/claim`
- **Submit:** `POST /api/remote-keyboard/submit`

**Claim request body:**
```json
{
  "id": 42,
  "client": "browser"
}
```

**Submit request body:**
```json
{
  "id": 42,
  "text": "My remote text"
}
```

**Runtime availability:**
1. Android app if already connected
2. Browser fallback over existing WiFi with a QR code
3. If WiFi is unavailable, firmware starts a hotspot and serves the same browser page; a USB-connected Android app can still claim the session in parallel

### Android App Behavior

The Android app polls `GET /api/remote-keyboard/session`, claims the session as `android`, then submits the final text over WiFi when connected that way.

---

## USB Serial Remote Keyboard Input

The same feature is available to the Android app over USB serial JSON commands.

**Session lookup:**
```json
{"cmd":"remote_keyboard_session_get"}
```

**Claim:**
```json
{"cmd":"remote_keyboard_claim","arg":{"id":42,"client":"android"}}
```

**Submit:**
```json
{"cmd":"remote_keyboard_submit","arg":{"id":42,"text":"My remote text"}}
```

**Response shape:**
- Session lookup and claim return the active snapshot with `active`, `id`, `title`, `text`, `maxLength`, `isPassword`, and optional `claimedBy`.
- Submit returns `{"ok":true}` on success.

---

## Technical Implementation Details

For developers working on the firmware, the remote control mechanism uses a "Virtual Button" injection system:

1. **HalGPIO**: Maintains a `virtualButtonMask`. The `injectVirtualButton(index)` method sets a bit in this mask. Standard methods like `wasPressed()` check both physical hardware state and this virtual mask.
2. **MappedInputManager**: Provides `injectVirtualActivation(Button button)` which maps a logical button (like `PageForward`) to the correct physical button index based on current user settings (orientation and side-button layout) and then calls the HAL injection.
3. **Command Handling**: WiFi and USB tasks set a `pendingPageTurn` flag in `CrossPointState`.
4. **Main Loop**: The main loop in `src/main.cpp` checks `pendingPageTurn` every tick and triggers the corresponding virtual activation in `MappedInputManager`.

Remote keyboard input uses a separate session-based flow:

1. `KeyboardEntryActivity` creates a `RemoteKeyboardSession` when the keyboard opens.
2. `RemoteKeyboardNetworkSession` either reuses the current WiFi server or starts an access point and temporary web server.
3. Browser and Android clients claim the session, then submit final text through the HTTP or USB routes above.
4. The activity consumes the submitted text and closes normally, or falls back to the local on-device keyboard when requested.
