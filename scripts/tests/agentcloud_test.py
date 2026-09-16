#!/usr/bin/env python3
"""Build/run Agentcloud pure tests and enforce source-level isolation contracts."""

from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="agentcloud-test-") as temp_dir:
        executable = Path(temp_dir) / "agentcloud_protocol_test"
        subprocess.run(
            [
                "c++",
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                f"-I{ROOT / 'src/activities/agentcloud'}",
                f"-I{ROOT / 'lib/GfxRenderer'}",
                str(ROOT / "scripts/tests/agentcloud_protocol_test.cpp"),
                str(ROOT / "src/activities/agentcloud/AgentcloudProtocol.cpp"),
                "-o",
                str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)

    platformio = (ROOT / "platformio.ini").read_text()
    default_section, agentcloud_section = platformio.split("[env:agentcloud_x4]", maxsplit=1)
    require("AGENTCLOUD_DASHBOARD" not in default_section, "dashboard macro leaked into default environment")
    require("-DAGENTCLOUD_DASHBOARD=1" in agentcloud_section, "dashboard macro missing")
    require("h2zero/NimBLE-Arduino @ 2.3.8" in agentcloud_section, "NimBLE version is not pinned")
    require("board_upload.offset_address = 0x650000" in agentcloud_section, "agentcloud image does not target app1")
    require("'agentcloud_x4'" in (ROOT / "scripts/git_branch.py").read_text(),
            "agentcloud environment does not receive CROSSPOINT_VERSION")

    auth_header = (ROOT / "src/activities/agentcloud/AgentcloudAuth.h").read_text()
    require("{0, 0, 0, 0, 0, 0, 0, 0}" in auth_header, "default authenticator is not zero/invalid")
    require("AgentcloudAuth.local.h" in (ROOT / ".gitignore").read_text(), "local authenticator is not ignored")

    activity = (ROOT / "src/activities/agentcloud/AgentcloudActivity.cpp").read_text()
    activity_header = (ROOT / "src/activities/agentcloud/AgentcloudActivity.h").read_text()
    for forbidden in ("#include <WiFi", "Storage.", "Preferences", "nvs_", "Serial"):
        require(forbidden not in activity, f"forbidden Agentcloud dependency: {forbidden}")
    require("static AgentcloudBle singleton;" in activity, "BLE callbacks do not have process lifetime")
    require("std::unique_ptr<AgentcloudBle>" not in activity_header, "activity still owns BLE callback lifetime")
    require("AgentcloudBle* ble = nullptr;" in activity_header, "activity BLE pointer is not non-owning")
    require(activity.count("CallbackScope callback(parent);") == 3, "not every BLE callback is lifetime guarded")
    require("std::atomic<AgentcloudActivity*> owner{nullptr};" in activity, "BLE owner is not atomic")
    require("std::atomic<uint32_t> activeCallbacks{0};" in activity, "active callback count is not atomic")
    require(activity.index("owner.exchange(nullptr)") < activity.index("waitForCallbacks()"),
            "BLE owner is not detached before callback drain")
    require("while (activeCallbacks.load() != 0)" in activity, "callback drain can return before callbacks finish")
    require("CALLBACK_DRAIN_ATTEMPTS" not in activity, "callback drain still has a timeout")
    require(activity.index("NimBLEDevice::deinit(false)") < activity.index("NimBLEDevice::deinit(true)"),
            "NimBLE objects can be cleared before the host stops")
    on_exit = activity[activity.index("void AgentcloudActivity::onExit()"): 
                       activity.index("void AgentcloudActivity::stopBle()")]
    on_enter = activity[activity.index("void AgentcloudActivity::onEnter()"): 
                        activity.index("void AgentcloudActivity::onExit()")]
    require(on_exit.index("stopping.store(true)") < on_exit.index("stopBle()") < on_exit.index("vQueueDelete"),
            "owner detach/drain does not precede queue deletion")
    require("createCharacteristic(WRITE_UUID, NIMBLE_PROPERTY::WRITE, MAX_BLE_WRITE_BYTES)" in activity,
            "BLE write characteristic is not capped at 20 bytes")
    activity_base = (ROOT / "src/activities/Activity.h").read_text()
    activity_manager = (ROOT / "src/activities/ActivityManager.cpp").read_text()
    require("virtual bool honorsScreenInversion() const { return true; }" in activity_base,
            "default activities no longer honor saved inversion")
    require("bool honorsScreenInversion() const override { return false; }" in activity_header,
            "dashboard does not opt out of saved inversion")
    require("currentActivity->honorsScreenInversion()" in activity_manager and
            "renderer.setInverted" in activity_manager,
            "activity inversion policy is not applied before rendering")
    require("renderer.setInverted(false);" not in activity, "dashboard toggles inversion after render dispatch")
    require("xQueueCreate(1, sizeof(agentcloud::PayloadMessage))" in activity,
            "payload queue is not a single latest-wins slot")
    require("xQueueOverwrite(payloadQueue" in activity, "complete payloads do not overwrite the latest queue slot")
    require("NOTOSANS_14_FONT_ID" in activity and "NOTOSANS_12_FONT_ID" in activity,
            "dashboard does not use the HM391-sized Noto Sans pairing")
    centered_titles = re.findall(
        r"renderer\.drawCenteredText\(NOTOSANS_18_FONT_ID, MESSAGE_TITLE_Y,.*?EpdFontFamily::REGULAR\);",
        activity,
        re.DOTALL,
    )
    require(len(centered_titles) == 2,
            "startup/all-clear headings do not use the HM391 18px title face")
    require("UI_12_FONT_ID" not in activity and "UI_10_FONT_ID" not in activity,
            "legacy one-line dashboard typography remains")
    require("renderer.drawPixel(x + column, y + row, ink)" in activity,
            "state masks are not painted through the orientation-safe renderer")
    require("AgentcloudMaterialIcons.h" in activity, "Material state masks are not wired into the dashboard")
    require("textLineScratch" in activity_header, "word wrapping does not use activity-owned fixed scratch")
    for heap_type in ("std::string ", "std::vector<", "std::unique_ptr<", "String "):
        require(heap_type not in activity and heap_type not in activity_header,
                f"heap-owning render type present: {heap_type.strip()}")
    require("STR_AGENTCLOUD_ALL_CLEAR" in activity and "STR_AGENTCLOUD_WAITING_FOR_ACTIVITY" in activity,
            "all-clear dashboard composition is missing")
    require("renderer.setOrientation(GfxRenderer::Orientation::Portrait);" in on_enter,
            "dashboard does not lock its logical viewport to portrait")
    require("SETTINGS.orientation" not in on_enter and "SETTINGS.orientation" not in on_exit,
            "dashboard mutates the saved reader orientation")
    require("rect.y + rect.height - 1" not in activity,
            "dashboard still draws bottom-edge row separators")
    require("separatorY(index, rect)" in activity and
            "rect.x + rect.width - 1, separator, true" in activity,
            "dashboard separators are not black top-edge boundaries")
    require("TITLE_BODY_GAP" in activity, "dashboard body does not include the two-pixel title gap")
    require("hasNewSettledUnreadIdentity(dashboard, parsedDashboard)" in activity,
            "settled-unread identity transitions do not request attention")
    require(activity.index("if (forceFullRefresh)") <
            activity.index("else if (firstPaint || agentcloud::partialCleanupDue"),
            "attention full refresh does not take priority")
    require("renderer.displayBuffer(HalDisplay::FULL_REFRESH);" in activity,
            "attention transition does not use the full waveform")
    protocol_header = (ROOT / "src/activities/agentcloud/AgentcloudProtocol.h").read_text()
    require("bool recent = false;" in protocol_header and "finished" not in protocol_header,
            "bridge recency is still treated as completion")
    icons = (ROOT / "src/activities/agentcloud/AgentcloudMaterialIcons.h").read_text()
    require("Material Symbols" in icons and "Apache License 2.0" in icons,
            "Material Symbols provenance/license is missing")
    require(icons.count("inline constexpr uint8_t state_icon_") == 11,
            "dashboard must contain exactly eleven flash-resident icon masks")
    require("STATE_ICON_BYTES = 34 * ((34 + 7) / 8)" in icons,
            "state icon storage is not the exact 34x34 packed geometry")
    require(on_enter.index("releaseSdFontCaches") < on_enter.index("hasValidAuthenticator") <
            on_enter.index("xQueueCreate") < on_enter.index("ble->begin") < on_enter.rindex("requestUpdateAndWait"),
            "waiting screen can repopulate font caches before BLE initialization")
    require("screenRectToAlignedMemRect" in (ROOT / "lib/GfxRenderer/GfxRenderer.cpp").read_text(),
            "displayWindow bypasses the shared orientation mapper")
    print("agentcloud source contracts passed")


if __name__ == "__main__":
    main()
