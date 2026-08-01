#include "SshServer.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

// libssh include order follows the LibSSH-ESP32 examples: the Arduino glue
// header first, then the port config, then the libssh API headers.
#include "libssh_esp32.h"
// clang-format off
#include "libssh_esp32_config.h"
#include <libssh/libssh.h>
#include <libssh/server.h>
// clang-format on

#include <lwip/sockets.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <string_view>
#include <vector>

namespace {
constexpr const char* TAG = "SSH";
constexpr const char* HOST_KEY_PATH = "/.crosspoint/ssh_host_key";
constexpr const char* CACHE_DIR = "/.crosspoint";
constexpr const char* SSH_DIR = "/.ssh";
constexpr const char* AUTHORIZED_KEYS_PATH = "/.ssh/authorized_keys";

// Task sized from the LibSSH-ESP32 server example (10240) plus headroom for
// the SD write path that runs on this task during scp uploads.
constexpr uint32_t TASK_STACK_BYTES = 12288;

constexpr size_t TRANSFER_BUF_SIZE = 4096;
constexpr size_t MAX_LINE_LEN = 200;
constexpr int SHELL_POLL_MS = 200;     // shell read poll; bounds stop() latency
constexpr int SCP_TIMEOUT_MS = 15000;  // per-read timeout during scp transfers
constexpr int MAX_AUTH_ATTEMPTS = 3;

// Strip any directory components a client may send in an scp header.
const char* scpBasename(const char* name) {
  const char* slash = strrchr(name, '/');
  return slash ? slash + 1 : name;
}

// Resolve `path` against `cwd` into a normalized absolute path
// ("." and ".." components are folded, never escaping the root).
std::string resolvePath(const std::string& cwd, const char* path) {
  std::string joined;
  if (!path || path[0] == '\0') {
    joined = cwd;
  } else if (path[0] == '/') {
    joined = path;
  } else {
    joined = cwd;
    if (joined.back() != '/') {
      joined += '/';
    }
    joined += path;
  }

  std::string out;
  out.reserve(joined.size());
  size_t i = 0;
  while (i < joined.size()) {
    while (i < joined.size() && joined[i] == '/') {
      i++;
    }
    size_t j = joined.find('/', i);
    if (j == std::string::npos) {
      j = joined.size();
    }
    const std::string_view comp(joined.data() + i, j - i);
    if (comp.empty() || comp == ".") {
      // skip
    } else if (comp == "..") {
      const size_t slash = out.rfind('/');
      if (slash != std::string::npos) {
        out.resize(slash);
      }
    } else {
      out += '/';
      out.append(comp);
    }
    i = j;
  }
  if (out.empty()) {
    out = "/";
  }
  return out;
}

// Split `line` into argv tokens in place, honoring backslash escapes and
// single/double quotes, so names with spaces work: mv "My Book.epub" Books/
// Returns the number of tokens found (further input is left unparsed).
int tokenizeLine(char* line, char* argv[], int maxArgs) {
  int argc = 0;
  char* out = line;
  const char* p = line;
  while (*p && argc < maxArgs) {
    while (*p == ' ') {
      p++;
    }
    if (!*p) {
      break;
    }
    argv[argc++] = out;
    char quote = 0;
    while (*p && (quote || *p != ' ')) {
      if (*p == '\\' && p[1]) {
        p++;
        *out++ = *p++;
      } else if (!quote && (*p == '"' || *p == '\'')) {
        quote = *p++;
      } else if (quote && *p == quote) {
        quote = 0;
        p++;
      } else {
        *out++ = *p++;
      }
    }
    if (*p) {
      p++;  // skip the delimiter
    }
    *out++ = '\0';  // never passes p: out only falls behind on quotes/escapes
  }
  return argc;
}

// Remove backslash escapes and surrounding quotes in place (scp remote paths
// arrive shell-escaped, e.g. `scp -f /My\ Book.epub`).
void unescapeInPlace(char* s) {
  char* out = s;
  char quote = 0;
  for (const char* p = s; *p; p++) {
    if (*p == '\\' && p[1]) {
      *out++ = *++p;
    } else if (!quote && (*p == '"' || *p == '\'')) {
      quote = *p;
    } else if (quote && *p == quote) {
      quote = 0;
    } else {
      *out++ = *p;
    }
  }
  *out = '\0';
}
}  // namespace

SshServer::SshServer() { statusMutex = xSemaphoreCreateMutex(); }

SshServer::~SshServer() {
  stop();
  if (statusMutex) {
    vSemaphoreDelete(statusMutex);
    statusMutex = nullptr;
  }
}

