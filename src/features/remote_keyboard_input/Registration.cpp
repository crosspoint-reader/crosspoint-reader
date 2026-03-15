#include "features/remote_keyboard_input/Registration.h"

#include <ArduinoJson.h>
#include <FeatureFlags.h>
#include <WebServer.h>

#include "core/features/FeatureCatalog.h"
#include "core/registries/WebRouteRegistry.h"
#include "network/RemoteKeyboardSession.h"

namespace features::remote_keyboard_input {
namespace {

#if ENABLE_REMOTE_KEYBOARD_INPUT
bool shouldRegisterRemoteKeyboardRoutes() { return core::FeatureCatalog::isEnabled("remote_keyboard_input"); }

void sendSessionSnapshot(WebServer* server) {
  const auto snapshot = REMOTE_KEYBOARD_SESSION.snapshot();

  JsonDocument doc;
  doc["active"] = snapshot.active;
  if (snapshot.active) {
    doc["id"] = snapshot.id;
    doc["title"] = snapshot.title;
    doc["text"] = snapshot.text;
    doc["maxLength"] = snapshot.maxLength;
    doc["isPassword"] = snapshot.isPassword;
    if (!snapshot.claimedBy.empty()) {
      doc["claimedBy"] = snapshot.claimedBy;
      doc["lastClaimAt"] = snapshot.lastClaimAt;
    }
  }

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void mountRemoteKeyboardRoutes(WebServer* server) {
  server->on("/remote-input", HTTP_GET, [server] {
    static const char kRemoteInputHtml[] = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>CrossPoint Remote Input</title>
  <style>
    :root {
      color-scheme: light;
      --ink: #111111;
      --paper: #f4f1e8;
      --panel: #fffdf7;
      --line: #2f2a24;
      --muted: #6d6458;
      --accent: #274c77;
      --accent-strong: #16324f;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      font-family: "IBM Plex Mono", "SFMono-Regular", Consolas, monospace;
      background:
        radial-gradient(circle at top, rgba(39, 76, 119, 0.08), transparent 38%),
        linear-gradient(180deg, #f8f5ee 0%, var(--paper) 100%);
      color: var(--ink);
      display: grid;
      place-items: center;
      padding: 24px;
    }
    .shell {
      width: min(640px, 100%);
      background: var(--panel);
      border: 2px solid var(--line);
      box-shadow: 8px 8px 0 rgba(17, 17, 17, 0.12);
      padding: 24px;
    }
    .eyebrow {
      font-size: 12px;
      letter-spacing: 0.24em;
      text-transform: uppercase;
      color: var(--muted);
      margin-bottom: 8px;
    }
    h1 {
      margin: 0 0 16px;
      font-size: clamp(28px, 5vw, 40px);
      line-height: 1;
    }
    .status {
      min-height: 20px;
      margin-bottom: 16px;
      color: var(--muted);
      font-size: 14px;
    }
    .field-label {
      display: block;
      font-size: 13px;
      margin-bottom: 8px;
      color: var(--muted);
      text-transform: uppercase;
      letter-spacing: 0.14em;
    }
    textarea, input {
      width: 100%;
      padding: 16px;
      font: inherit;
      border: 2px solid var(--line);
      border-radius: 0;
      background: #ffffff;
      color: var(--ink);
      resize: vertical;
      min-height: 180px;
    }
    input {
      min-height: 0;
      height: 56px;
    }
    .actions {
      display: flex;
      gap: 12px;
      margin-top: 16px;
      flex-wrap: wrap;
    }
    button {
      border: 2px solid var(--line);
      background: var(--accent);
      color: #ffffff;
      font: inherit;
      padding: 14px 18px;
      cursor: pointer;
      min-width: 160px;
    }
    button.secondary {
      background: transparent;
      color: var(--ink);
    }
    button:disabled {
      cursor: not-allowed;
      opacity: 0.55;
    }
    .small {
      margin-top: 16px;
      font-size: 13px;
      color: var(--muted);
      line-height: 1.5;
    }
  </style>
</head>
<body>
  <main class="shell">
    <div class="eyebrow">CrossPoint Remote Keyboard</div>
    <h1 id="title">Waiting for device input</h1>
    <div class="status" id="status">Connecting…</div>
    <label class="field-label" for="textInput">Text</label>
    <textarea id="textInput" placeholder="Waiting for an active text session…" disabled></textarea>
    <div class="actions">
      <button id="submitBtn" disabled>Send To Device</button>
      <button id="refreshBtn" class="secondary" type="button">Refresh</button>
    </div>
    <div class="small" id="meta">Keep this page open while the device waits for text.</div>
  </main>
  <script>
    let activeSession = null;
    let keepAliveHandle = null;

    const titleNode = document.getElementById('title');
    const statusNode = document.getElementById('status');
    const textInput = document.getElementById('textInput');
    const submitBtn = document.getElementById('submitBtn');
    const refreshBtn = document.getElementById('refreshBtn');
    const metaNode = document.getElementById('meta');

    function sessionLabel(session) {
      if (!session) return 'No active text session';
      return session.title || 'Remote text input';
    }

    function setStatus(message) {
      statusNode.textContent = message;
    }

    async function fetchSession() {
      const response = await fetch('/api/remote-keyboard/session', { cache: 'no-store' });
      if (!response.ok) {
        throw new Error('Session lookup failed');
      }
      return await response.json();
    }

    async function claimSession(session) {
      const response = await fetch('/api/remote-keyboard/claim', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ id: session.id, client: 'browser' })
      });
      if (!response.ok) {
        throw new Error('Claim failed');
      }
      return await response.json();
    }

    async function submitSession() {
      if (!activeSession) return;
      submitBtn.disabled = true;
      setStatus('Sending text to device…');
      const response = await fetch('/api/remote-keyboard/submit', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ id: activeSession.id, text: textInput.value })
      });
      if (!response.ok) {
        const message = await response.text();
        throw new Error(message || 'Submit failed');
      }
      setStatus('Text sent. The device should continue immediately.');
    }

