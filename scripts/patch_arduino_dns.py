"""Run Arduino DNS cache invalidation in the lwIP TCP/IP context."""

from pathlib import Path

Import("env")  # noqa: F821

framework = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
source = Path(framework) / "libraries/Network/src/NetworkManager.cpp"
original = '''    hasGlobalV6 = hasGlobalV6Now;
    hasGlobalV4 = hasGlobalV4Now;
    dns_clear_cache();
    log_d("Clearing DNS cache");'''
replacement = '''    // Cache invalidation can invoke SNTP callbacks that change lwIP timers.
    const esp_err_t clearError = esp_netif_tcpip_exec([](void *) -> esp_err_t {
      dns_clear_cache();
      return ESP_OK;
    }, nullptr);
    if (clearError != ESP_OK) {
      log_e("Failed to clear DNS cache: %d", clearError);
      return 0;
    }
    hasGlobalV6 = hasGlobalV6Now;
    hasGlobalV4 = hasGlobalV4Now;
    log_d("Clearing DNS cache");'''

text = source.read_text()
if replacement in text:
    print("Arduino DNS cache context patch already applied")
elif text.count(original) == 1:
    source.write_text(text.replace(original, replacement))
    print("Patched Arduino DNS cache invalidation to use TCP/IP context")
else:
    raise RuntimeError("Unrecognized Arduino DNS implementation; review the context patch")