bool SshServer::begin(const char* sessionPassword) {
  if (running.load()) {
    return true;
  }
  if (!sessionPassword || sessionPassword[0] == '\0') {
    LOG_ERR(TAG, "No session password supplied");
    return false;
  }
  snprintf(password, sizeof(password), "%s", sessionPassword);

  LOG_DBG(TAG, "Free heap before SSH start: %d bytes", ESP.getFreeHeap());

  static bool libsshInitialized = false;
  if (!libsshInitialized) {
    libssh_begin();
    libsshInitialized = true;
  }

  std::string hostKeyB64;
  if (!loadOrCreateHostKey(hostKeyB64)) {
    LOG_ERR(TAG, "No host key available");
    return false;
  }

  sshbind = ssh_bind_new();
  if (!sshbind) {
    LOG_ERR(TAG, "ssh_bind_new failed");
    return false;
  }

  unsigned int port = PORT;
  ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_BINDPORT, &port);
  if (ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_IMPORT_KEY_STR, hostKeyB64.c_str()) < 0) {
    LOG_ERR(TAG, "Host key import failed: %s", ssh_get_error(sshbind));
    ssh_bind_free(sshbind);
    sshbind = nullptr;
    return false;
  }

  if (ssh_bind_listen(sshbind) < 0) {
    LOG_ERR(TAG, "Listen failed: %s", ssh_get_error(sshbind));
    ssh_bind_free(sshbind);
    sshbind = nullptr;
    return false;
  }
  listenFd = ssh_bind_get_fd(sshbind);

  // Make the drop location for `scp id_rsa.pub ...:/.ssh/authorized_keys` exist.
  Storage.ensureDirectoryExists(SSH_DIR);
  loadAuthorizedKeys();

  stopRequested.store(false);
  running.store(true);
  const BaseType_t created = xTaskCreate(&taskTrampoline, "SshServer", TASK_STACK_BYTES, this, 1, &taskHandle);
  if (created != pdPASS) {
    LOG_ERR(TAG, "Failed to create server task");
    running.store(false);
    freeAuthorizedKeys();
    ssh_bind_free(sshbind);
    sshbind = nullptr;
    listenFd = -1;
    return false;
  }

  LOG_INF(TAG, "SSH server listening on port %u", PORT);
  return true;
}

void SshServer::stop() {
  if (!running.load()) {
    return;
  }
  stopRequested.store(true);
  // The task frees all libssh resources and clears `running` before deleting
  // itself. Shell/scp reads poll the stop flag at least every SHELL_POLL_MS.
  for (int i = 0; i < 100 && running.load(); i++) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (running.load()) {
    // A wedged client kept the task alive; the activity restarts the device on
    // exit anyway, so log it rather than yanking the task mid-malloc.
    LOG_ERR(TAG, "Server task did not stop in time");
  } else {
    LOG_DBG(TAG, "Free heap after SSH stop: %d bytes", ESP.getFreeHeap());
  }
}

SshServer::TransferStatus SshServer::getStatus() const {
  TransferStatus copy;
  if (xSemaphoreTake(statusMutex, portMAX_DELAY) == pdTRUE) {
    copy = status;
    xSemaphoreGive(statusMutex);
  }
  return copy;
}

void SshServer::setClientConnected(bool connected) {
  if (xSemaphoreTake(statusMutex, portMAX_DELAY) == pdTRUE) {
    status.clientConnected = connected;
    if (!connected) {
      status.inProgress = false;
      status.received = 0;
      status.total = 0;
      status.filename.clear();
    }
    xSemaphoreGive(statusMutex);
  }
}

void SshServer::setTransferProgress(const char* filename, size_t received, size_t total) {
  if (xSemaphoreTake(statusMutex, portMAX_DELAY) == pdTRUE) {
    status.inProgress = true;
    status.received = received;
    status.total = total;
    status.filename = filename;
    xSemaphoreGive(statusMutex);
  }
}

void SshServer::setTransferComplete(const char* filename) {
  if (xSemaphoreTake(statusMutex, portMAX_DELAY) == pdTRUE) {
    status.inProgress = false;
    status.received = 0;
    status.total = 0;
    status.filename.clear();
    // An empty name means the transfer was aborted: clear progress without
    // announcing a completed file.
    if (filename[0] != '\0') {
      status.lastCompleteName = filename;
      status.lastCompleteAt = millis();
    }
    xSemaphoreGive(statusMutex);
  }
}

void SshServer::taskTrampoline(void* param) { static_cast<SshServer*>(param)->serverTaskLoop(); }

void SshServer::serverTaskLoop() {
  while (!stopRequested.load()) {
    // ssh_bind_accept() blocks, so wait for a pending connection with select()
    // and keep polling the stop flag in between.
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(listenFd, &readSet);
    timeval tv = {.tv_sec = 0, .tv_usec = 500 * 1000};
    const int ready = select(listenFd + 1, &readSet, nullptr, nullptr, &tv);
    if (ready < 0) {
      LOG_ERR(TAG, "select failed: %d", errno);
      break;
    }
    if (ready > 0 && FD_ISSET(listenFd, &readSet)) {
      handleClient();
    }
    vTaskDelay(1);
  }

  if (sshbind) {
    ssh_bind_free(sshbind);
    sshbind = nullptr;
  }
  freeAuthorizedKeys();
  listenFd = -1;
  running.store(false);
  vTaskDelete(nullptr);
}

