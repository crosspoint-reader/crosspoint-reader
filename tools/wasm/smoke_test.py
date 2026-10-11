#!/usr/bin/env python3
"""Exercise the built firmware through the browser's public controls."""

import argparse
import json
from pathlib import Path
import shutil
import tempfile
import time
from urllib.error import URLError
from urllib.request import urlopen

from playwright.sync_api import sync_playwright

from sd_card_test import exercise_sd_card, exercise_card_lifecycle, exercise_restart

ROOT = Path(__file__).resolve().parents[2]
BUTTONS = {"confirm": "Confirm", "back": "Back", "next": "↓ Next page", "previous": "↑ Previous page", "home": "Home button"}


def log_count(page, text):
    return page.locator("#log-output").text_content().count(text)


def wait_log(page, text, previous=0):
    page.wait_for_function(
        "([text, previous]) => document.querySelector('#log-output').textContent.split(text).length - 1 > previous",
        arg=[text, previous],
    )


def screen(page):
    return page.frame_locator("iframe").locator("canvas")


def frame(page):
    return page.locator("iframe").element_handle().content_frame()


def preview_url(url, device):
    if url.startswith("file:"):
        return f"{url}/crosspoint-{device}.html"
    return f"{url}/?device={device}"


def check_requests(requests, url, device):
    if url.startswith("file:"):
        unexpected = [request[:200] for request in requests if request != preview_url(url, device)
                      and not request.startswith(("data:", "blob:"))]
        assert not unexpected, f"Standalone preview requested another file or network resource: {unexpected}"
    else:
        assert all(request.startswith(url + "/") for request in requests), "Preview made an external request"


def check_runtime(page):
    assert page.evaluate("crossOriginIsolated === false")
    assert frame(page).evaluate("typeof SharedArrayBuffer === 'undefined'"), "Test with shared memory unavailable"
    assert not page.workers, "Single-threaded preview must not create workers"


def painted(page):
    frame(page).evaluate("new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))")


def press(page, button):
    before = screen(page).evaluate("c => c.toDataURL()")
    page.get_by_role("button", name=BUTTONS[button], exact=True).click(delay=65)
    frame(page).wait_for_function(
        "previous => document.querySelector('canvas').toDataURL() !== previous", arg=before
    )
    painted(page)


def reset_card(page):
    page.once("dialog", lambda dialog: dialog.accept())
    page.locator("#reset").click()
    page.wait_for_function("document.querySelector('#status-dot').className === 'ready'")


def upload_file(page, name, data):
    page.locator("#open-sd").click()
    page.wait_for_function("document.querySelector('#sd-message').textContent.startsWith('Ready.')")
    page.locator("#file-input").set_input_files({"name": name, "mimeType": "application/octet-stream", "buffer": data})
    page.wait_for_function("document.querySelector('#sd-message').textContent.startsWith('Ready.')")
    assert name in page.locator("#sd-list").text_content()
    page.locator("#sd-close").click()


def open_book(page):
    page.wait_for_function("document.querySelector('#status-dot').className === 'ready'")
    wait_log(page, "Entering activity: Home")
    for _ in range(3):
        press(page, "confirm")
    wait_log(page, "Rendered page in")
    painted(page)


def dark_fraction(page):
    return screen(page).evaluate("""canvas => {
      const data = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
      let dark = 0;
      for (let i = 0; i < data.length; i += 4) if (data[i] < 128) dark++;
      return dark / (data.length / 4);
    }""")


def canvas_point(page, x, y):
    # Rotation/reset animates the device width; use the settled CSS geometry.
    page.locator('#device').evaluate("el => Promise.all(el.getAnimations().map(animation => animation.finished))")
    box = screen(page).bounding_box()
    return box["x"] + box["width"] * x, box["y"] + box["height"] * y


def touch_tap(page, x, y, mouse=False):
    before = screen(page).evaluate("c => c.toDataURL()")
    point = canvas_point(page, x, y)
    if mouse:
        page.mouse.click(*point, delay=60)
    else:
        page.touchscreen.tap(*point)
    frame(page).wait_for_function(
        "previous => document.querySelector('canvas').toDataURL() !== previous", arg=before
    )
    painted(page)


