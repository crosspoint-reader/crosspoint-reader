# Home menu

Open Settings → Display → Home menu to choose which actions appear on Home.
Clear a checkbox to hide that action.
Settings stays checked and visible.
These choices affect Home only, and other routes through Settings remain available.
Continue Reading and book covers keep their existing behavior.

One selection applies to all themes.
The selected count includes Settings, and each layout determines its capacity.
When Home is full, hide an action before selecting another one.
When a smaller layout needs space, it hides the lowest actions first and keeps Settings.
A popup names the actions that it hides.
Switching back to a larger layout does not restore those actions.

Unavailable actions do not appear in the editor.
Plugins keeps its existing priority over OPDS in their shared Home slot.
Hiding Plugins does not reveal OPDS while the plugin system remains enabled.
If configuration changes make an action available again, Home selects it again and applies the capacity limit.
New actions in future firmware default to hidden unless that update explicitly changes the default.

## Device verification

Back up `/.crosspoint/settings.json` before testing.
Host checks do not establish display, input, or heap behavior on a device.
Use an X3/X4 for the ESP32-C3 baseline and a supported PSRAM board for Cover Grid.

1. Open the editor, hide File Transfer, and return Home. Make sure that File Transfer disappears and Settings remains visible.
2. Restart the device. Make sure that the saved selection remains unchanged.
3. Hide every editable action. Make sure that Settings and the existing book controls remain accessible.
4. Switch among Classic, Lyra, Lyra Extended, RoundedRaff, and Cover Grid where supported. Make sure that selected actions fit above the footer or inside the icon strip.
5. Fill a larger layout, then choose a smaller layout. Make sure that one popup names all automatically hidden actions.
6. Return to a larger layout. Make sure that automatically hidden actions remain unchecked.
7. Fill Home and try to check another action. Make sure that the editor refuses and displays “Hide another item first.”
8. Hide Plugins, disable the plugin system, then enable it before leaving Settings. Make sure that Plugins returns, subject to capacity.
9. With Plugins disabled, hide OPDS, remove its last server, and add a server. Make sure that OPDS returns, subject to capacity.
10. Repeat availability changes through the web interface. Make sure that the Home selection follows the same rules.
11. Use touch, remapped buttons, held navigation, Back, and Home gestures. Make sure that labels, icons, focus, and opened destinations match.
12. Open a book in portrait, inverted portrait, landscape clockwise, and landscape counterclockwise. Return Home and make sure that its normal portrait layout works.
13. Repeat editor and overflow-popup visits. Measure internal free heap and its largest block separately from PSRAM, and make sure that repeated visits do not leak memory.
14. Use the largest UI scale and another language. Make sure that the editor and combined popup show their full messages.

The ESP32-C3 device target is more than 50 KB of free heap.
Missing Settings, overlapping rows, wrong destinations, repeated notifications, and declining heap are failure signs.
