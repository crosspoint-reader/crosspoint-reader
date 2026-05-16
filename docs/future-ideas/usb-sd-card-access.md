# USB SD Card Access

## Idea

Expose the Xteink SD card to a host computer over USB so developers and users can inspect files, pull crash reports,
and drop books or package archives without removing the card or starting the Wi-Fi file server.

## Why It Matters

The firmware already writes panic details to `/crash_report.txt` on the SD card after a crash. Today that file is easy
to mention in the UI but not always easy to retrieve: the device does not currently mount the SD card as a USB volume on
macOS, and serial access only shows live logs.

USB SD access would also improve common workflows:

- copy EPUB files directly from a computer
- retrieve `crash_report.txt` after a panic
- inspect `/.marginalia/packages`, `/.marginalia/inbox`, and `/.marginalia/package-state`
- side-load `.mpkg.zip` archives without relying on Wi-Fi

## Possible Shapes

### USB MSC Mode

Expose the SD card as a USB Mass Storage Class device. This is the most familiar user experience because the card would
appear as a normal removable drive.

Main concern: the firmware and host computer must not write the same filesystem at the same time. The firmware would
need a dedicated mode that unmounts or locks internal SD access while USB MSC owns the card.

### Read-Only Diagnostic Export

Expose a smaller USB endpoint or serial command that can read selected files, starting with `/crash_report.txt`.

This is less convenient for general file transfer but safer because the firmware can keep filesystem ownership and only
stream known files out.

### Improve Wi-Fi File Manager First

Keep USB simple and add explicit crash-report download plus package/book shortcuts to the existing web server.

This avoids USB filesystem ownership problems, but still depends on Wi-Fi setup and does not help when networking is
unavailable.

## Initial Recommendation

Investigate USB MSC support, but implement a read-only crash-report export first if MSC requires risky filesystem
handoff work. Crash retrieval is the urgent debugging gap, while full drag-and-drop SD access needs careful power,
locking, and corruption handling.

## Open Questions

- Can the ESP32-C3 USB stack expose MSC and serial/JTAG in the way this board is wired?
- Can the SD card be safely detached from firmware services while USB owns it?
- What should the device UI show while the SD card is mounted on the host?
- Should USB file access be read-only by default?
- How do we recover if the host unplugs during a write?

## Deferred Because

The current priority is stabilizing the package/theme path and shipping the Hub flow. USB SD access needs hardware,
filesystem, and UX investigation before implementation.
