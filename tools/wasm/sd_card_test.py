"""Exercise the live SD card manager through its UI and bounded message protocol."""
from pathlib import Path
import tempfile


def wait_ready(page):
    page.wait_for_function("document.querySelector('#sd-message').textContent.startsWith('Ready.')")


def select(page, path):
    page.locator(f'#sd-list tr[data-path="{path}"] .file-name').click()


def answer(page, button, value=None):
    page.once('dialog', lambda dialog: dialog.accept(value) if value is not None else dialog.accept())
    page.locator(button).click()


def rpc(page, operation, path, **kwargs):
    return page.evaluate("""payload => new Promise(resolve => {
      const frame = document.querySelector('iframe').contentWindow;
      const id = `test-${crypto.randomUUID()}`;
      const receive = event => {
        if (event.source !== frame || event.data.type !== 'sd-response' || event.data.id !== id) return;
        window.removeEventListener('message', receive);
        if (event.data.result?.bytes) event.data.result.bytes = Array.from(new Uint8Array(event.data.result.bytes));
        resolve(event.data);
      };
      window.addEventListener('message', receive);
      if (payload.operation === 'write') payload.bytes = new Uint8Array(payload.size || 0).buffer;
      frame.postMessage({ type: 'sd-request', id, ...payload }, '*');
    })""", {'operation': operation, 'path': path, **kwargs})


def exercise_sd_card(page, output, device, active_book):
    original_frame = page.locator('iframe').element_handle().content_frame()
    original_screen = page.frame_locator('iframe').locator('canvas').evaluate('c => c.toDataURL()')
    page.locator('#open-sd').click()
    wait_ready(page)
    # The open book cannot be overwritten, renamed, or removed, even via its parent.
    select(page, active_book)
    answer(page, '#sd-delete')
    page.wait_for_function("document.querySelector('#sd-message').textContent.startsWith('Close this book')")
    for operation in ('write', 'delete', 'rename'):
        result = rpc(page, operation, active_book, destination='/books/renamed.epub', replace=True)
        assert result['error']['code'] == 'activeBook', result
    assert rpc(page, 'delete', '/books')['error']['code'] == 'activeBook'
    for path in ('/../outside', '/books/../../outside', '/books//bad', '/books/./bad', '/books/bad\\name'):
        assert rpc(page, 'write', path)['error']['code'] == 'invalidPath', path
    assert rpc(page, 'write', '/.crosspoint/settings.json')['error']['code'] == 'protectedFile'
    assert rpc(page, 'delete', '/')['error']['code'] == 'protectedFile'
    assert rpc(page, 'restore', '/')['error']['code'] == 'invalidOperation'
    assert rpc(page, 'write', '/books/too-big.bin', size=64 * 1024 * 1024 + 1)['error']['code'] == 'storageFull'

    answer(page, '#sd-new-folder', 'QA files')
    wait_ready(page)
    page.get_by_role('button', name='Open: QA files', exact=True).click()
    wait_ready(page)
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp) / '테스트 folder'
        (folder / 'nested').mkdir(parents=True)
        (folder / 'nested/data.bin').write_bytes(b'\x00\xff\x01browser SD\x00')
        (folder / '.hidden.txt').write_text('hidden file')
        (folder / 'empty.txt').touch()
        page.locator('#folder-input').set_input_files(str(folder))
        wait_ready(page)
        page.get_by_role('button', name='Open: 테스트 folder', exact=True).click()
        wait_ready(page)
        assert '.hidden.txt' not in page.locator('#sd-list').text_content()
        page.locator('#sd-hidden').check()
        assert '.hidden.txt' in page.locator('#sd-list').text_content()
        assert '0 B' in page.locator('#sd-list').text_content()
        page.get_by_role('button', name='Open: nested', exact=True).click()
        wait_ready(page)
        select(page, '/books/QA files/테스트 folder/nested/data.bin')
        with page.expect_download() as capture:
            page.locator('#sd-download').click()
        capture.value.save_as(output / f'{device}-sd-download.bin')
        assert (output / f'{device}-sd-download.bin').read_bytes() == (folder / 'nested/data.bin').read_bytes()
        wait_ready(page)
        # Downloading must not detach the filesystem's byte buffer.
        with page.expect_download() as capture:
            page.locator('#sd-download').click()
        capture.value.save_as(output / f'{device}-sd-download-again.bin')
        assert (output / f'{device}-sd-download-again.bin').read_bytes() == (folder / 'nested/data.bin').read_bytes()
        wait_ready(page)
        answer(page, '#sd-rename', 'renamed.bin')
        wait_ready(page)
        select(page, '/books/QA files/테스트 folder/nested/renamed.bin')
        answer(page, '#sd-delete')
        wait_ready(page)
        assert page.locator('#sd-empty').is_visible()
        page.locator('#sd-path').get_by_role('button', name='테스트 folder', exact=True).click()
        wait_ready(page)
        page.once('dialog', lambda dialog: dialog.accept())
        page.locator('#file-input').set_input_files({'name': 'empty.txt', 'mimeType': 'text/plain', 'buffer': b'replaced'})
        wait_ready(page)
        assert '8 B' in page.locator('#sd-list').text_content()
        page.screenshot(path=str(output / f'{device}-sd-manager.png'))
        page.set_viewport_size({'width': 390, 'height': 844})
        page.wait_for_timeout(200)
        page.screenshot(path=str(output / f'{device}-sd-mobile.png'))
        assert page.locator('#sd-dialog').evaluate('d => d.scrollWidth <= d.clientWidth')
        page.set_viewport_size({'width': 1200, 'height': 1050})
        page.locator('#sd-path').get_by_role('button', name='books', exact=True).click()
        wait_ready(page)
        select(page, '/books/QA files')
        answer(page, '#sd-delete')
        wait_ready(page)
        assert 'QA files' not in page.locator('#sd-list').text_content()
    page.locator('#sd-close').click()
    assert page.locator('iframe').element_handle().content_frame() == original_frame
    assert page.frame_locator('iframe').locator('canvas').evaluate('c => c.toDataURL()') == original_screen
    print(f'PASS {device}: live SD browsing, folder upload, binary download, overwrite, rename, recursive delete, hidden files, active-book protection, path/size limits, responsive layout', flush=True)


