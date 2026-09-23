# Transfer files over Wi-Fi

Use File Transfer to move books, fonts, screenshots, and other files between the reader and a phone, tablet, or computer. You use a web page in your browser. You do not need to install an app.

## Before you start

File Transfer is available while the reader is in **File Transfer** or **Calibre Wireless** mode. You can:

- Upload, download, rename, move, and delete SD card files.
- Create folders.
- Change many reader settings in a browser.
- Manage saved Wi-Fi networks and book catalogs.
- Upload and delete `.cpfont` font families.
- Send books from Calibre.

The file-transfer page has no password. Use it only on a private network that you trust, or in hotspot mode when you control who connects.

## Start File Transfer

1. From the Home screen, select **File Transfer**.
2. Choose a connection mode:

| Mode | Use when |
|------|----------|
| **Join Network** | The reader joins your usual Wi-Fi network. |
| **Calibre Wireless** | You want to send books from the CrossPoint Calibre plugin. |
| **Create Hotspot** | The reader creates its own Wi-Fi network. |

## Join your Wi-Fi network

1. Select **Join Network**.
2. If the reader has saved networks, it first tries the most recent one. Press **Back** to cancel. Press **Confirm** to open the network list.
3. Select a 2.4 GHz Wi-Fi network from the list.
4. Enter the password if prompted.
5. Save the password if you want the reader to reconnect next time.

After it connects, the reader shows:

- The Wi-Fi network name.
- A QR code for the web page.
- A number address, for example `http://192.168.1.102/`.
- The usual local address, `http://crosspoint.local/`.

Open either address on a phone, tablet, or computer that uses the same Wi-Fi network.

## Create a hotspot

1. Select **Create Hotspot**.
2. Connect your phone or computer to this open Wi-Fi network:

```text
CrossPoint-Reader
```

3. Open the address shown on the reader. Try `http://crosspoint.local/` first. If it does not work, use the number address, usually `http://192.168.4.1/`.

The reader shows one QR code to join the hotspot and another to open the web page.

## Send books from Calibre

Calibre Wireless starts the same file-transfer service and shows setup instructions on the reader. Use it with the CrossPoint Calibre plugin. See the [User Guide](../USER_GUIDE.md#351-send-books-from-calibre) for setup.

If you use Calibre as a book catalog, add `/opds` to its catalog address.

## Use the web page

The web page has four main sections.

### Home

The Home page shows the reader status, connection type, address, device type, and running time.

### File Manager

The File Manager page lets you:

- Browse SD card folders.
- Upload files.
- Create folders.
- Download files.
- Rename files.
- Move files into existing folders.
- Delete selected files or empty folders.

If you upload a file with the same name, it replaces the existing file. When you replace, move, rename, or delete an EPUB, CrossPoint prepares its saved book information again.

### Settings

The Settings page lets you change many reader settings. It also includes:

- Saved Wi-Fi networks
- OPDS servers

The page accepts passwords when you add or edit an entry. It does not show saved passwords.

### Fonts

The Fonts page lists installed font families and accepts `.cpfont` files. Upload one font family at a time. CrossPoint checks the family name and font file before it saves the file.

Installed fonts appear in **Settings → Reader → Font Family** after the reader updates its font list.

## Advanced connection methods

Experienced users can use `curl`, WebDAV, or WebSocket clients while File Transfer is active.

Endpoint details are documented in [webserver-endpoints.md](./webserver-endpoints.md).

## Keep your files private

- There is no password on the file-transfer page.
- Anyone on the same network can use it while it is active.
- File Transfer stops when you leave **File Transfer** or **Calibre Wireless**.
- Hotspot mode creates an open Wi-Fi network. Disconnect when you finish.

## Tips

1. If you do not have a private Wi-Fi network, use **Create Hotspot**.
2. Try `crosspoint.local` first. Keep the number address as a backup.
3. If an upload stops in **Join Network** mode, move closer to the router.
4. Upload custom fonts in the Fonts section, or copy them to `/.fonts/` or `/fonts/` on the SD card.
5. Leave File Transfer when you finish. This saves battery.

## Related Documentation

- [User Guide](../USER_GUIDE.md)
- [Webserver Endpoints](./webserver-endpoints.md)
- [SD Card Fonts](./sd-card-fonts.md)
- [Troubleshooting](./troubleshooting.md)
