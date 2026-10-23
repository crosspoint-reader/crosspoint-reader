# Plugin picker disclosure regression

The native tests use the firmware's Ubuntu fonts and FreeInkUI's wrapping and
row measurement. They check the five-event disclosure in English, Spanish and
Catalan/Valencian, bounded rows with long descriptions and wrapped titles, and
unchanged row heights for short subtitles.

```sh
cmake -S test -B build/test
cmake --build build/test --target PluginPickerLayoutTest
ctest --test-dir build/test -R PluginPickerLayout --output-on-failure
```

For a device or simulator check, copy `fixtures/device.json` to
`/plugins/event-disclosure/device.json` on the SD card, then open **Plugins**.
The entire `Receives:` sentence should be visible, including the final sleep
event. Repeat with a smaller screen or larger UI font, and move between rows
to check that selection remains visible. Remove this layout-only fixture when
finished; its event endpoints use the reserved `.invalid` domain.

`fixtures/picker.png` shows the fixture on the X4 simulator. The local UI test
build disabled the web server because its native compatibility layer does not
yet support the current firmware endpoint implementations. Plugin discovery,
event subscription labels, rendering and button navigation used the firmware
code. This screenshot is not physical e-paper validation.