def exercise_card_lifecycle(page, url):
    # Close the reader, replace its source, and verify its real cache is removed.
    before = rpc(page, 'list', '/.crosspoint')['result']['entries']
    caches = [entry['path'] for entry in before if '/epub_' in entry['path']]
    assert caches, 'Opening the sample should create a book cache'
    page.locator('[data-button="0"]').click(delay=1200)
    page.wait_for_function("document.querySelector('#log-output').textContent.includes('Exiting activity: EpubReader')")
    sample = Path('.cache/wasm/x4/sdcard/books/A Small Book of Pages.epub').read_bytes()
    page.locator('#open-sd').click()
    wait_ready(page)
    page.once('dialog', lambda dialog: dialog.accept())
    page.locator('#file-input').set_input_files({'name': 'A Small Book of Pages.epub', 'mimeType': 'application/epub+zip', 'buffer': sample})
    wait_ready(page)
    after = rpc(page, 'list', '/.crosspoint')['result']['entries']
    assert not any(entry['path'] in caches for entry in after), 'Replacement kept stale book caches'

    # Drop a directory whose API returns several batches, including an empty folder.
    page.evaluate("""() => {
      const file = name => ({ name, isFile: true, file: resolve => resolve(new File(['dropped'], name)) });
      const folder = (name, batches) => ({ name, isFile: false, createReader: () => ({
        readEntries: resolve => resolve(batches.shift() || []),
      }) });
      const entry = folder('Dropped', [[file('one.txt')], [folder('Empty', []), file('two.txt')], []]);
      const event = new Event('drop', { bubbles: true, cancelable: true });
      Object.defineProperty(event, 'dataTransfer', { value: { items: [{ webkitGetAsEntry: () => entry }], files: [] } });
      document.querySelector('#sd-drop-zone').dispatchEvent(event);
    }""")
    wait_ready(page)
    result = rpc(page, 'list', '/books/Dropped')['result']['entries']
    assert {entry['path'].split('/')[-1] for entry in result} == {'one.txt', 'two.txt', 'Empty'}
    page.locator('#sd-close').click()
    if not url.startswith('file:'):
        old = page.locator('iframe').element_handle().content_frame()
        page.locator('#device-select').select_option('x3')
        page.wait_for_function("document.querySelector('#device-name').textContent === 'X3' && document.querySelector('#status-dot').className === 'ready'")
        assert page.locator('iframe').element_handle().content_frame() != old
        assert len(rpc(page, 'list', '/books/Dropped')['result']['entries']) == 3
        page.locator('#device-select').select_option('x4')
        page.wait_for_function("document.querySelector('#device-name').textContent === 'X4' && document.querySelector('#status-dot').className === 'ready'")
        assert len(rpc(page, 'list', '/books/Dropped')['result']['entries']) == 3

    # Clearing the card while a host file is being read must cancel that upload.
    page.locator('#open-sd').click()
    wait_ready(page)
    page.evaluate("""() => {
      const original = File.prototype.arrayBuffer;
      File.prototype.arrayBuffer = function() {
        File.prototype.arrayBuffer = original;
        return new Promise(resolve => { window.finishOldUpload = () => original.call(this).then(resolve); });
      };
    }""")
    page.locator('#file-input').set_input_files({'name': 'stale.txt', 'mimeType': 'text/plain', 'buffer': b'must not reappear'})
    page.wait_for_function('typeof finishOldUpload === "function"')
    page.locator('#sd-close').click()
    answer(page, '#reset')
    page.wait_for_function("document.querySelector('#status-dot').className === 'ready'")
    page.evaluate('finishOldUpload()')
    page.wait_for_timeout(100)
    files = rpc(page, 'list', '/books')['result']['entries']
    assert [entry['path'] for entry in files] == ['/books/A Small Book of Pages.epub']
    print('PASS x4: closed-book cache invalidation, directory drop batches, interrupted-upload cancellation' + (', card preserved across device changes' if not url.startswith('file:') else ''), flush=True)


def exercise_restart(page, device):
    assert 'error' not in rpc(page, 'write', '/books/restart-check.bin', size=31)
    settings = rpc(page, 'download', '/.crosspoint/settings.json')['result']['bytes']
    old = page.locator('iframe').element_handle()
    if device == 'x3':
        page.locator('#open-sd').click()
        wait_ready(page)
        page.locator('#sd-restart').click()
    else:
        page.locator('#restart').click()
    page.wait_for_function('old => !old.isConnected', arg=old)
    page.wait_for_function("document.querySelector('#status-dot').className === 'ready'")
    assert rpc(page, 'download', '/books/restart-check.bin')['result']['bytes'] == [0] * 31
    assert rpc(page, 'download', '/.crosspoint/settings.json')['result']['bytes'] == settings
    assert not page.locator('#sd-dialog').is_visible()
    print(f'PASS {device}: explicit firmware restart preserves files and saved settings', flush=True)