def open_touch_book(page):
    page.wait_for_function("document.querySelector('#status-dot').className === 'ready'")
    touch_tap(page, 0.5, 0.43)  # Home: Browse Files
    wait_log(page, "Entering activity: FileBrowser")
    touch_tap(page, 0.5, 0.17, mouse=True)  # books directory, using a mouse
    touch_tap(page, 0.5, 0.17)  # first EPUB, using a touch contact
    wait_log(page, "Rendered page in")
    painted(page)


def swipe(page, forward, touch=False, duration=0, outside=False):
    start = canvas_point(page, 0.8 if forward else 0.2, 0.5)
    end_x = (-0.05 if forward else 1.05) if outside else (0.2 if forward else 0.8)
    end = canvas_point(page, end_x, 0.5)
    before = log_count(page, "Rendered page in")
    if touch:
        session = page.context.new_cdp_session(page)
        session.send('Input.dispatchTouchEvent', {"type": "touchStart", "touchPoints": [{"x": start[0], "y": start[1]}]})
        for step in range(1, 11):
            x = start[0] + (end[0] - start[0]) * step / 10
            session.send('Input.dispatchTouchEvent', {"type": "touchMove", "touchPoints": [{"x": x, "y": end[1]}]})
        session.send('Input.dispatchTouchEvent', {"type": "touchEnd", "touchPoints": []})
        session.detach()
    else:
        page.mouse.move(*start)
        page.mouse.down()
        for step in range(1, 11):
            page.mouse.move(start[0] + (end[0] - start[0]) * step / 10, end[1])
            if duration:
                page.wait_for_timeout(duration * 1000 / 10)
        page.mouse.up()
    wait_log(page, "Rendered page in", before)
    painted(page)


def mouse_flick(page, dx, dy=0, x=0.65, y=0.5, move=True):
    start = canvas_point(page, x, y)
    before = log_count(page, "Rendered page in")
    page.mouse.move(*start)
    if move:
        page.mouse.down()
        page.mouse.move(start[0] + dx, start[1] + dy)
        page.mouse.up()
    else:
        # A fast flick may deliver only its release position, without a move.
        session = page.context.new_cdp_session(page)
        session.send('Input.dispatchMouseEvent', {'type': 'mousePressed', 'x': start[0], 'y': start[1],
                                                 'button': 'left', 'buttons': 1, 'clickCount': 1})
        session.send('Input.dispatchMouseEvent', {'type': 'mouseReleased', 'x': start[0] + dx, 'y': start[1] + dy,
                                                 'button': 'left', 'buttons': 0, 'clickCount': 1})
        session.detach()
    wait_log(page, "Rendered page in", before)
    painted(page)
    assert log_count(page, "Rendered page in") == before + 1, "One flick must turn exactly one page"


def mouse_release_while_moving(page, forward):
    # Native Chrome can deliver a final buttons=0 move, releasing capture before
    # pointerup. Mouse.move()/up() alone do not reproduce that event ordering.
    start = canvas_point(page, 0.8 if forward else 0.2, 0.5)
    end = canvas_point(page, 0.2 if forward else 0.8, 0.5)
    before = log_count(page, "Rendered page in")
    frame(page).evaluate("""() => {
      window.releaseOrder = [];
      for (const type of ['lostpointercapture', 'pointerup']) {
        document.querySelector('canvas').addEventListener(type, e => window.releaseOrder.push([type, e.buttons]), {once:true});
      }
    }""")
    page.mouse.move(*start)
    page.mouse.down()
    page.mouse.move(end[0] + (10 if forward else -10), end[1], steps=5)
    session = page.context.new_cdp_session(page)
    session.send('Input.dispatchMouseEvent', {'type': 'mouseMoved', 'x': end[0], 'y': end[1], 'button': 'none', 'buttons': 0})
    page.mouse.up()
    session.detach()
    assert frame(page).evaluate('window.releaseOrder') == [['lostpointercapture', 0], ['pointerup', 0]]
    wait_log(page, "Rendered page in", before)
    page.wait_for_timeout(250)
    assert log_count(page, "Rendered page in") == before + 1, "Capture loss and pointerup must not duplicate the swipe"
    painted(page)