void SshServer::handleClient() {
  ssh_session session = ssh_new();
  if (!session) {
    LOG_ERR(TAG, "OOM: ssh session");
    return;
  }

  // Bound blocking libssh calls so a stalled client can't wedge the task.
  long timeoutSec = 30;
  ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeoutSec);

  if (ssh_bind_accept(sshbind, session) != SSH_OK) {
    LOG_ERR(TAG, "Accept failed: %s", ssh_get_error(sshbind));
    ssh_free(session);
    return;
  }

  LOG_DBG(TAG, "Client connected, free heap: %d bytes", ESP.getFreeHeap());

  if (ssh_handle_key_exchange(session) != SSH_OK) {
    LOG_ERR(TAG, "Key exchange failed: %s", ssh_get_error(session));
    ssh_disconnect(session);
    ssh_free(session);
    return;
  }

  if (authenticate(session)) {
    setClientConnected(true);
    ssh_channel channel = openChannel(session);
    if (channel) {
      serveChannel(session, channel);
      ssh_channel_free(channel);
    }
    setClientConnected(false);
  }

  ssh_disconnect(session);
  ssh_free(session);
  LOG_DBG(TAG, "Client disconnected, free heap: %d bytes, stack high water: %u", ESP.getFreeHeap(),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

void SshServer::loadAuthorizedKeys() {
  freeAuthorizedKeys();

  const String content = Storage.readFile(AUTHORIZED_KEYS_PATH);
  if (content.length() == 0) {
    LOG_DBG(TAG, "No %s; public key auth disabled", AUTHORIZED_KEYS_PATH);
    return;
  }
  authorizedKeys.reserve(4);

  // Standard authorized_keys format: "<type> <base64> [comment]" per line.
  const std::string text(content.c_str());
  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) {
      eol = text.size();
    }
    std::string lineStr = text.substr(pos, eol - pos);
    pos = eol + 1;
    if (!lineStr.empty() && lineStr.back() == '\r') {
      lineStr.pop_back();
    }
    if (lineStr.empty() || lineStr[0] == '#') {
      continue;
    }

    char* savePtr = nullptr;
    const char* typeName = strtok_r(lineStr.data(), " ", &savePtr);
    const char* b64 = strtok_r(nullptr, " ", &savePtr);
    if (!typeName || !b64) {
      continue;
    }
    const enum ssh_keytypes_e type = ssh_key_type_from_name(typeName);
    if (type == SSH_KEYTYPE_UNKNOWN) {
      LOG_ERR(TAG, "authorized_keys: unsupported key type '%s'", typeName);
      continue;
    }
    ssh_key key = nullptr;
    if (ssh_pki_import_pubkey_base64(b64, type, &key) == SSH_OK && key) {
      authorizedKeys.push_back(key);
    } else {
      LOG_ERR(TAG, "authorized_keys: failed to parse a '%s' key", typeName);
    }
  }
  LOG_INF(TAG, "Loaded %u authorized public key(s)", static_cast<unsigned>(authorizedKeys.size()));
}

void SshServer::freeAuthorizedKeys() {
  for (ssh_key key : authorizedKeys) {
    ssh_key_free(key);
  }
  authorizedKeys.clear();
}

bool SshServer::isAuthorizedKey(ssh_key clientKey) const {
  for (ssh_key key : authorizedKeys) {
    if (ssh_key_cmp(clientKey, key, SSH_KEY_CMP_PUBLIC) == 0) {
      return true;
    }
  }
  return false;
}

bool SshServer::authenticate(ssh_session session) {
  const int methods = SSH_AUTH_METHOD_PASSWORD | (authorizedKeys.empty() ? 0 : SSH_AUTH_METHOD_PUBLICKEY);
  int attempts = 0;
  ssh_message message = nullptr;
  while (!stopRequested.load() && (message = ssh_message_get(session)) != nullptr) {
    bool authenticated = false;
    bool rejected = false;
    if (ssh_message_type(message) == SSH_REQUEST_AUTH) {
      const int method = ssh_message_subtype(message);
      const char* user = ssh_message_auth_user(message);
      const bool userOk = user && strcmp(user, USERNAME) == 0;

      // The suggested replacements (callback-based auth_*_function) need the
      // ssh_event polling model; the message-loop server keeps these accessors.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
      if (method == SSH_AUTH_METHOD_PASSWORD) {
        const char* pass = ssh_message_auth_password(message);
        if (userOk && pass && strcmp(pass, password) == 0) {
          ssh_message_auth_reply_success(message, 0);
          authenticated = true;
        } else {
          rejected = true;
          attempts++;
          LOG_DBG(TAG, "Password failure %d for user '%s'", attempts, user ? user : "?");
          vTaskDelay(pdMS_TO_TICKS(500));  // slow down brute-force attempts
        }
      } else if (method == SSH_AUTH_METHOD_PUBLICKEY) {
        ssh_key clientKey = ssh_message_auth_pubkey(message);
        if (userOk && clientKey && isAuthorizedKey(clientKey)) {
          if (ssh_message_auth_publickey_state(message) == SSH_PUBLICKEY_STATE_VALID) {
            // libssh verified the signature against this key.
            ssh_message_auth_reply_success(message, 0);
            authenticated = true;
            LOG_INF(TAG, "Public key auth for '%s'", user);
          } else {
            // Probe for a known key: invite the signed request.
            ssh_message_auth_reply_pk_ok_simple(message);
          }
        } else {
          // Unknown-key probes are normal (clients try every key they hold),
          // so they are rejected without counting toward the attempt limit.
          rejected = true;
        }
      } else {
        rejected = true;
      }
#pragma GCC diagnostic pop

      if (rejected) {
        ssh_message_auth_set_methods(message, methods);
        ssh_message_reply_default(message);
      }
    } else {
      ssh_message_reply_default(message);
    }
    ssh_message_free(message);
    if (authenticated) {
      return true;
    }
    if (attempts >= MAX_AUTH_ATTEMPTS) {
      break;
    }
  }
  return false;
}

