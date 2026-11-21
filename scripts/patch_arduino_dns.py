"""Build Arduino 3.3.11's NetworkManager with the DNS cache clear in TCP/IP context."""

import json
from pathlib import Path


BEFORE = """    hasGlobalV6 = hasGlobalV6Now;
    hasGlobalV4 = hasGlobalV4Now;
    dns_clear_cache();"""

AFTER = """    // Clearing pending DNS requests can invoke the SNTP retry callback here.
    const esp_err_t status = esp_netif_tcpip_exec([](void *) -> esp_err_t {
      dns_clear_cache();
      return ESP_OK;
    }, nullptr);
    if (status != ESP_OK) {
      log_e("Failed to clear DNS cache: %s", esp_err_to_name(status));
      return 0;
    }
    hasGlobalV6 = hasGlobalV6Now;
    hasGlobalV4 = hasGlobalV4Now;"""


def patch_network_manager(env, node):
    framework = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32"))
    upstream = framework / "libraries/Network/src/NetworkManager.cpp"
    if Path(node.srcnode().get_abspath()).resolve() != upstream.resolve():
        return node
    source = upstream.read_text(encoding="utf-8")
    if json.loads((framework / "package.json").read_text())["version"] != "3.3.11" or source.count(BEFORE) != 1:
        raise RuntimeError("Review the Arduino DNS cache patch for this framework version")

    patched = source.replace(BEFORE, AFTER)
    target = Path(env.subst("$BUILD_DIR")) / "patched/NetworkManager.cpp"
    if not target.is_file() or target.read_text(encoding="utf-8") != patched:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(patched, encoding="utf-8")
    return env.File(str(target))


Import("env")
env.AddBuildMiddleware(patch_network_manager, "*NetworkManager.cpp")
