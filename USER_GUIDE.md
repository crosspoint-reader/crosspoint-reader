# CrossPoint User Guide

Welcome to the **CrossPoint** firmware. This guide outlines the hardware controls, navigation, and reading features of 
the device.

## 1. Hardware Overview

The device utilises the standard buttons on the Xtink X4 in the same layout:

### Button Layout
| Location        | Buttons                                    |
|-----------------|--------------------------------------------|
| **Bottom Edge** | **Back**, **Confirm**, **Left**, **Right** |
| **Right Side**  | **Power**, **Volume Up**, **Volume Down**  |

---

## 2. Power & Startup

### Power On / Off

To turn the device on or off, **press and hold the Power button for half a second**. In **Settings** you can configure
the power button to trigger on a short press instead of a long one.

### First Launch

Upon turning the device on for the first time, you will be placed on the **Home** screen.

> **Note:** On subsequent restarts, the firmware will automatically reopen the last book you were reading.

---

## 3. Screens

### 3.1 Home Screen

The Home Screen is the main entry point to the firmware. From here you can navigate to the **Book Selection** screen,
**Settings** screen, or **File Upload** screen.

### 3.2 Book Selection (Read)

The Book Selection acts as a folder and file browser.

* **Navigate List:** Use **Left** (or **Volume Up**), or **Right** (or **Volume Down**) to move the selection cursor up 
  and down through folders and books.
* **Open Selection:** Press **Confirm** to open a folder or read a selected book.

### 3.3 Reading Screen

See [4. Reading Mode](#4-reading-mode) below for more information.

### 3.4 File Upload Screen

The File Upload screen allows you to upload new e-books to the device. When you enter the screen you'll be prompted with
a WiFi selection dialog and then your X4 will start hosting a web server.

See the [webserver docs](./docs/webserver.md) for more information on how to connect to the web server and upload files.

### 3.5 Settings

The Settings screen allows you to configure the device's behavior. There are a few settings you can adjust:
- **White Sleep Screen**: Whether to use the white screen or black (inverted) default sleep screen
- **Extra Paragraph Spacing**: If enabled, vertical space will be added between paragraphs in the book, if disabled,
  paragraphs will not have vertical space between them, but will have first word indentation.
- **Short Power Button Click**: Whether to trigger the power button on a short press or a long press.

### 3.6 Sleep Screen

You can customize the sleep screen by placing custom images in specific locations on the SD card:

- **Single Image:** Place a file named `sleep.bmp` in the root directory.
- **Multiple Images:** Create a `sleep` directory in the root of the SD card and place any number of `.bmp` images inside. If images are found in this directory, they will take priority over the `sleep.png` file, and one will be randomly selected each time the device sleeps.

> [!TIP]
> For best results:
> - Use uncompressed BMP files with 24-bit color depth
> - Use a resolution of 480x800 pixels to match the device's screen resolution.

---

## 4. Reading Mode

Once you have opened a book, the button layout changes to facilitate reading.

### Page Turning
| Action            | Buttons                              |
|-------------------|--------------------------------------|
| **Previous Page** | Press **Left** _or_ **Volume Up**    |
| **Next Page**     | Press **Right** _or_ **Volume Down** |

### Chapter Navigation
* **Next Chapter:** Press and **hold** the **Right** (or **Volume Down**) button briefly, then release.
* **Previous Chapter:** Press and **hold** the **Left** (or **Volume Up**) button briefly, then release.

### System Navigation
* **Return to Home:** Press **Back** to close the book and return to the Book Selection screen.
* **Chapter Menu:** Press **Confirm** to open the Table of Contents/Chapter Selection screen.

---

## 5. Chapter Selection Screen

Accessible by pressing **Confirm** while inside a book.

1.  Use **Left** (or **Volume Up**), or **Right** (or **Volume Down**) to highlight the desired chapter.
2.  Press **Confirm** to jump to that chapter.
3.  *Alternatively, press **Back** to cancel and return to your current page.*

---

## 6. Current Limitations & Roadmap

Please note that this firmware is currently in active development. The following features are **not yet supported** but
are planned for future updates:

* **Images:** Embedded images in e-books will not render.
* **Text Formatting:** There are currently no settings to adjust font type, size, line spacing, or margins.
* **Rotation**: Different rotation options are not supported.