ssh_channel SshServer::openChannel(ssh_session session) {
  ssh_message message = nullptr;
  while (!stopRequested.load() && (message = ssh_message_get(session)) != nullptr) {
    ssh_channel channel = nullptr;
    if (ssh_message_type(message) == SSH_REQUEST_CHANNEL_OPEN && ssh_message_subtype(message) == SSH_CHANNEL_SESSION) {
      channel = ssh_message_channel_request_open_reply_accept(message);
    } else {
      ssh_message_reply_default(message);
    }
    ssh_message_free(message);
    if (channel) {
      return channel;
    }
  }
  return nullptr;
}

void SshServer::serveChannel(ssh_session session, ssh_channel channel) {
  // Wait for the client to tell us what it wants: an interactive shell, or an
  // exec request (scp, or a one-shot command).
  bool shell = false;
  auto execCmd = makeUniqueNoThrow<char[]>(MAX_LINE_LEN + 1);
  if (!execCmd) {
    LOG_ERR(TAG, "OOM: exec buffer");
    return;
  }
  execCmd[0] = '\0';

  ssh_message message = nullptr;
  while (!shell && execCmd[0] == '\0' && !stopRequested.load() && (message = ssh_message_get(session)) != nullptr) {
    if (ssh_message_type(message) == SSH_REQUEST_CHANNEL) {
      const int subtype = ssh_message_subtype(message);
      if (subtype == SSH_CHANNEL_REQUEST_SHELL) {
        shell = true;
        ssh_message_channel_request_reply_success(message);
      } else if (subtype == SSH_CHANNEL_REQUEST_PTY) {
        ssh_message_channel_request_reply_success(message);
      } else if (subtype == SSH_CHANNEL_REQUEST_EXEC) {
        const char* cmd = ssh_message_channel_request_command(message);
        if (cmd) {
          snprintf(execCmd.get(), MAX_LINE_LEN + 1, "%s", cmd);
        }
        ssh_message_channel_request_reply_success(message);
      } else {
        // env, window-change, etc. - acknowledge and ignore
        ssh_message_channel_request_reply_success(message);
      }
    } else {
      ssh_message_reply_default(message);
    }
    ssh_message_free(message);
  }

  int exitStatus = 0;
  if (shell) {
    runShell(channel);
  } else if (execCmd[0] != '\0') {
    LOG_DBG(TAG, "exec: %s", execCmd.get());
    // scp runs as an exec request: "scp -t <target>" (upload to us) or
    // "scp -f <path>" (download from us).
    if (strncmp(execCmd.get(), "scp ", 4) == 0) {
      // Flags always precede the -t/-f mode switch; everything after it is
      // the path (possibly containing spaces, escapes, or quotes).
      char mode = '\0';
      char* target = nullptr;
      char* modeFlag = strstr(execCmd.get(), " -t ");
      if (modeFlag) {
        mode = 't';
      } else if ((modeFlag = strstr(execCmd.get(), " -f ")) != nullptr) {
        mode = 'f';
      }
      bool recursive = false;
      if (modeFlag) {
        target = modeFlag + 4;
        *modeFlag = '\0';  // limit the -r scan to the flags before -t/-f
        recursive = strstr(execCmd.get(), " -r") != nullptr;
        unescapeInPlace(target);
      }
      if (recursive || !mode || !target || target[0] == '\0') {
        channelPrintf(channel, "\x02scp: only single-file transfers are supported\n");
        exitStatus = 1;
      } else if (mode == 't') {
        scpSink(channel, target);
      } else {
        scpSource(channel, target);
      }
    } else {
      ShellContext ctx;  // one-shot exec commands run from the root
      executeCommand(channel, ctx, execCmd.get(), false);
    }
  }

  ssh_channel_request_send_exit_status(channel, exitStatus);
  ssh_channel_send_eof(channel);
  ssh_channel_close(channel);
}

// ---------------------------------------------------------------------------
// Interactive shell
// ---------------------------------------------------------------------------

void SshServer::printPrompt(ssh_channel channel, const ShellContext& ctx) {
  channelPrintf(channel, "%s:%s> ", USERNAME, ctx.cwd.c_str());
}