    function applySession(session) {
      activeSession = session && session.active ? session : null;
      titleNode.textContent = sessionLabel(activeSession);
      if (!activeSession) {
        textInput.disabled = true;
        textInput.value = '';
        submitBtn.disabled = true;
        metaNode.textContent = 'Open a text field on the device, then refresh this page.';
        setStatus('No active text session.');
        return;
      }

      if (document.activeElement !== textInput) {
        textInput.value = activeSession.text || '';
      }
      textInput.disabled = false;
      textInput.type = activeSession.isPassword ? 'password' : 'text';
      if (activeSession.maxLength > 0) {
        textInput.maxLength = activeSession.maxLength;
        metaNode.textContent = 'Maximum length: ' + activeSession.maxLength + ' characters.';
      } else {
        textInput.removeAttribute('maxLength');
        metaNode.textContent = 'No input length limit.';
      }
      submitBtn.disabled = false;
      setStatus(activeSession.claimedBy === 'browser'
        ? 'Connected. Use this field and send the result.'
        : 'Connected. This page is now controlling the device input.');
      textInput.focus({ preventScroll: true });
    }

    async function refreshSession() {
      try {
        const session = await fetchSession();
        applySession(session);
        if (activeSession) {
          const claimed = await claimSession(activeSession);
          applySession(claimed);
        }
      } catch (error) {
        setStatus(error.message || 'Connection failed');
      }
    }

    submitBtn.addEventListener('click', async () => {
      try {
        await submitSession();
      } catch (error) {
        setStatus(error.message || 'Submit failed');
        submitBtn.disabled = false;
      }
    });

    refreshBtn.addEventListener('click', () => { refreshSession(); });
    textInput.addEventListener('keydown', async (event) => {
      if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
        event.preventDefault();
        await submitBtn.click();
      }
    });

    keepAliveHandle = window.setInterval(refreshSession, 1000);
    refreshSession();
  </script>
</body>
</html>
)HTML";
    server->send(200, "text/html; charset=utf-8", kRemoteInputHtml);
  });

  server->on("/api/remote-keyboard/session", HTTP_GET, [server] { sendSessionSnapshot(server); });

  server->on("/api/remote-keyboard/claim", HTTP_POST, [server] {
    if (!server->hasArg("plain")) {
      server->send(400, "text/plain", "Missing body");
      return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, server->arg("plain"))) {
      server->send(400, "text/plain", "Invalid JSON body");
      return;
    }

    const uint32_t id = doc["id"] | 0;
    const char* client = doc["client"] | "browser";
    if (id == 0 || !REMOTE_KEYBOARD_SESSION.claim(id, client)) {
      server->send(404, "text/plain", "Remote keyboard session not found");
      return;
    }

    sendSessionSnapshot(server);
  });

  server->on("/api/remote-keyboard/submit", HTTP_POST, [server] {
    if (!server->hasArg("plain")) {
      server->send(400, "text/plain", "Missing body");
      return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, server->arg("plain"))) {
      server->send(400, "text/plain", "Invalid JSON body");
      return;
    }

    const uint32_t id = doc["id"] | 0;
    const char* text = doc["text"] | "";
    switch (REMOTE_KEYBOARD_SESSION.submit(id, text)) {
      case RemoteKeyboardSession::SubmitResult::Submitted:
        server->send(200, "application/json", "{\"ok\":true}");
        return;
      case RemoteKeyboardSession::SubmitResult::TextTooLong:
        server->send(400, "text/plain", "Text exceeds session length limit");
        return;
      case RemoteKeyboardSession::SubmitResult::InvalidSession:
      default:
        server->send(404, "text/plain", "Remote keyboard session not found");
        return;
    }
  });
}
#endif

}  // namespace

void registerFeature() {
#if ENABLE_REMOTE_KEYBOARD_INPUT
  core::WebRouteEntry webRouteEntry{};
  webRouteEntry.routeId = "remote_keyboard_input_api";
  webRouteEntry.shouldRegister = shouldRegisterRemoteKeyboardRoutes;
  webRouteEntry.mountRoutes = mountRemoteKeyboardRoutes;
  core::WebRouteRegistry::add(webRouteEntry);
#endif
}

}  // namespace features::remote_keyboard_input
