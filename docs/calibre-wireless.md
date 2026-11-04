# Calibre Wireless Transfers

[Back to the User Guide](../USER_GUIDE.md#transfer-and-sync)

CrossPoint supports sending books from Calibre using the CrossPoint Reader device plugin.

## Installing the Plugin in Calibre

If you don't already have the plugin installed:

1. Head to https://github.com/crosspoint-reader/calibre-plugins/releases to download the latest version of the crosspoint_reader plugin.
2. Download the zip file.
3. Open Calibre → Preferences → Plugins → Load plugin from file → Select the zip file.
4. Restart Calibre.

## Configuring the CrossPoint Plugin in Calibre

1. In Calibre select Preferences.
2. In the Preferences dialog select Plugins.
3. In Plugins search for "crosspoint".
4. Click on "Customize plugin".
5. If broadcast discovery fails, set "Host" to the IP shown in File Transfer → Calibre Wireless after the reader joins a network. This adds a UDP probe target. The reader must still reply to UDP discovery. Host and Port do not enable a direct connection without that reply. See [Uploading Books](#uploading-books) for the connection step.
6. Leave the other settings as they are.
7. [optional] Modify the "Upload path" to point to a subfolder other than the root "/" folder. Enter this as a path relative to the root folder. Example: `/mybooks`
8. Restart Calibre.

<img width="420" height="385" alt="CrossPoint plugin configuration in Calibre" src="https://github.com/user-attachments/assets/01fc7e33-a9a7-48ba-9e26-2e68d1f9daec" />

## Uploading Books

To upload a book using the CrossPoint plugin in Calibre:

1. On the device: File Transfer -> Calibre Wireless, then join a network.
2. Connect your computer to the same network as the device.
3. In Calibre, select one or more books.
4. Right-click on that selection.
5. Select "Send to Device" > "Send to main memory"

Network client isolation can prevent the computer from reaching the reader even on the same network. If this happens, use a network that allows devices to communicate.

Upload path sets the base folder. When Send to root is off, the plugin uses its Upload template if one is configured, otherwise Calibre's send-to-device template. The template chooses folders and filenames within the base folder.

Send to root skips template formatting, including template-derived folders. It keeps the configured Upload path: with `/mybooks` as the base folder, books go directly into `/mybooks`, not the filesystem root.

<img width="783" height="310" alt="Send selected books to the device in Calibre" src="https://github.com/user-attachments/assets/741b0909-2e1d-4f16-8af0-2c43fbda5ce6" />

## Removing a Book

The current plugin supports deleting books from the device through Calibre. Browse Files and the web interface also offer deletion.

See the [plugin source](https://github.com/crosspoint-reader/calibre-plugins/blob/master/crosspoint_reader/driver.py) and [plugin configuration](https://github.com/crosspoint-reader/calibre-plugins/blob/master/crosspoint_reader/config.py) for the current behavior.