void SshServer::runShell(ssh_channel channel) {
  auto line = makeUniqueNoThrow<char[]>(MAX_LINE_LEN + 1);
  if (!line) {
    LOG_ERR(TAG, "OOM: shell line buffer");
    return;
  }
  size_t lineLen = 0;
  ShellContext ctx;

  // ANSI escape-sequence parser state: 0 = normal, 1 = got ESC, 2 = in CSI.
  // Arrow keys etc. arrive as ESC [ <final>; swallow them instead of letting
  // their printable bytes land in the line buffer.
  int escState = 0;

  channelPrintf(channel, "CrossPoint Reader SSH - type 'help' for commands.\r\n");
  printPrompt(channel, ctx);

  while (!stopRequested.load() && ssh_channel_is_open(channel) && !ssh_channel_is_eof(channel)) {
    char ch = 0;
    const int n = ssh_channel_read_timeout(channel, &ch, 1, 0, SHELL_POLL_MS);
    if (n == SSH_ERROR) {
      break;
    }
    if (n <= 0) {
      continue;  // timeout - poll stop flag again
    }

    if (escState == 1) {
      escState = (ch == '[') ? 2 : 0;
      continue;
    }
    if (escState == 2) {
      if (ch >= 0x40 && ch <= 0x7e) {  // CSI final byte
        escState = 0;
      }
      continue;
    }

    if (ch == 0x1b) {  // ESC
      escState = 1;
    } else if (ch == '\r' || ch == '\n') {
      channelPrintf(channel, "\r\n");
      line[lineLen] = '\0';
      if (lineLen > 0 && !executeCommand(channel, ctx, line.get(), true)) {
        break;
      }
      lineLen = 0;
      printPrompt(channel, ctx);
    } else if (ch == '\t') {
      handleTabCompletion(channel, ctx, line.get(), lineLen);
    } else if (ch == 0x03) {  // ctrl-C: discard the current line
      channelPrintf(channel, "^C\r\n");
      printPrompt(channel, ctx);
      lineLen = 0;
    } else if (ch == 0x04) {  // ctrl-D on an empty line: exit
      if (lineLen == 0) {
        break;
      }
    } else if (ch == 0x08 || ch == 0x7f) {  // backspace
      if (lineLen > 0) {
        lineLen--;
        channelPrintf(channel, "\b \b");
      }
    } else if (ch >= 0x20 && lineLen < MAX_LINE_LEN) {
      line[lineLen++] = ch;
      channelPrintf(channel, "%c", ch);  // echo
    }
  }
}

void SshServer::handleTabCompletion(ssh_channel channel, const ShellContext& ctx, char* line, size_t& lineLen) {
  line[lineLen] = '\0';

  // The token being completed runs from the last unescaped space to the end
  // of the line ("My\ Year..." is one token).
  size_t tokenStart = lineLen;
  while (tokenStart > 0 && !(line[tokenStart - 1] == ' ' && (tokenStart < 2 || line[tokenStart - 2] != '\\'))) {
    tokenStart--;
  }
  const bool completeCommand = (tokenStart == 0);

  // Matching happens in unescaped space; the line keeps the escaped form.
  std::string token(line + tokenStart);
  unescapeInPlace(token.data());
  token.resize(strlen(token.c_str()));

  // Transient, bounded (MAX_MATCHES entries of one filename each) and freed on
  // return; a fixed pool would complicate the two string-length dimensions.
  constexpr size_t MAX_MATCHES = 32;
  std::vector<std::string> matches;
  matches.reserve(8);

  size_t prefixLen = 0;  // length of the (unescaped) part already typed

  if (completeCommand) {
    static constexpr const char* COMMANDS[] = {"ls", "cat", "rm", "mv", "mkdir", "cd", "pwd", "free", "help", "exit"};
    prefixLen = token.size();
    for (const char* cmd : COMMANDS) {
      if (strncmp(cmd, token.c_str(), prefixLen) == 0) {
        matches.emplace_back(cmd);
      }
    }
  } else {
    // Split the token into a directory part and a name prefix.
    std::string dirPath;
    std::string namePrefix;
    const size_t slash = token.rfind('/');
    if (slash != std::string::npos) {
      dirPath = resolvePath(ctx.cwd, token.substr(0, slash + 1).c_str());
      namePrefix = token.substr(slash + 1);
    } else {
      dirPath = ctx.cwd;
      namePrefix = token;
    }
    prefixLen = namePrefix.size();

    HalFile dir = Storage.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) {
      return;
    }
    auto name = makeUniqueNoThrow<char[]>(128);
    if (!name) {
      LOG_ERR(TAG, "OOM: completion buffer");
      return;
    }
    while (matches.size() < MAX_MATCHES && !stopRequested.load()) {
      HalFile entry = dir.openNextFile();
      if (!entry) {
        break;
      }
      entry.getName(name.get(), 128);
      if (strncmp(name.get(), namePrefix.c_str(), prefixLen) == 0) {
        // Directories get a trailing '/' so completion can descend into them.
        matches.emplace_back(std::string(name.get()) + (entry.isDirectory() ? "/" : ""));
      }
    }
  }

  if (matches.empty()) {
    channelPrintf(channel, "\a");  // bell
    return;
  }

  // Extend the line by the longest common prefix of all matches.
  std::string common = matches[0];
  for (const auto& m : matches) {
    size_t k = 0;
    while (k < common.size() && k < m.size() && common[k] == m[k]) {
      k++;
    }
    common.resize(k);
  }

  // Insert in escaped form so the result parses back as a single token.
  const auto appendChar = [&](char c) {
    const bool needsEscape = (c == ' ' || c == '\\' || c == '"' || c == '\'');
    if (lineLen + (needsEscape ? 2u : 1u) > MAX_LINE_LEN) {
      return false;
    }
    if (needsEscape) {
      line[lineLen++] = '\\';
      channelPrintf(channel, "\\");
    }
    line[lineLen++] = c;
    channelPrintf(channel, "%c", c);
    return true;
  };

  bool extended = false;
  for (size_t k = prefixLen; k < common.size() && appendChar(common[k]); k++) {
    extended = true;
  }

  if (matches.size() == 1) {
    // Unique completion: follow with a space unless it is a directory.
    if (common.back() != '/' && lineLen < MAX_LINE_LEN) {
      line[lineLen++] = ' ';
      channelPrintf(channel, " ");
    }
  } else if (!extended) {
    // Nothing more to add automatically: show the candidates.
    channelPrintf(channel, "\r\n");
    for (const auto& m : matches) {
      channelPrintf(channel, "%s  ", m.c_str());
    }
    channelPrintf(channel, "\r\n");
    line[lineLen] = '\0';
    printPrompt(channel, ctx);
    channelPrintf(channel, "%s", line);
  }
}

