CrossPoint browser preview

Download the HTML for your device from the PR comment, then double-click it:

  crosspoint-x4.html
  crosspoint-x3.html
  crosspoint-x4pro.html

Open it in Chrome. Each file works on its own, offline.
No server, Python installation, or other extracted files are needed.

Open Browse Files, books, A Small Book of Pages.
X4 Pro supports touch and mouse drags; X4 and X3 use the buttons below the screen.
Choose Browse SD card to upload files or folders, browse directories, download,
rename, or delete files. Files are copied immediately without restarting.
Reopen Browse Files in the reader to refresh its list.

To add fonts, upload a family folder containing .cpfont files into /fonts.
Choose Restart firmware, then select the family in the reader's font settings.
Restart firmware preserves the card and saved settings. Clear SD card restores
the sample card. Reloading the HTML also discards files and settings.
Files stay in this tab's memory; uploads are limited to 64 MiB of card contents.

Screenshots and diagnostic logs can be downloaded.
Open a different HTML file to try another device.

For the hosted multi-device website, extract the complete static-site ZIP and
serve its folder with any ordinary static web server, or run:

  python3 -m http.server 8099 --bind 127.0.0.1

Then open http://127.0.0.1:8099. The hosted index.html requires a server;
use the crosspoint-*.html files for direct opening.

This preview tests reading and menus. It does not simulate device memory limits,
e-ink refresh behavior, network services, firmware updates, or sleep.
