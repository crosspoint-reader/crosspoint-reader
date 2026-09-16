#!/usr/bin/env python3
"""Build/run Agentcloud pure tests and enforce source-level isolation contracts."""

from pathlib import Path
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
    centered_text = activity[activity.index("void AgentcloudActivity::renderCenteredText("):
                             activity.index("uint8_t AgentcloudActivity::drawWrappedText(")]
    require("renderer.drawCenteredText(NOTOSANS_18_FONT_ID, MESSAGE_TITLE_Y, title, true," in centered_text and
            "EpdFontFamily::REGULAR" in centered_text and
            "NOTOSANS_12_FONT_ID, EpdFontFamily::REGULAR" in centered_text and
            "MESSAGE_BODY_Y" in centered_text,
            "startup/all-clear states do not share the HM391 centered text layout")
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
            activity.index("else if (firstPaint || cleanupDue)"),
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

    render_row_text = activity[activity.index("void AgentcloudActivity::renderRowText("):
                               activity.index("void AgentcloudActivity::renderStateOnly(")]
    wrapped_text = activity[activity.index("uint8_t AgentcloudActivity::drawWrappedText("):
                            activity.index("void AgentcloudActivity::drawStateIcon(")]
    text_layer = activity[activity.index("void AgentcloudActivity::renderTextLayer()"):
                          activity.index("bool AgentcloudActivity::antiAliasedTextAvailable()")]
    for forbidden in ("fillRect(", "drawLine(", "drawStateIcon(", "clearScreen(", "displayBuffer("):
        require(forbidden not in centered_text + render_row_text + wrapped_text + text_layer,
                f"grayscale text layer contains non-text drawing: {forbidden}")
    require("renderCenteredText(" in text_layer and "renderRowText(" in text_layer,
            "text-only pass does not cover centered and dashboard-row states")

    aa_gate = activity[activity.index("bool AgentcloudActivity::antiAliasedTextAvailable() const"):
                       activity.index("bool AgentcloudActivity::renderAntiAliasedText(")]
    require("SETTINGS.textAntiAliasing != 0" in aa_gate and "capabilities.supported()" in aa_gate and
            "capabilities.stripUploads" in aa_gate and "spinnerOnly" not in aa_gate,
            "text AA availability is not based only on setting and overlay strip support")

    aa_render = activity[activity.index("bool AgentcloudActivity::renderAntiAliasedText("):
                         activity.index("void AgentcloudActivity::render(RenderLock&&)")]
    require("AA_STRIP_ROWS = 20" in activity and "AA_STRIP_BYTES == 2000" in activity,
            "text AA does not use 20 physical rows / 2000-byte strips")
    require("HalDisplay::DISPLAY_WIDTH_BYTES" in activity and
            "sizeof(((agentcloud::PayloadMessage*)nullptr)->bytes)" in activity,
            "AA strip size is not compile-time checked against panel and payload storage")
    require("reinterpret_cast<uint8_t*>(queuedPayload.bytes)" in aa_render,
            "AA strip scratch does not reuse the synchronized queue workspace")
    for allocation in ("malloc(", "calloc(", "realloc(", "new (", "makeUnique", "std::vector", "std::string"):
        require(allocation not in aa_render, f"AA render allocates via {allocation}")

    begin = aa_render.index("renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Overlay, baseMode)")
    lsb = aa_render.index("renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB)", begin)
    lsb_begin = aa_render.index("renderer.beginStripTarget(scratch, y, rows)", lsb)
    lsb_clear = aa_render.index("renderer.clearScreen(0x00)", lsb_begin)
    lsb_text = aa_render.index("renderTextLayer()", lsb_clear)
    lsb_end = aa_render.index("renderer.endStripTarget()", lsb_text)
    lsb_write = aa_render.index("renderer.writeGrayscalePlaneStrip(true, scratch, y, rows)", lsb_end)
    msb = aa_render.index("renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB)", lsb_write)
    msb_begin = aa_render.index("renderer.beginStripTarget(scratch, y, rows)", msb)
    msb_clear = aa_render.index("renderer.clearScreen(0x00)", msb_begin)
    msb_text = aa_render.index("renderTextLayer()", msb_clear)
    msb_end = aa_render.index("renderer.endStripTarget()", msb_text)
    msb_write = aa_render.index("renderer.writeGrayscalePlaneStrip(false, scratch, y, rows)", msb_end)
    gray_display = aa_render.index("renderer.displayGrayBuffer()", msb_write)
    bw_restore = aa_render.index("renderer.setRenderMode(GfxRenderer::BW)", gray_display)
    cleanup = aa_render.index("renderer.cleanupGrayscaleWithFrameBuffer()", bw_restore)
    success_return = aa_render.index("return true;", cleanup)
    require(begin < lsb < lsb_begin < lsb_clear < lsb_text < lsb_end < lsb_write < msb < msb_begin <
            msb_clear < msb_text < msb_end < msb_write < gray_display < bw_restore < cleanup < success_return,
            "AA base/plane/display/cleanup ordering is broken")
    require(aa_render.count("renderer.endStripTarget();") >= 4 and
            aa_render.count("renderer.setRenderMode(GfxRenderer::BW);") >= 3,
            "AA failure paths do not restore strip and BW state")
    require("partialRefreshCount = 0;" in aa_render and "forceFullRefresh = false;" in aa_render,
            "successful AA does not reset refresh bookkeeping")
    require("storeBwBuffer" not in aa_render, "dashboard AA allocates the 48KB reader backup")

    activity_loop = activity[activity.index("void AgentcloudActivity::loop()"):
                             activity.index("void AgentcloudActivity::renderMessage(")]
    payload_start = activity_loop.index("if (payloadQueue != nullptr && uxQueueMessagesWaiting(payloadQueue) != 0)")
    payload_lock = activity_loop.index("RenderLock stateLock;", payload_start)
    payload_unlock = activity_loop.index("\n    }\n    if (dashboardNeedsRender) requestUpdateAndWait();", payload_lock)
    payload_request = activity_loop.index("if (dashboardNeedsRender) requestUpdateAndWait();", payload_unlock)
    locked_payload = activity_loop[payload_lock:payload_unlock]
    require(activity_loop.index("uxQueueMessagesWaiting(payloadQueue)", payload_start) < payload_lock <
            locked_payload.index("xQueueReceive(payloadQueue, &queuedPayload, 0)") + payload_lock,
            "payload receive can block on RenderLock before confirming queue work")
    for mutation in ("parsePayload(queuedPayload.bytes", "parsedDashboard", "dashboard = parsedDashboard",
                     "dirtyRows =", "spinnerPhase", "spinnerOnly = false", "lastSpinnerStepMs"):
        require(mutation in locked_payload, f"payload state escapes RenderLock: {mutation}")
    require("requestUpdateAndWait" not in locked_payload and payload_unlock < payload_request,
            "payload render wait occurs while RenderLock is held")

    spinner_start = activity_loop.index("bool spinnerNeedsRender = false;")
    spinner_lock = activity_loop.index("RenderLock stateLock;", spinner_start)
    spinner_unlock = activity_loop.index("\n  }\n  if (spinnerNeedsRender) requestUpdateAndWait();", spinner_lock)
    spinner_request = activity_loop.index("if (spinnerNeedsRender) requestUpdateAndWait();", spinner_unlock)
    locked_spinner = activity_loop[spinner_lock:spinner_unlock]
    due_recheck = locked_spinner.index("agentcloud::spinnerDue(")
    for mutation in ("spinnerPhase =", "dirtyRows = activeRows", "spinnerOnly = true", "lastSpinnerStepMs = spinnerNow"):
        require(due_recheck < locked_spinner.index(mutation), f"spinner mutation precedes locked due recheck: {mutation}")
    require("requestUpdateAndWait" not in locked_spinner and spinner_unlock < spinner_request,
            "spinner render wait occurs while RenderLock is held")
    require(activity_loop.count("requestUpdateAndWait();") == 2,
            "Agentcloud loop has an unverified blocking render request")
    require("RenderLock" not in activity_loop[:payload_start],
            "button navigation is unnecessarily held behind RenderLock")
    advertising = activity_loop[activity_loop.index("const uint32_t now = millis();", spinner_request):]
    require("RenderLock" not in advertising and "maintainAdvertising()" in advertising,
            "BLE advertising maintenance is held behind RenderLock")

    dashboard_render = activity[activity.index("void AgentcloudActivity::render(RenderLock&&)"):
                                activity.index("#endif", activity.index("void AgentcloudActivity::render(RenderLock&&)"))]
    waiting_aa = dashboard_render.index("renderAntiAliasedText(HalDisplay::HALF_REFRESH)")
    waiting_bw = dashboard_render.index("renderer.displayBuffer(HalDisplay::HALF_REFRESH)", waiting_aa)
    cleanup_due = dashboard_render.index("const bool cleanupDue = agentcloud::partialCleanupDue(partialRefreshCount)")
    aa_available = dashboard_render.index("const bool aaAvailable = antiAliasedTextAvailable()", cleanup_due)
    aa_call = dashboard_render.index(
        "if (agentcloud::shouldRenderAntiAliasedText(aaAvailable, spinnerOnly, cleanupDue))", aa_available)
    base_choice = dashboard_render.index(
        "forceFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::HALF_REFRESH", aa_call)
    aa_success = dashboard_render.index("if (renderAntiAliasedText(baseMode))", base_choice)
    aa_return = dashboard_render.index("return;", aa_success)
    bw_full = dashboard_render.index("if (forceFullRefresh)", aa_success)
    partial_window = dashboard_render.index("renderer.displayWindow", bw_full)
    unsafe_retry = dashboard_render.index(
        "spinnerOnly && aaAvailable && renderAntiAliasedText(HalDisplay::HALF_REFRESH)", partial_window)
    unsafe_bw = dashboard_render.index("renderer.displayBuffer(HalDisplay::HALF_REFRESH)", unsafe_retry)
    require(waiting_aa < waiting_bw < cleanup_due < aa_available < aa_call < base_choice < aa_success < aa_return <
            bw_full < partial_window < unsafe_retry < unsafe_bw,
            "AA success/fallback does not precede the unchanged B/W refresh paths")
    require("FAST_REFRESH" not in aa_render and "FAST_REFRESH" not in dashboard_render[aa_call:bw_full],
            "dashboard text AA uses a differential FAST base")
    require("else if (firstPaint || cleanupDue)" in dashboard_render,
            "failed AA maintenance does not retain the B/W cleanup fallback")

    forced_refresh = activity_header[activity_header.index("bool handleForcedRefresh() override"):
                                     activity_header.index(" private:")]
    forced_lock = forced_refresh.index("RenderLock lock(*this);")
    forced_unlock = forced_refresh.index("\n    }\n    requestUpdate();", forced_lock)
    require(forced_lock < forced_refresh.index("dirtyRows = 0x0f;") < forced_unlock and
            forced_lock < forced_refresh.index("spinnerOnly = false;") < forced_unlock and
            "partialRefreshCount = agentcloud::MAX_PARTIAL_REFRESHES;" in forced_refresh,
            "manual refresh state is not synchronized before scheduling its redraw")
    require(forced_unlock < forced_refresh.index("requestUpdate();") and
            "requestUpdateAndWait" not in forced_refresh and
            "return true;" in forced_refresh,
            "manual refresh is not deferred or still allows main's immediate B/W refresh")
    require("virtual bool handleForcedRefresh() { return false; }" in activity_base,
            "default activities no longer retain the normal forced-refresh fallback")
    print("agentcloud source contracts passed")


if __name__ == "__main__":
    main()
