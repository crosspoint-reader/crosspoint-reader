#pragma once
#if CROSSPOINT_DESSEN_APPS

#include <freeink/DessenServices.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class GfxRenderer;
class SdCardFontManager;
namespace freeink {
namespace content {
class Crypto;
}
namespace ui {
class GfxRendererTarget;
}
}  // namespace freeink

// Firmware implementation of the dessen capability services: the thin adapter
// between the sandbox's syscall ABI and CrossPoint's own C++ APIs. It reuses
// what the firmware already has — WolfsslCrypto, HalStorage, the board HALs,
// HttpDownloader, bookkey — and performs no crypto or I/O of its own.
//
// Threading: synchronous calls (crypto, storage, board, time) run inline on the
// render task where the app's step() executes — they are CPU/SD-bound and quick.
// The blocking calls (network, RSA keygen, keyboard entry) must NOT run there
// (TLS overflows the render stack; keygen trips the watchdog), so they are
// *queued*: the service returns kPending, DessenAppActivity::serviceAsyncWork()
// runs the work on the loop task, and poll() (cheap) reports completion back to
// the render task. At most one async op is outstanding, matching the VM.
class CrosspointServices : public dessen::Services {
 public:
  // `appName` scopes device_id and the key store; `appDir` is the plugin folder.
  CrosspointServices(std::string appName, std::string appDir);
  ~CrosspointServices() override;

  // ---- core ----
  uint32_t timeNow() override;
  int clock(uint8_t* out) override;
  bool randomBytes(uint8_t* out, uint32_t n) override;
  int deviceId(char* out, uint32_t cap) override;
  // ---- storage ----
  int fsRead(const char* path, uint8_t* out, uint32_t cap) override;
  int fsWrite(const char* path, const uint8_t* data, uint32_t n) override;
  int fsList(const char* path, char* out, uint32_t cap) override;
  int fsDelete(const char* path) override;
  int fsAppDir(char* out, uint32_t cap) override;
  int keystore(const char* path, const uint8_t* key, uint32_t n, uint32_t expires) override;
  // launcher / home
  int recents(char* out, uint32_t cap) override;
  int bookProgress(const char* path) override;
  int fontLoad(const char* path, int size) override;
  int wifiConnectStart() override;

  // DessenAppActivity wires the renderer + its text target so fontLoad can
  // register the app's custom font and bind it to the custom font slot.
  void setFontContext(GfxRenderer* renderer, freeink::ui::GfxRendererTarget* target) {
    renderer_ = renderer;
    target_ = target;
  }
  // ---- crypto (all wrap freeink::content::Crypto) ----
  int hash(int algo, const uint8_t* data, uint32_t n, uint8_t* out, uint32_t cap) override;
  int aesCbc(int flags, const uint8_t* keyiv, const uint8_t* data, uint32_t n, uint8_t* out, uint32_t cap) override;
  int rsaEncrypt(const uint8_t* cert, uint32_t cn, const uint8_t* data, uint32_t n, uint8_t* out,
                 uint32_t cap) override;
  int rsaDecrypt(int flags, const uint8_t* priv, uint32_t pn, const uint8_t* data, uint32_t n, uint8_t* out,
                 uint32_t cap) override;
  int rsaSign(int algo, const uint8_t* priv, uint32_t pn, const uint8_t* digest, uint32_t dn, uint8_t* out,
              uint32_t cap) override;
  int pkcs12Parse(const uint8_t* p12, uint32_t n, const char* pass, uint8_t* out, uint32_t cap) override;
  int secureWrap(const uint8_t* data, uint32_t n, uint8_t* out, uint32_t cap) override;
  int secureUnwrap(const uint8_t* blob, uint32_t n, uint8_t* out, uint32_t cap) override;
  // ---- board ----
  int backlightSet(int brightness, int warm) override;
  int backlightGet() override;
  int battery() override;
  int haptic(int ms, int intensity) override;
  int accel(uint8_t* out) override;
  // ---- async: queued on the render task, run on the loop task ----
  int keygenStart(uint32_t bits, uint8_t* pub, uint32_t pubCap, uint8_t* priv, uint32_t privCap) override;
  int netStart(const char* head, const uint8_t* body, uint32_t bn, uint8_t* resp, uint32_t cap) override;
  int fetchStart(const char* url, const char* dest) override;
  int textInputStart(const char* prompt, int flags, char* out, uint32_t cap) override;
  int poll() override;

  // ---- loop-task side, driven by DessenAppActivity ----
  bool hasPendingWork() const { return pending_ != Op::None && !done_; }
  bool wantsKeyboard() const { return pending_ == Op::TextInput && !done_; }
  // Run the queued network/keygen work (blocking) on the caller's (loop) task.
  void serviceAsyncWork();
  // The keyboard prompt the app asked for, and whether to mask it.
  const std::string& keyboardPrompt() const { return prompt_; }
  bool keyboardMasked() const { return inputFlags_ & 1; }
  // DessenAppActivity delivers the keyboard result (or cancellation) here.
  void deliverText(const char* text);
  void cancelText();

 private:
  enum class Op : uint8_t { None, Net, Fetch, Keygen, TextInput, Wifi };

  int resolve(const char* path, std::string& out) const;  // app "/x" -> real SD path
  bool deviceKek(uint8_t out[32]) const;                  // secure_wrap key

  std::string appName_, appDir_;
  std::unique_ptr<freeink::content::Crypto> crypto_;
  // The one custom font an app may load (replacing any previous one). The
  // manager owns the loaded font; unloadFont() unregisters it from the renderer.
  GfxRenderer* renderer_ = nullptr;
  freeink::ui::GfxRendererTarget* target_ = nullptr;
  std::unique_ptr<SdCardFontManager> fontManager_;
  bool customFontLoaded_ = false;
  static constexpr int kCustomFontId = 0x7F00;  // app font slot in the renderer's font map

  // Single outstanding async op. Buffers point into guest memory (valid while
  // the VM is parked) except `head_`/`url_`/`dest_`, which are copied.
  Op pending_ = Op::None;
  bool done_ = false;
  int result_ = 0;
  std::string head_, url_, dest_, prompt_;
  std::vector<uint8_t> body_;
  uint8_t* out_ = nullptr;  // net resp / keygen pub / text buffer
  uint32_t outCap_ = 0;
  uint8_t* priv_ = nullptr;  // keygen private buffer
  uint32_t privCap_ = 0;
  uint32_t bits_ = 0;
  unsigned long wifiStart_ = 0;
  int inputFlags_ = 0;
};

#endif  // CROSSPOINT_DESSEN_APPS
