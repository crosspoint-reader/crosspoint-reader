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
The author confirmed receiving and opening an iPhone-sent EPUB on X4C.
An optimized wolfSSL build received a 549,500-byte EPUB on X4 Pro, with the
sender reporting completion. A later X4C log reached Saved after receiving
3,887,895 compressed bytes, with at least 5,505,024 file bytes written before
the final archive entry. That transfer took several minutes. These results
predate the ACK-queue optimization below; its throughput and transfer integrity
still require device testing. Near-limit 30 MiB transfers remain unverified.
Repeated start/stop heap recovery, near-limit transfers, remapped buttons, and
all four display orientations still require hardware validation. Its SDK profile
provides 8 MiB PSRAM, but available internal heap during radio/TLS startup must
still be measured on an X4C. The receiver retains exclusive radio ownership until AirDrop exits. The ten-minute
automatic exit applies only while waiting for a sender.

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

### Transfer troubleshooting

After accepting, the reader shows that it is waiting for the sender, then
received/saved KiB counts. Counts refresh at most once every ten seconds to limit
e-ink work. Received bytes describe the compressed transport; saved bytes describe
the current extracted file, so these numbers need not match.

Failed acceptance responses and an upload that never starts are reported as
errors. The reader no longer silently returns to the availability screen after
45 seconds. Approval expires after 90 seconds and is consumed only by the
approved peer.

Each AirDrop session replaces `/AirDrop/last-transfer.log`. Copy it before opening
AirDrop again if a transfer fails. The log is optional, capped at 32 KiB, and
written by the main task at ten-second intervals or state changes. It records
connection counts, received bytes, storage write latency, radio synchronization,
queue occupancy and errors; it does not capture file contents or peer addresses.
A log-write failure disables diagnostics without rejecting a transfer.

Diagnostic state numbers are Waiting=0, Asking=1, Receiving=2, Saved=3,
TooLarge=4 and Error=5. `rx` is compressed body bytes, `wire` includes TLS traffic,
`sdms` is the longest file-write call, and `lock`, `mask`, `drop`, `q`, and `flight`
describe the current radio window, RX drops, TX queue and in-flight frames.

The X4C large-file stall still requires device reproduction. Passing the native
5.4 MB TLS checksum test does not establish radio reliability or SD-card timing.

The ten-minute automatic exit applies only while waiting for a sender. Receiving,
completed, failed, and oversized results remain on screen until dismissed. If
opening or writing the diagnostic log fails, the receiving/error popup says so;
the transfer can still continue. A missing log alone does not identify the failure.

## ACK queue experiment

Queued plain TCP acknowledgements and their link retries are skipped when a
successfully queued newer acknowledgement for the same endpoints, ports and
send sequence covers more received bytes. Equal acknowledgements are retained
for loss recovery. Data, control flags, zero windows, SACK and unknown TCP/IPv6
options retain their original handling. One fixed ACK record is shared under
the existing transmit-state lock; no receive buffers or task ownership change.

The diagnostic log adds `ackskip` (obsolete ACK attempts avoided) and `txdrop`
(new outgoing packets refused by a full queue). These are session counters.
The supplied X4C log measured about 6 KiB/s in single-window intervals and
21 KiB/s with three windows. It did not capture packet types, so queue congestion
alone does not prove obsolete ACKs caused the slowdown. Host policy tests are
not a device throughput measurement. The 5,760-byte TCP receive window and
peer-controlled availability windows remain unchanged; 20-second completion is
not established. Compare the same iPhone/file/location before and after, retaining
the SD log and checking that reception completes and file size/hash agree.