bool SshServer::executeCommand(ssh_channel channel, ShellContext& ctx, char* line, bool interactive) {
  char* argv[4] = {nullptr, nullptr, nullptr, nullptr};
  const int argc = tokenizeLine(line, argv, 4);
  if (argc == 0) {
    return true;
  }
  const char* cmd = argv[0];
  const char* arg1 = argc > 1 ? argv[1] : nullptr;
  const char* arg2 = argc > 2 ? argv[2] : nullptr;
  if (argc > 3) {
    // Almost always an unquoted name with spaces, not a real 4-argument call.
    channelPrintf(channel, "%s: too many arguments (quote names with spaces: mv \"My Book.epub\" dir/)\r\n", cmd);
    return true;
  }

  if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0 || strcmp(cmd, "logout") == 0) {
    return false;
  }

  if (strcmp(cmd, "help") == 0) {
    // Split into chunks: channelPrintf formats into a fixed 224-byte buffer.
    channelPrintf(channel,
                  "Commands (tab completes; quote or \\-escape names with spaces):\r\n"
                  "  ls [path]        list directory\r\n"
                  "  cd [path]        change directory\r\n"
                  "  pwd              print working directory\r\n"
                  "  cat <file>       print file contents\r\n");
    channelPrintf(channel,
                  "  rm <path>        delete file or empty directory\r\n"
                  "  mv <src> <dst>   move/rename\r\n"
                  "  mkdir <path>     create directory\r\n"
                  "  free             show free heap\r\n"
                  "  exit             close the session\r\n");
    channelPrintf(channel, "Upload:   scp book.epub %s@<ip>:/\r\n", USERNAME);
    channelPrintf(channel, "Download: scp %s@<ip>:/book.epub .\r\n", USERNAME);
  } else if (strcmp(cmd, "ls") == 0) {
    commandLs(channel, resolvePath(ctx.cwd, arg1).c_str());
  } else if (strcmp(cmd, "cd") == 0) {
    const std::string path = arg1 ? resolvePath(ctx.cwd, arg1) : "/";
    HalFile dir = Storage.open(path.c_str());
    if (dir && dir.isDirectory()) {
      ctx.cwd = path;
    } else {
      channelPrintf(channel, "cd: not a directory: %s\r\n", path.c_str());
    }
  } else if (strcmp(cmd, "pwd") == 0) {
    channelPrintf(channel, "%s\r\n", ctx.cwd.c_str());
  } else if (strcmp(cmd, "cat") == 0) {
    if (arg1) {
      commandCat(channel, resolvePath(ctx.cwd, arg1).c_str());
    } else {
      channelPrintf(channel, "usage: cat <file>\r\n");
    }
  } else if (strcmp(cmd, "rm") == 0) {
    if (!arg1) {
      channelPrintf(channel, "usage: rm <path>\r\n");
    } else {
      const std::string path = resolvePath(ctx.cwd, arg1);
      if (Storage.remove(path.c_str()) || Storage.rmdir(path.c_str())) {
        channelPrintf(channel, "removed %s\r\n", path.c_str());
      } else {
        channelPrintf(channel, "rm: cannot remove %s\r\n", path.c_str());
      }
    }
  } else if (strcmp(cmd, "mv") == 0) {
    if (!arg1 || !arg2) {
      channelPrintf(channel, "usage: mv <src> <dst>\r\n");
    } else {
      const std::string src = resolvePath(ctx.cwd, arg1);
      std::string dst = resolvePath(ctx.cwd, arg2);
      // Moving into a directory: append the source name, as rename() itself
      // only accepts a full destination path.
      HalFile dstDir = Storage.open(dst.c_str());
      if (dstDir && dstDir.isDirectory()) {
        if (dst.back() != '/') {
          dst += '/';
        }
        dst += src.substr(src.rfind('/') + 1);
      }
      dstDir.close();  // release the handle before renaming into the directory
      if (Storage.rename(src.c_str(), dst.c_str())) {
        channelPrintf(channel, "%s -> %s\r\n", src.c_str(), dst.c_str());
      } else {
        channelPrintf(channel, "mv: cannot move %s to %s\r\n", src.c_str(), dst.c_str());
      }
    }
  } else if (strcmp(cmd, "mkdir") == 0) {
    if (!arg1) {
      channelPrintf(channel, "usage: mkdir <path>\r\n");
    } else if (Storage.mkdir(resolvePath(ctx.cwd, arg1).c_str())) {
      channelPrintf(channel, "created %s\r\n", arg1);
    } else {
      channelPrintf(channel, "mkdir: failed\r\n");
    }
  } else if (strcmp(cmd, "free") == 0) {
    channelPrintf(channel, "free heap: %d bytes\r\n", ESP.getFreeHeap());
  } else {
    channelPrintf(channel, "unknown command: %s%s\r\n", cmd, interactive ? " (try 'help')" : "");
  }
  return true;
}