def exercise_touch(browser, url, device, output):
    context = browser.new_context(viewport={"width": 1200, "height": 1050}, locale="en-US", has_touch=True, offline=url.startswith("file:"))
    page = context.new_page()
    errors, requests = [], []
    page.on("pageerror", lambda error: errors.append(str(error)))
    page.on("request", lambda request: requests.append(request.url))
    page.goto(preview_url(url, device))
    page.wait_for_function("document.querySelector('#status-dot').className === 'ready'")
    check_runtime(page)
    assert page.locator('[data-button]:visible').all_text_contents() == [BUTTONS['previous'], BUTTONS['next'], BUTTONS['home']]

    # Canceled contacts and drags off the canvas must not select menu rows.
    for reason in ("pointercancel", "outside", "blur", "lostpointercapture"):
        page.mouse.move(*canvas_point(page, 0.5, 0.43))
        page.mouse.down()
        page.wait_for_timeout(30)
        if reason == "outside":
            box = screen(page).bounding_box()
            page.mouse.move(box['x'] - 5, box['y'] + box['height'] * 0.43)
        elif reason == "blur":
            page.locator('#copy').focus()
        elif reason == "lostpointercapture":
            screen(page).evaluate("c => c.releasePointerCapture(1)")
        else:
            screen(page).dispatch_event('pointercancel', {"pointerId": 1})
        page.mouse.up()
        page.wait_for_timeout(100)
        assert not log_count(page, "Entering activity: FileBrowser"), f"{reason} activated a menu row"

    # Start each rotation from portrait and use the real More / Orientation UI.
    for orientation in range(4):
        if orientation:
            reset_card(page)
        open_touch_book(page)
        if orientation:
            touch_tap(page, 0.5, 0.5)  # reader toolbar
            touch_tap(page, 0.8, 0.94)  # More
            touch_tap(page, 0.5, 0.85)  # Reading Orientation
            touch_tap(page, 0.5, 0.414 + orientation * 0.077)
            wanted_rotation = [90, 180, 270, 0][orientation]
            frame(page).wait_for_function(
                "rotation => document.querySelector('canvas').dataset.rotation === String(rotation)", arg=wanted_rotation
            )
            touch_tap(page, 0.5, 0.05)  # dismiss More, then toolbar, inside bezel insets
            touch_tap(page, 0.5, 0.05)
        painted(page)
        dimensions = [800, 480] if orientation % 2 else [480, 800]
        assert screen(page).evaluate("c => [c.width, c.height]") == dimensions
        assert 0.01 < dark_fraction(page) < 0.5
        initial = screen(page).evaluate("c => c.toDataURL()")
        # Firmware defaults to swipe-only page turns; preserve those settings.
        swipe(page, forward=True)
        assert screen(page).evaluate("c => c.toDataURL()") != initial
        swipe(page, forward=False, touch=True)
        assert screen(page).evaluate("c => c.toDataURL()") == initial, f"Swipe coordinates failed in orientation {orientation}"
        # Desktop drags often exceed the firmware's finger-swipe timeout or
        # finish on the bezel. Each complete drag must turn exactly one page.
        before = log_count(page, "Rendered page in")
        swipe(page, forward=True, duration=1.1)
        assert log_count(page, "Rendered page in") == before + 1
        swipe(page, forward=False, duration=0.3, outside=True)
        assert screen(page).evaluate("c => c.toDataURL()") == initial, f"Mouse drag failed in orientation {orientation}"
        # One move event, immediate release: cover brief flicks, diagonal
        # movement, and the short space between a starting point and the bezel.
        for dx, dy, x, y in ((-15, 0, 0.55, 0.5), (-25, 12, 0.75, 0.15), (-65, 0, 0.02, 0.9), (-180, 0, 0.65, 0.5)):
            mouse_flick(page, dx, dy, x, y)
            mouse_flick(page, 15, x=0.98, y=y)
            assert screen(page).evaluate("c => c.toDataURL()") == initial, f"Flick failed in orientation {orientation}"
        mouse_flick(page, -15, move=False)
        mouse_flick(page, 15, move=False)
        assert screen(page).evaluate("c => c.toDataURL()") == initial, f"Release-only flick failed in orientation {orientation}"
        mouse_release_while_moving(page, forward=True)
        mouse_release_while_moving(page, forward=False)
        assert screen(page).evaluate("c => c.toDataURL()") == initial, f"Moving release failed in orientation {orientation}"
        if orientation == 0:
            for reason in ("pointercancel", "blur", "lostpointercapture"):
                page.mouse.move(*canvas_point(page, 0.8, 0.5))
                page.mouse.down()
                page.mouse.move(*canvas_point(page, 0.2, 0.5), steps=10)
                if reason == "blur":
                    page.locator('#copy').focus()
                elif reason == "lostpointercapture":
                    screen(page).evaluate("c => c.releasePointerCapture(1)")
                else:
                    screen(page).dispatch_event('pointercancel', {"pointerId": 1})
                page.mouse.up()
                page.wait_for_timeout(100)
                assert screen(page).evaluate("c => c.toDataURL()") == initial, f"Canceled drag turned a page: {reason}"
        touch_tap(page, 0.5, 0.5)  # center tap opens the toolbar in every orientation
        touch_tap(page, 0.5, 0.05)
        assert screen(page).evaluate("c => c.toDataURL()") == initial
        page.screenshot(path=str(output / f"{device}-orientation-{orientation}.png"))

    before = log_count(page, "Entering activity: Home")
    screen(page).focus()
    page.keyboard.press('h', delay=60)
    wait_log(page, "Entering activity: Home", before)
    # Reset rotation explicitly; uploads preserve the running firmware.
    sample = ROOT / f".cache/wasm/{device}/sdcard/books/A Small Book of Pages.epub"
    reset_card(page)
    upload_file(page, "00-upload.epub", sample.read_bytes())
    open_touch_book(page)
    wait_log(page, "Loading ePub: /books/00-upload.epub")
    exercise_sd_card(page, output, device, "/books/00-upload.epub")
    # A held Home key opens the reader menu; its release must not also go Home.
    before = log_count(page, "Entering activity: Home")
    initial = screen(page).evaluate("c => c.toDataURL()")
    page.locator('[data-button="6"]').click(delay=1100)
    frame(page).wait_for_function(
        "previous => document.querySelector('canvas').toDataURL() !== previous", arg=initial
    )
    assert log_count(page, "Entering activity: Home") == before, "Held Home was treated as a tap"
    touch_tap(page, 0.5, 0.05)
    with page.expect_download() as capture:
        page.locator('#screenshot').click()
    capture.value.save_as(output / f"{device}-download.png")
    assert (output / f"{device}-download.png").read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
    exercise_restart(page, device)
    reset_card(page)
    wait_log(page, "Entering activity: Home")
    page.locator('#open-sd').click()
    page.wait_for_function("document.querySelector('#sd-list').textContent.includes('A Small Book')")
    assert '00-upload' not in page.locator('#sd-list').text_content()
    page.locator('#sd-close').click()
    deadline = time.monotonic() + 5
    while page.workers and time.monotonic() < deadline:
        page.wait_for_timeout(100)
    assert not page.workers, "Single-threaded preview must not create workers"
    assert not errors, errors
    check_requests(requests, url, device)
    (output / f"{device}-log.txt").write_text(page.locator('#log-output').text_content())
    context.close()
    print(f"PASS {device}: touch/mouse EPUB, four orientations, swipe, short/fast/slow/edge mouse drags, canceled contacts, Home tap/hold, import, screenshot, reset", flush=True)


