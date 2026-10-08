CrossPoint browser preview

Extract this artifact, then double-click the HTML file for your device:

  crosspoint-x4.html
  crosspoint-x3.html
  crosspoint-x4pro.html

Open it in Chrome. Each file works on its own, offline.
No server, Python installation, or other extracted files are needed.

Open Browse Files, books, A Small Book of Pages.
X4 Pro supports touch and mouse drags; X4 and X3 use the buttons below the screen.
You can also import your own EPUB. Books stay in browser memory and are cleared
by reset or reload. Screenshots and diagnostic logs can be downloaded.
Open a different HTML file to try another device.

For the hosted multi-device website, serve the complete artifact folder with
any ordinary static web server, or run this command from that folder:

  python3 -m http.server 8099 --bind 127.0.0.1

Then open http://127.0.0.1:8099. The hosted index.html requires a server;
use the crosspoint-*.html files for direct opening.

This preview tests reading and menus. It does not simulate device memory limits,
e-ink refresh behavior, network services, firmware updates, or sleep.
