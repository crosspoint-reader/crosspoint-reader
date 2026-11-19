#if CROSSPOINT_DESSEN_APPS
#include "DessenAppActivity.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_random.h>
#include <sdk/dessen_abi.h>  // DSN_HOME_*

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

DessenAppActivity::DessenAppActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string appPath,
                                     bool homeLauncher)
    : Activity("DessenApp", renderer, mappedInput),
      appPath(std::move(appPath)),
      homeLauncher(homeLauncher),
      uiTarget(makeUiTarget(renderer)) {}

bool DessenAppActivity::loadApp() {
  HalFile file;
  if (!Storage.openFileForRead("DSN", appPath, file)) return false;
  const size_t size = file.fileSize();
  // Peek the header for the memory block the app asked for (2nd u32, little-endian).
  uint8_t hdr[DSN_HEADER_SIZE];
  if (size < DSN_HEADER_SIZE || file.read(hdr, DSN_HEADER_SIZE) != static_cast<int>(DSN_HEADER_SIZE)) return false;
  const uint32_t memSize = hdr[4] | hdr[5] << 8 | hdr[6] << 16 | hdr[7] << 24;
  if (memSize < size || memSize > MAX_APP_MEMORY) {
    LOG_ERR("DSN", "App wants %u bytes (file %u, cap %u): out of range", static_cast<unsigned>(memSize),
            static_cast<unsigned>(size), static_cast<unsigned>(MAX_APP_MEMORY));
    return false;
  }
  memory = makeUniqueNoThrow<uint8_t[]>(memSize);  // > ~4 KB -> PSRAM on an S3
  if (!memory) {
    LOG_ERR("DSN", "OOM: %u bytes of app memory", static_cast<unsigned>(memSize));
    return false;
  }
  if (!file.seek(0) || file.read(memory.get(), size) != static_cast<int>(size)) {
    LOG_ERR("DSN", "Unreadable app: %s", appPath.c_str());
    return false;
  }
  if (!host.load(memory.get(), static_cast<uint32_t>(size), memSize)) {
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
  services->setFontContext(&renderer, &uiTarget);  // lets font_load bind the custom font slot
  return true;
}

void DessenAppActivity::onEnter() {
  Activity::onEnter();
  {
    RenderLock lock;
    if (loadApp()) {
      requestUpdate();
      return;
    }
  }
  leaveApp();
}

void DessenAppActivity::leaveApp() {
  if (homeLauncher) {
    activityManager.goHome(HomeMenuItem::NONE, /*cleanInitialRefresh=*/false, /*forceDefaultHome=*/true);
  } else {
    finish();
  }
}

bool DessenAppActivity::handleHomeGesture() {
  if (!homeLauncher) return false;
  leaveApp();
  return true;
}

void DessenAppActivity::openKeyboard() {
  auto kbd = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, services->keyboardPrompt());
  if (!kbd) {
    RenderLock lock;
    services->cancelText();
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(kbd), [this](const ActivityResult& result) {
    RenderLock lock;
    if (result.isCancelled || !std::holds_alternative<KeyboardResult>(result.data)) {
      services->cancelText();
    } else {
      services->deliverText(std::get<KeyboardResult>(result.data).text.c_str());
    }
    requestUpdate();
  });
}

void DessenAppActivity::loop() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, FORCE_QUIT_HOLD_MS)) {
    LOG_INF("DSN", "Force quit: %s", appPath.c_str());
    leaveApp();
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
    if (state == dessen::State::Busy) {
      if (services->wantsKeyboard()) {
        lock.unlock();
        openKeyboard();
        return;
      }
      // Blocking work uses the loop stack. Keep the render task out of poll().
      if (services->serviceAsyncWork()) requestUpdate();
      return;
    }
    deliver = state == dessen::State::Waiting && hasEvent(snap);
    if (deliver) pendingInput = snap;
  }

  // A launcher/home app asked to open a firmware screen and end. goTo*/goHome
  // replace this activity, so there is nothing to finish afterwards.
  if (state == dessen::State::Launch) {
    switch (host.launch()) {
      case dessen::Launch::Reader:
        if (!activityManager.goToReader(host.launchPath())) leaveApp();
        break;
      case dessen::Launch::Settings:
        activityManager.goToSettings();
        break;
      case dessen::Launch::Browser:
        activityManager.goToFileBrowser();
        break;
      case dessen::Launch::Home: {
        // open_home(dest): open the destination directly, the same way the home
        // menu does. goHome(item) only lands on the home menu with the item
        // highlighted, which reads as "back to the default home".
        switch (host.launchHomeDest()) {
          case DSN_HOME_SETTINGS:
            activityManager.goToSettings();
            break;
          case DSN_HOME_LIBRARY:
            if (!activityManager.goToLibrary()) leaveApp();
            break;
          case DSN_HOME_PLUGINS:
            activityManager.goToPlugins();
            break;
          case DSN_HOME_OPDS:
            activityManager.goToBrowser();
            break;
          case DSN_HOME_TRANSFER:
            activityManager.goToFileTransfer();
            break;
          case DSN_HOME_FILES:
          default:
            activityManager.goToFileBrowser();
            break;
        }
        break;
      }
      default:
        leaveApp();
        break;
    }
    return;
  }

  if (state == dessen::State::Exited || state == dessen::State::Faulted) {
    if (state == dessen::State::Faulted) {
      LOG_ERR("DSN", "App fault at 0x%08x (pc 0x%08x): %s", static_cast<unsigned>(host.vm().fault_addr),
              static_cast<unsigned>(host.vm().pc), appPath.c_str());
    }
    leaveApp();
    return;
  }
  if (deliver || state == dessen::State::Running) requestUpdate();
}

void DessenAppActivity::render(RenderLock&&) {
  if (!services || host.state() == dessen::State::Launch) return;
  // Carries the board's bezel insets for the current rotation; the app draws inside them.
  const fui::DeviceContext device = uiTarget.deviceContext();
  bool drew = false;
  dessen::State state = dessen::State::Running;
  for (int i = 0; i < MAX_STEPS_PER_RENDER; i++) {
    state = host.step(uiTarget, device, pendingInput, STEP_BUDGET);
    pendingInput = fui::InputSnapshot{};
    if (host.takeRefreshRequest()) {
      renderer.displayBuffer();
      // Snapshot the latest clean frame so it can be restored after an overlay.
      // discard first: the slot holds the previous frame and storeBwBuffer()
      // errors on a double store.
      renderer.discardStoredBwBuffer();
      frameStored = renderer.storeBwBuffer();
      drew = true;
    }
    if (state != dessen::State::Running) break;  // still running: loop() asks for another pass
  }
  // We were re-rendered with no new frame of our own while the app is parked —
  // i.e. a system overlay (the frontlight panel) popped and ActivityManager asked
  // us to repaint. The parked app can't redraw itself, so restore its last frame
  // to wipe the overlay. Gated on a parked state so long in-VM work (e.g. puzzle
  // generation, which also produces no frame) does not repaint every pass.
  if (!drew && frameStored && (state == dessen::State::Waiting || state == dessen::State::Busy)) {
    renderer.restoreBwBuffer();  // repaints the saved frame; frees the slot
    renderer.displayBuffer();
    frameStored = renderer.storeBwBuffer();  // re-save it for the next overlay (slot is free)
  }
}

#endif  // CROSSPOINT_DESSEN_APPS