def exercise_mouse_scheduling(browser, url):
    context = browser.new_context(viewport={"width": 1200, "height": 1050}, has_touch=True, offline=url.startswith("file:"))
    page = context.new_page()

    # Delay browser input handling; a completed swipe must not become a tap.
    instrumentation = """await createCrosspoint(module);
      window.runtimeTrace = { steps: 0, stops: 0 };
      const originalStep = module._preview_step;
      const originalStop = module._preview_stop;
      module._preview_step = () => { window.runtimeTrace.steps++; return originalStep(); };
      module._preview_stop = () => { window.runtimeTrace.stops++; originalStop(); };
      const touch = module._preview_touch;
      module._preview_touch = (phase, ...point) => {
        touch(phase, ...point);
        if (phase === 0) {
          const until = performance.now() + 8;
          while (performance.now() < until) {}
        }
      };
    """
    if url.startswith('file:'):
        # Instrument the embedded frame before its first instantiation.
        page.add_init_script(r"""Object.defineProperty(globalThis, 'crosspointBundle', {
          configurable: true,
          set(bundle) {
            if (window === top) {
              bundle.frame = bundle.frame.replace(/src="data:text\/javascript;base64,([^" ]+)"/,
                (_, data) => 'src="data:text/javascript;base64,' + btoa(atob(data).replace(
                  'await createCrosspoint(module);', INSTRUMENTATION)) + '"');
            }
            Object.defineProperty(globalThis, 'crosspointBundle', { value: bundle, configurable: true });
          }
        });""".replace('INSTRUMENTATION', json.dumps(instrumentation)))
    else:
        def pause_after_down(route):
            response = route.fetch()
            script = response.text().replace("await createCrosspoint(module);", instrumentation)
            route.fulfill(response=response, body=script)
        page.route('**/frame.js', pause_after_down)
    page.goto(preview_url(url, 'x4pro'))
    open_touch_book(page)
    initial = screen(page).evaluate('c => c.toDataURL()')
    for distance in (15, 35, 100):
        mouse_flick(page, -distance, x=0.55)
        mouse_flick(page, distance, x=0.4)
        assert screen(page).evaluate('c => c.toDataURL()') == initial
    page.evaluate("window.oldPreviewState = document.querySelector('iframe').contentWindow.runtimeTrace")
    assert page.evaluate('oldPreviewState.steps > 0')
    reset_card(page)
    wait_log(page, "Entering activity: Home")
    assert page.evaluate('oldPreviewState.stops') >= 1
    stopped_at = page.evaluate('oldPreviewState.steps')
    page.wait_for_timeout(150)
    assert page.evaluate('oldPreviewState.steps') == stopped_at, "Reset left the old scheduler running"
    check_runtime(page)
    context.close()
    print('PASS x4pro: delayed mouse input and scheduler stopped on reset', flush=True)


