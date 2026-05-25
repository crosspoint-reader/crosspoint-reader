"""
PlatformIO pre-build script: patch WebSockets for Arduino-ESP32 3.x.

WebSockets 2.7.3 still calls WiFiClient::flush() in the client disconnect
path. On Arduino-ESP32 3.x, WiFiClient is a typedef for NetworkClient and
NetworkClient::flush() is deprecated in favor of clear(). The implementation
currently delegates flush() to clear(), so this patch preserves behavior while
removing the compiler warning.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os


OLD = """#if (WEBSOCKETS_NETWORK_TYPE != NETWORK_ESP8266_ASYNC)
            client->tcp->flush();
#endif"""

NEW = """#if (WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP32)
            client->tcp->clear();
#elif (WEBSOCKETS_NETWORK_TYPE != NETWORK_ESP8266_ASYNC)
            client->tcp->flush();
#endif"""


def patch_websockets(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    for env_dir in os.listdir(libdeps_dir):
        client_cpp = os.path.join(libdeps_dir, env_dir, "WebSockets", "src", "WebSocketsClient.cpp")
        if os.path.isfile(client_cpp):
            _patch_one(client_cpp)


def _patch_one(path):
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()
    if NEW in content:
        return
    if OLD not in content:
        raise RuntimeError("WebSockets patch does not apply cleanly: %s" % path)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(content.replace(OLD, NEW, 1))
    print("Applied WebSockets NetworkClient::clear patch")


patch_websockets(env)  # noqa: F821
