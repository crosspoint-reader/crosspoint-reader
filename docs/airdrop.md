## AirDrop on X4 Pro and X4C (experimental)

Build with `pio run -e x4pro` or `pio run -e x4c`. Their release and release-candidate
profiles also enable `CROSSPOINT_AIRDROP` and the vendored receiver. Other boards
exclude the receiver because they have not been physically tested.
Use the image for your exact board; X4 Pro and X4C images are not interchangeable.

Choose **File Transfer → AirDrop**. Select **CrossPoint X4 Pro** or **CrossPoint X4C**
from the iPhone AirDrop share sheet, then accept the request on the reader.
On X4C, use the navigation buttons to select Accept or Decline, then Confirm.
Back declines a pending request; Back while waiting stops the receiver.
The confirmation shows the first filename and, for a batch, the total file count.
Unicode filename metadata is decoded; visible glyphs depend on the UI font.
Very long names are shortened in the popup.
Reading the sender metadata can delay the popup. Requests with invalid metadata,
more than 64 files, or over 512 KiB of metadata are rejected.
Decline rejects it. No Mac relay or Wi-Fi access point is used.

The receive window lasts ten minutes; an accepted transfer may finish afterward.
Wi-Fi and Bluetooth cannot be used simultaneously with this receiver. The control
center is unavailable while receiving; stop AirDrop before changing its controls.
Files are saved under `/AirDrop`; **Show received files** opens that folder.
Existing files are preserved by placing duplicates in a numbered subfolder.

The limit is 30 MiB (31,457,280 bytes) for the decoded archive, including all
files and archive metadata. Leave a little room below this for an individual EPUB.
If advertised sizes already exceed the limit, the reader rejects the request
before asking for acceptance. If sizes are missing or inaccurate, the archive
header and running decoded-byte checks enforce the same limit during receipt.
Oversized transfers show a cancellation popup; OK returns to waiting for another
transfer. Completed earlier files in a batch may remain saved.
The receiver decodes DvZip records into a fixed 256 KiB PSRAM buffer and writes
files incrementally through the main task. The compressed residue is capped at
512 KiB. It does not allocate a 30 MiB RAM buffer. This stream path accepts the
DvZip format observed from iPhone; gzip and bare-cpio bodies are rejected.
Long filenames up to 255 bytes are supported, subject to the SD filesystem.
Success is sent only after archive validation and completed file writes. A later
failure can leave earlier complete files saved. An unfinished file is removed on
failure or Stop; a power loss may leave a hidden `.incoming-N.part` file.
A callback taking longer than ten seconds reports failure even if the card later
finishes the write. Large books may take many minutes.

This is an independent, experimental AirDrop implementation. The earlier buffered
build received an iPhone EPUB and its saved archive passed a readback CRC check.
The 30 MiB streaming build still needs a real large-file transfer check.
The author confirmed receiving and opening an iPhone-sent EPUB on X4C.
Repeated start/stop heap recovery, near-limit transfers, remapped buttons, and
all four display orientations still require hardware validation. Its SDK profile
provides 8 MiB PSRAM, but available internal heap during radio/TLS startup must
still be measured on an X4C. The receiver retains exclusive radio ownership until
Stop or the ten-minute receive window ends.

## Ownership and memory

The activity owns the receiver session and open SD file. The TLS worker sends
bounded chunks through a single-slot mailbox; the main task handles storage and
consent. Stop waits for worker ownership to end before releasing radio and buffers.
The control center is blocked during the session so it cannot suspend polling or
reconfigure the radio underneath a transfer.

Streaming uses 256 KiB of PSRAM for decoded records and at most 512 KiB for
compressed residue. Growth can temporarily retain the old residue allocation,
so decode/residue allocations can peak at 1,088 KiB. Request metadata is bounded
to 512 KiB and freed before the acceptance dialog and upload. These figures do
not include TLS, radio queues, task stacks, fonts or the display. There is no
30 MiB allocation. Internal heap and largest free blocks must be measured
separately; PSRAM availability does not guarantee TLS startup will succeed.

## Verification

Run `python3 test/airdrop/run.py` for the sanitizer-backed host tests. The same
suite is registered with CTest. Build both board targets and their release
profiles before publishing firmware.

On each device, check discovery, Accept, Decline, Back/Stop, received-file opening,
duplicate filenames, missing/full SD card, and an advertised oversized request.
Check a near-limit real transfer and repeat start/stop while measuring internal
heap, its largest free block, PSRAM and task stack watermarks. Test popup input
in all four orientations and with remapped buttons. A compile or parser test
does not establish radio interoperability or panel behaviour.