void SshServer::commandLs(ssh_channel channel, const char* path) {
  HalFile dir = Storage.open(path);
  if (!dir || !dir.isDirectory()) {
    channelPrintf(channel, "ls: not a directory: %s\r\n", path);
    return;
  }
  auto name = makeUniqueNoThrow<char[]>(128);
  if (!name) {
    LOG_ERR(TAG, "OOM: ls buffer");
    return;
  }
  while (!stopRequested.load()) {
    HalFile entry = dir.openNextFile();
    if (!entry) {
      break;
    }
    entry.getName(name.get(), 128);
    if (entry.isDirectory()) {
      channelPrintf(channel, "%10s  %s/\r\n", "<dir>", name.get());
    } else {
      channelPrintf(channel, "%10u  %s\r\n", static_cast<unsigned>(entry.fileSize()), name.get());
    }
  }
}

void SshServer::commandCat(ssh_channel channel, const char* path) {
  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file)) {
    channelPrintf(channel, "cat: cannot open %s\r\n", path);
    return;
  }
  auto buf = makeUniqueNoThrow<char[]>(512);
  if (!buf) {
    LOG_ERR(TAG, "OOM: cat buffer");
    return;
  }
  int n = 0;
  while (!stopRequested.load() && (n = file.read(buf.get(), 512)) > 0) {
    if (ssh_channel_write(channel, buf.get(), n) == SSH_ERROR) {
      break;
    }
  }
  channelPrintf(channel, "\r\n");
}

// ---------------------------------------------------------------------------
// scp (single files only; -r is rejected up front)
// ---------------------------------------------------------------------------

namespace {
// Read one scp control line ("C0644 <size> <name>\n", "T...", "E") byte by
// byte. Returns length, 0 on clean EOF, -1 on error/timeout.
int readScpLine(ssh_channel channel, char* buf, size_t bufSize) {
  size_t len = 0;
  while (len + 1 < bufSize) {
    char ch = 0;
    const int n = ssh_channel_read_timeout(channel, &ch, 1, 0, SCP_TIMEOUT_MS);
    if (n == 0) {
      return len == 0 ? 0 : -1;  // EOF
    }
    if (n < 0) {
      return -1;
    }
    if (ch == '\n') {
      buf[len] = '\0';
      return static_cast<int>(len);
    }
    buf[len++] = ch;
  }
  return -1;  // control line too long
}

bool scpAck(ssh_channel channel) {
  const char zero = 0;
  return ssh_channel_write(channel, &zero, 1) != SSH_ERROR;
}
}  // namespace

void SshServer::scpSink(ssh_channel channel, const char* target) {
  auto line = makeUniqueNoThrow<char[]>(MAX_LINE_LEN + 1);
  auto buf = makeUniqueNoThrow<uint8_t[]>(TRANSFER_BUF_SIZE);
  if (!line || !buf) {
    LOG_ERR(TAG, "OOM: scp buffers");
    return;
  }

  // Uploads land in `target` when it is a directory, otherwise at `target`.
  bool targetIsDir = strcmp(target, "/") == 0;
  if (!targetIsDir) {
    HalFile t = Storage.open(target);
    targetIsDir = t && t.isDirectory();
  }

  if (!scpAck(channel)) {
    return;
  }

  while (!stopRequested.load()) {
    const int lineLen = readScpLine(channel, line.get(), MAX_LINE_LEN + 1);
    if (lineLen <= 0) {
      return;  // clean EOF after last file, or error
    }

    if (line[0] == 'T' || line[0] == 'E') {
      // Timestamps / end-of-directory: acknowledge and ignore.
      if (!scpAck(channel)) {
        return;
      }
      continue;
    }
    if (line[0] == 'D') {
      channelPrintf(channel, "\x02scp: directories are not supported\n");
      return;
    }
    if (line[0] != 'C') {
      channelPrintf(channel, "\x02scp: unexpected control record\n");
      return;
    }

    // "C0644 <size> <name>"
    char* sizeStart = strchr(line.get(), ' ');
    char* nameStart = sizeStart ? strchr(sizeStart + 1, ' ') : nullptr;
    if (!nameStart) {
      channelPrintf(channel, "\x02scp: malformed file header\n");
      return;
    }
    const size_t fileSize = strtoul(sizeStart + 1, nullptr, 10);
    const char* baseName = scpBasename(nameStart + 1);
    if (baseName[0] == '\0' || strstr(baseName, "..") != nullptr) {
      channelPrintf(channel, "\x02scp: bad file name\n");
      return;
    }

    std::string destPath;
    if (targetIsDir) {
      destPath = target;
      if (destPath.back() != '/') {
        destPath += '/';
      }
      destPath += baseName;
    } else {
      destPath = target;
    }

    if (Storage.exists(destPath.c_str())) {
      Storage.remove(destPath.c_str());
    }
    HalFile file;
    if (!Storage.openFileForWrite(TAG, destPath, file)) {
      channelPrintf(channel, "\x02scp: cannot open %s for writing\n", destPath.c_str());
      return;
    }
    if (!scpAck(channel)) {
      return;
    }

    LOG_INF(TAG, "scp upload: %s (%u bytes)", destPath.c_str(), static_cast<unsigned>(fileSize));
    setTransferProgress(baseName, 0, fileSize);

    size_t received = 0;
    bool ok = true;
    while (received < fileSize && !stopRequested.load()) {
      const size_t want = std::min(fileSize - received, TRANSFER_BUF_SIZE);
      const int n = ssh_channel_read_timeout(channel, buf.get(), want, 0, SCP_TIMEOUT_MS);
      if (n <= 0) {
        ok = false;
        break;
      }
      if (file.write(buf.get(), n) != static_cast<size_t>(n)) {
        channelPrintf(channel, "\x02scp: write failed (SD card full?)\n");
        ok = false;
        break;
      }
      received += n;
      setTransferProgress(baseName, received, fileSize);
    }

    // Close (and flush) before reporting success or removing the partial file.
    file.close();
    if (!ok || stopRequested.load()) {
      Storage.remove(destPath.c_str());  // drop the partial file
      setTransferComplete("");
      return;
    }

    // Sender follows the data with a status byte; consume it and acknowledge.
    char senderStatus = 0;
    ssh_channel_read_timeout(channel, &senderStatus, 1, 0, SCP_TIMEOUT_MS);
    if (!scpAck(channel)) {
      return;
    }
    setTransferComplete(baseName);
    LOG_INF(TAG, "scp upload complete: %s", destPath.c_str());
  }
}

