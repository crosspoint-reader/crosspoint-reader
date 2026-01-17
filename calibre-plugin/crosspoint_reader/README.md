# CrossPoint Reader Calibre Plugin

This plugin adds CrossPoint Reader as a wireless device in Calibre. It uploads
EPUB files over WebSocket to the CrossPoint web server.

Protocol:
- Connect to ws://<host>:<port>/
- Send: START:<filename>:<size>:<path>
- Wait for READY
- Send binary frames with file content
- Wait for DONE (or ERROR:<message>)

Default settings:
- Auto-discover device via UDP
- Host fallback: 192.168.4.1
- Port: 81
- Upload path: /

Install:
1. Zip the contents of `calibre-plugin/crosspoint_reader/plugin`.
2. In Calibre: Preferences > Plugins > Load plugin from file.
3. The device should appear in Calibre once it is discoverable on the network.

No configuration needed. The plugin auto-discovers the device via UDP and
falls back to 192.168.4.1:81.
