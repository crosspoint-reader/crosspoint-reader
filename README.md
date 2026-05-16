# shortbread

![shortbread on desk](./docs/images/desk.jpg)

Custom firmware for the **Xteink X4** e-paper device. A lean, focused build — e-reader first, with games and small utilities on the side.

## Why this exists

A couple of Xteink firmware forks caught my eye — each had something the others didn't. [biscuit](https://github.com/yattsu/biscuit) had the cleanest UI and the games. [CrossInk](https://github.com/uxjulia/CrossInk) had the best reading experience. I wanted both in one place, stripped to just the parts I actually use. So I built shortbread.

Want a feature added, changed, or removed? Open an issue or ping me on GitHub.

## Hardware

| Spec | Value |
|------|-------|
| SoC | ESP32-C3 (RISC-V, 160MHz) |
| RAM | 380KB SRAM |
| Flash | 16MB |
| Display | 4.26" 800×480 e-ink, 1-bit mono |
| Input | 7 buttons |
| WiFi | 2.4GHz 802.11 b/g/n |
| BLE | 5.0 |
| Storage | MicroSD (FAT32) |
| Port | USB-C |

## What's in it

Five tiles on the home screen:

| Tile | Contents |
|------|----------|
| **Network** | WiFi connect, BLE scanner |
| **Tools** | File browser, calculator, clock, unit converter, password manager, WiFi scanner |
| **Games** | Casino, Minesweeper, Sudoku, Chess, Snake, Tetris, Maze, and more |
| **Reader** | EPUB 2/3, KOReader sync, Calibre wireless transfer, recent books with cover art |
| **Settings** | Display, reader fonts, controls, WiFi file transfer, device info |

## Installing

No releases yet — manual install only.

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- Python 3.8+
- USB-C data cable

### Flash

```bash
git clone --recursive https://github.com/deepakvettickal/shortbread
cd shortbread
pio run --target upload
```

### Build only

```bash
pio run -j 16
```

## SD card

Data is stored under `/shortbread/` on the MicroSD card (FAT32).

## Credits

Built on top of:

- **[crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader)** — core EPUB engine, rendering, KOReader sync, Calibre transfer. The foundation everything runs on.
- **[biscuit](https://github.com/yattsu/biscuit)** — UI design, games, and overall architecture this fork is based on. Sorry [@yattsu](https://github.com/yattsu) — I borrowed heavily from your work and even stole the naming idea (biscuit → shortbread). Your build is what made this possible.
- **[CrossInk](https://github.com/uxjulia/CrossInk)** — reader settings menu and fonts (ChareInk, Lexend Deca).
