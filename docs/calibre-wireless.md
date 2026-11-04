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
5. Update the value for "Host" to match the IP for your device.
6. Leave the other settings as they are.
7. [optional] Modify the "Upload path" to point to a subfolder other than the root "/" folder. Enter this as a path relative to the root folder. Example: `/mybooks`
8. Restart Calibre.

<img width="420" height="385" alt="CrossPoint plugin configuration in Calibre" src="https://github.com/user-attachments/assets/01fc7e33-a9a7-48ba-9e26-2e68d1f9daec" />

## Uploading Books

To upload a book using the CrossPoint plugin in Calibre:

1. On the device: File Transfer -> Calibre Wireless, then join a network.
2. Select one or more books.
3. Right-click on that selection.
4. Select "Send to Device" > "Send to main memory"

The CrossPoint plugin will connect to your device, create a folder for the book's author in the root folder (or the folder you configured for the plugin), then copy the book into that folder.

<img width="783" height="310" alt="Send selected books to the device in Calibre" src="https://github.com/user-attachments/assets/741b0909-2e1d-4f16-8af0-2c43fbda5ce6" />

## Removing a Book

Books cannot be removed from your device through Calibre. Use the web interface instead.
