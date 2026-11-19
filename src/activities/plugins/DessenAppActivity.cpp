#if CROSSPOINT_DESSEN_APPS
#include "DessenAppActivity.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_random.h>

#include <utility>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
constexpr unsigned long FORCE_QUIT_HOLD_MS = 1000;
// Instruction budget per render pass, so a busy app still yields to the UI.
constexpr uint32_t STEP_BUDGET = 200000;
constexpr int MAX_STEPS_PER_RENDER = 8;

bool hasEvent(const fui::InputSnapshot& s) {
  return s.confirm || s.back || s.prev || s.next || (s.touchReleased && s.touchX >= 0);
}
}  // namespace

DessenAppActivity::DessenAppActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string appPath)
    : Activity("DessenApp", renderer, mappedInput), appPath(std::move(appPath)), uiTarget(makeUiTarget(renderer)) {}

bool DessenAppActivity::loadApp() {
  memory = makeUniqueNoThrow<uint8_t[]>(APP_MEMORY_BYTES);
  if (!memory) {
    LOG_ERR("DSN", "OOM: %u bytes of app memory", static_cast<unsigned>(APP_MEMORY_BYTES));
    return false;
  }
  HalFile file;
  if (!Storage.openFileForRead("DSN", appPath, file)) return false;
  const size_t size = file.fileSize();
  if (size == 0 || size > APP_MEMORY_BYTES || file.read(memory.get(), size) != static_cast<int>(size)) {
    LOG_ERR("DSN", "Unreadable or oversized app (%u bytes): %s", static_cast<unsigned>(size), appPath.c_str());
    return false;
  }
  if (!host.load(memory.get(), static_cast<uint32_t>(size), APP_MEMORY_BYTES)) {
    LOG_ERR("DSN", "Not a valid dessen app: %s", appPath.c_str());
    return false;
  }
  host.setRandom(esp_random);
  // appDir = the plugin folder; appName = its basename. These scope the app's
  // device id, key store, and secure_wrap blobs.
  const size_t slash = appPath.find_last_of('/');
  const std::string appDir = slash == std::string::npos ? "/" : appPath.substr(0, slash);
  const size_t nameSlash = appDir.find_last_of('/');
  const std::string appName = nameSlash == std::string::npos ? appDir : appDir.substr(nameSlash + 1);
  services = makeUniqueNoThrow<CrosspointServices>(appName, appDir);
  if (!services) {
    LOG_ERR("DSN", "OOM: services");
    return false;
  }
  host.setServices(services.get());
  return true;
}

void DessenAppActivity::onEnter() {
  Activity::onEnter();
  if (!loadApp()) {
    finish();
    return;
  }
  requestUpdate();
}

void DessenAppActivity::openKeyboard() {
  keyboardOpen = true;
  auto kbd = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, services->keyboardPrompt());
  if (!kbd) {
    services->cancelText();
    keyboardOpen = false;
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(kbd), [this](const ActivityResult& result) {
    if (result.isCancelled || !std::holds_alternative<KeyboardResult>(result.data)) {
      services->cancelText();
    } else {
      services->deliverText(std::get<KeyboardResult>(result.data).text.c_str());
    }
    keyboardOpen = false;
    requestUpdate();
  });
}

void DessenAppActivity::loop() {
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= FORCE_QUIT_HOLD_MS) {
    LOG_INF("DSN", "Force quit: %s", appPath.c_str());
    finish();
    return;
  }

  // An async call parked the app. The network / keygen work and the keyboard
  // run here on the loop task (bigger stack than the render task, where step()
  // runs), then poll() resumes the VM. Under RenderLock so the write of the
  // result and the render task's poll() do not race.
  if (host.state() == dessen::State::Busy) {
    if (services->wantsKeyboard()) {
      if (!keyboardOpen) openKeyboard();
      return;  // waiting on the keyboard subactivity's result
    }
    if (services->hasPendingWork()) {
      RenderLock lock;
      services->serviceAsyncWork();  // blocking HTTP / keygen
    }
    requestUpdate();  // render() will poll(), get the result, and resume the app
    return;
  }

  fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
  using Button = MappedInputManager::Button;
  snap.confirm = mappedInput.wasReleased(Button::Confirm);
  snap.back = mappedInput.wasReleased(Button::Back);
  snap.prev = mappedInput.wasReleased(Button::Left) || mappedInput.wasReleased(Button::Up);
  snap.next = mappedInput.wasReleased(Button::Right) || mappedInput.wasReleased(Button::Down);

  dessen::State state;
  bool deliver = false;
  {
    RenderLock lock;
    state = host.state();
    deliver = state == dessen::State::Waiting && hasEvent(snap);
    if (deliver) pendingInput = snap;
  }

  if (state == dessen::State::Exited || state == dessen::State::Faulted) {
    if (state == dessen::State::Faulted) {
      LOG_ERR("DSN", "App fault at 0x%08x (pc 0x%08x): %s", static_cast<unsigned>(host.vm().fault_addr),
              static_cast<unsigned>(host.vm().pc), appPath.c_str());
    }
    finish();
    return;
  }
  if (deliver || state == dessen::State::Running) requestUpdate();
}

void DessenAppActivity::render(RenderLock&&) {
  // Carries the board's bezel insets for the current rotation; the app draws inside them.
  const fui::DeviceContext device = uiTarget.deviceContext();
  for (int i = 0; i < MAX_STEPS_PER_RENDER; i++) {
    const dessen::State state = host.step(uiTarget, device, pendingInput, STEP_BUDGET);
    pendingInput = fui::InputSnapshot{};
    if (host.takeRefreshRequest()) renderer.displayBuffer();
    if (state != dessen::State::Running) break;  // still running: loop() asks for another pass
  }
}

#endif  // CROSSPOINT_DESSEN_APPS