void SshServer::scpSource(ssh_channel channel, const char* path) {
  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file)) {
    channelPrintf(channel, "\x02scp: no such file: %s\n", path);
    return;
  }
  auto buf = makeUniqueNoThrow<uint8_t[]>(TRANSFER_BUF_SIZE);
  if (!buf) {
    LOG_ERR(TAG, "OOM: scp buffer");
    return;
  }

  // The receiving side starts the exchange with an ack.
  char clientStatus = 0;
  if (ssh_channel_read_timeout(channel, &clientStatus, 1, 0, SCP_TIMEOUT_MS) <= 0 || clientStatus != 0) {
    return;
  }

  const size_t fileSize = file.fileSize();
  channelPrintf(channel, "C0644 %u %s\n", static_cast<unsigned>(fileSize), scpBasename(path));
  if (ssh_channel_read_timeout(channel, &clientStatus, 1, 0, SCP_TIMEOUT_MS) <= 0 || clientStatus != 0) {
    return;
  }

  LOG_INF(TAG, "scp download: %s (%u bytes)", path, static_cast<unsigned>(fileSize));
  setTransferProgress(scpBasename(path), 0, fileSize);

  size_t sent = 0;
  int n = 0;
  while (!stopRequested.load() && (n = file.read(buf.get(), TRANSFER_BUF_SIZE)) > 0) {
    int written = 0;
    while (written < n) {
      const int w = ssh_channel_write(channel, buf.get() + written, n - written);
      if (w == SSH_ERROR) {
        setTransferComplete("");
        return;
      }
      written += w;
    }
    sent += n;
    setTransferProgress(scpBasename(path), sent, fileSize);
  }

  scpAck(channel);
  ssh_channel_read_timeout(channel, &clientStatus, 1, 0, SCP_TIMEOUT_MS);
  setTransferComplete(scpBasename(path));
  LOG_INF(TAG, "scp download complete: %s", path);
}

// ---------------------------------------------------------------------------
// Host key
// ---------------------------------------------------------------------------

bool SshServer::loadOrCreateHostKey(std::string& b64Key) {
  const String cached = Storage.readFile(HOST_KEY_PATH);
  if (cached.length() > 0) {
    b64Key = cached.c_str();
    return true;
  }

  LOG_INF(TAG, "Generating ed25519 host key...");
  ssh_key key = nullptr;
  if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK) {
    LOG_ERR(TAG, "Host key generation failed");
    return false;
  }
  char* b64 = nullptr;
  const int rc = ssh_pki_export_privkey_base64(key, nullptr, nullptr, nullptr, &b64);
  ssh_key_free(key);
  if (rc != SSH_OK || !b64) {
    LOG_ERR(TAG, "Host key export failed");
    return false;
  }
  b64Key = b64;
  ssh_string_free_char(b64);

  Storage.ensureDirectoryExists(CACHE_DIR);
  if (!Storage.writeFile(HOST_KEY_PATH, b64Key.c_str())) {
    // Not fatal: the key still works, clients just see a new host key next time.
    LOG_ERR(TAG, "Failed to persist host key to %s", HOST_KEY_PATH);
  }
  return true;
}

void SshServer::channelPrintf(ssh_channel channel, const char* fmt, ...) {
  char buf[224];
  va_list args;
  va_start(args, fmt);
  const int len = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (len > 0) {
    ssh_channel_write(channel, buf, std::min(static_cast<size_t>(len), sizeof(buf) - 1));
  }
}