def exercise(browser, url, device, output):
    context = browser.new_context(viewport={"width": 1200, "height": 1050}, locale="en-US", offline=url.startswith("file:"))
    page = context.new_page()
    errors = []
    requests = []
    page.on("pageerror", lambda error: errors.append(str(error)))
    page.on("request", lambda request: requests.append(request.url))
    page.goto(preview_url(url, device))
    open_book(page)
    check_runtime(page)
    dimensions = [480, 800] if device == "x4" else [528, 792]
    assert screen(page).evaluate("c => [c.width, c.height]") == dimensions
    assert 0.01 < dark_fraction(page) < 0.5, "Book screen should contain dark text on a light page"
    page.screenshot(path=str(output / f"{device}-reader.png"))
    initial = screen(page).evaluate("c => c.toDataURL()")
    before = log_count(page, "Rendered page in")
    press(page, "next")
    wait_log(page, "Rendered page in", before)
    painted(page)
    assert screen(page).evaluate("c => c.toDataURL()") != initial, "Next page did not change the frame"
    before = log_count(page, "Rendered page in")
    # Exercise keyboard input in the embedded display as well as pointer buttons.
    screen(page).focus()
    page.keyboard.press("ArrowUp", delay=65)
    wait_log(page, "Rendered page in", before)
    painted(page)
    assert screen(page).evaluate("c => c.toDataURL()") == initial, "Previous page did not restore the first page"

    # Reader menu: orientation is the fifth row for a book without bookmarks or footnotes.
    for orientation in range(1, 5):
        press(page, "confirm")
        for _ in range(4):
            press(page, "next")
        press(page, "confirm")
        press(page, "next")
        press(page, "confirm")
        before = log_count(page, "Rendered page in")
        press(page, "back")
        wait_log(page, "Rendered page in", before)
        painted(page)
        wanted = dimensions[::-1] if orientation % 2 else dimensions
        assert screen(page).evaluate("c => [c.width, c.height]") == wanted
        page.screenshot(path=str(output / f"{device}-orientation-{orientation % 4}.png"))

    # Toggle the actual reader's Night Mode setting, then verify rendered polarity.
    press(page, "confirm")
    press(page, "next")
    press(page, "next")
    press(page, "confirm")
    before = log_count(page, "Rendered page in")
    press(page, "back")
    wait_log(page, "Rendered page in", before)
    painted(page)
    assert dark_fraction(page) > 0.5, "Night Mode did not invert the reader"

    with page.expect_download() as capture:
        page.get_by_role("button", name="Save screenshot", exact=True).click()
    download = capture.value
    download.save_as(output / f"{device}-download.png")
    assert (output / f"{device}-download.png").read_bytes().startswith(b"\x89PNG\r\n\x1a\n")

    sample = ROOT / f".cache/wasm/{device}/sdcard/books/A Small Book of Pages.epub"
    original_frame = frame(page)
    before_upload = screen(page).evaluate("c => c.toDataURL()")
    upload_file(page, "00-upload.epub", sample.read_bytes())
    assert frame(page) == original_frame, "Upload replaced the running firmware"
    assert screen(page).evaluate("c => c.toDataURL()") == before_upload, "Upload changed the reading page"
    assert dark_fraction(page) > 0.5, "Upload reset Night Mode"
    upload_file(page, "notes.txt", b"An ordinary text file on the card.")
    exercise_sd_card(page, output, device, "/books/A Small Book of Pages.epub")
    if device == "x4":
        exercise_card_lifecycle(page, url)
    exercise_restart(page, device)
    reset_card(page)
    wait_log(page, "Entering activity: Home")
    page.locator('#open-sd').click()
    page.wait_for_function("document.querySelector('#sd-list').textContent.includes('A Small Book')")
    assert '00-upload' not in page.locator('#sd-list').text_content()
    page.locator('#sd-close').click()
    assert not errors, errors
    check_requests(requests, url, device)
    deadline = time.monotonic() + 5
    while page.workers and time.monotonic() < deadline:
        page.wait_for_timeout(100)
    assert not page.workers, "Single-threaded preview must not create workers"
    (output / f"{device}-log.txt").write_text(page.locator("#log-output").text_content())
    context.close()
    print(f"PASS {device}: EPUB, page buttons/keyboard, four orientations, settings, screenshot, import, reset", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8099")
    parser.add_argument("--standalone", type=Path, help="Test HTML artifacts directly from this directory, offline")
    parser.add_argument("--browser-channel", choices=("chromium", "chrome"), default="chromium")
    parser.add_argument("--device", choices=("x4", "x3", "x4pro", "all"), default="all")
    parser.add_argument("--output", type=Path, default=ROOT / "build/preview-test")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if not args.standalone:
        deadline = time.monotonic() + 10
        while True:
            try:
                with urlopen(args.url, timeout=1) as response:
                    assert response.status == 200
                    assert not response.headers.get("Cross-Origin-Opener-Policy"), "Test with an ordinary HTTP server"
                    assert not response.headers.get("Cross-Origin-Embedder-Policy"), "Test without isolation headers"
                break
            except URLError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.1)
    with tempfile.TemporaryDirectory(prefix="crosspoint-offline-") as temporary, sync_playwright() as playwright:
        url = args.url.rstrip("/")
        if args.standalone:
            # Only the HTML files exist here: no adjacent JS, WASM, data, or server.
            folder = Path(temporary) / "Preview files 한글"
            folder.mkdir()
            for device in ("x4", "x3", "x4pro") if args.device == "all" else (args.device,):
                shutil.copyfile(args.standalone / f"crosspoint-{device}.html", folder / f"crosspoint-{device}.html")
            url = folder.as_uri()
        browser = playwright.chromium.launch(**({"channel": "chrome"} if args.browser_channel == "chrome" else {}))
        try:
            for device in ("x4", "x3", "x4pro") if args.device == "all" else (args.device,):
                test = exercise_touch if device == "x4pro" else exercise
                test(browser, url, device, args.output)
                if device == "x4pro":
                    exercise_mouse_scheduling(browser, url)
        except Exception:
            page = browser.contexts[-1].pages[0]
            page.screenshot(path=str(args.output / f"{device}-failure.png"))
            (args.output / f"{device}-failure.log").write_text(page.locator("#log-output").text_content())
            raise
        finally:
            browser.close()


if __name__ == "__main__":
    main()
