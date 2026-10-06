# Pocket Library

All of English Wikipedia, with its pictures, in your pocket and offline. Also
Wiktionary, Wikivoyage, Wikiquote, Wikisource, Wikibooks and a medical
encyclopedia with first-aid pages, all on a microSD card in an
Xteink X4 Pro e-reader. It
searches by touch and needs no network, account or phone.

It's a set of additions to [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader),
the open-source e-reader firmware, so your books keep working as before.

The short version, six steps on one page: **[noah-pi.github.io/pocket-library](https://noah-pi.github.io/pocket-library/)**.
This guide is the long version.

## What it does

- **Library** on the Home screen: a tile for each collection, your ebooks,
  and what you read recently.
- **Search everything** as you type, across every collection and your
  books. It finds well-known titles from their first letters and forgives
  typos. Search opens from the magnifier tab on the Cover Grid home, or from
  the power button if you set it to (Settings → Controls → Short Power Button
  Click → **Search**).
- **Articles** read like a book: tap a link to follow it, Back to return.
  Swipe up for the next page, down for the previous one. Tap the middle of a
  page for the contents and a toolbar: **Images**, text size, search.
- **Pictures** on request: **Images** shows an article's pictures; tap one to
  see it full screen.
- **Medical**: First Aid and a medical encyclopedia one tap from the shelf.
- It remembers your place in every article.

## What you need

| | |
|---|---|
| **Xteink X4 Pro** | The Pro only. The original X4 and the X3 use a different chip and won't start this firmware; the X4 Classic is untested. |
| **microSD card** | 256 GB for Wikipedia with pictures, or 128 GB with Wikipedia as text only. |
| **A Mac** | With a USB-C data cable (some cables only charge) and an SD card reader. Linux works too. |
| **Space for downloads** | Wikipedia with pictures alone is 127 GB. An external drive is easiest. |
| **Python 3.11 or newer** | The scripts check and tell you if it's missing. Get it from [python.org](https://www.python.org/downloads/): the macOS installer takes a few minutes. Skip Homebrew's version on older macOS, which can take hours to build. |
| **Time** | Downloading takes a few hours on a fast connection (127 GB is about 3 hours at 100 Mbps). Copying to the card takes about 3 hours, because SD cards write slowly. Both can run overnight and pick up where they stopped. |

## 1. Back up the reader and install CrossPoint

Skip this step if your reader already runs CrossPoint.

Open **Terminal** (in Applications → Utilities), plug in the reader, press its
power button to wake it, and paste these two lines:

```sh
mkdir -p ~/PocketLib && cd ~/PocketLib && curl -fsSLO https://github.com/noah-pi/pocket-library/releases/download/dev/flash.sh
bash flash.sh backup
```

This copies the reader's whole memory, including Xteink's original software,
to `~/PocketLib/backups`. It takes about 3 minutes. Keep that file: it puts
the reader back exactly as it came.

Then install CrossPoint once: open [crosspointreader.com](https://crosspointreader.com)
in **Chrome**, choose **Flash tools**, pick **Xteink X4 Pro**, and install.
The reader restarts into CrossPoint.

## 2. Install Pocket Library

With the reader plugged in and awake:

```sh
cd ~/PocketLib && curl -fsSLO https://github.com/noah-pi/pocket-library/releases/download/dev/flash.sh
bash flash.sh
```

The script sets up Espressif's flashing tool, downloads the latest build and
checks it against its published checksum. It asks before writing anything.
Your books, settings and card aren't touched.

**Success:** the reader restarts, and **Settings → About** shows a version
ending in `-pocketlib-` and a short code.

Prefer clicking? Download `pocketlib-x4pro.bin` from the
[dev release](https://github.com/noah-pi/pocket-library/releases/tag/dev) and
install it at crosspointreader.com → Flash tools → Xteink X4 Pro → **Custom .bin**.

## 3. Build the card

**Format the card.** Put it in the Mac and open **Disk Utility**. Choose
**View → Show All Devices**, select the card itself (the top line, not the
volume under it), and click **Erase**. Use name `POCKETLIB`, format **ExFAT**
and scheme **Master Boot Record**. This erases the card.

**Get the tools:**

```sh
cd ~/PocketLib && curl -fsSLO https://github.com/noah-pi/pocket-library/releases/download/dev/get-tools.sh
bash get-tools.sh
cd ~/PocketLib/cardbuilder
```

**Choose what goes on it.** Open the list with `open -e library.toml`:

- `staging` is where downloads go: `~/PocketLib/downloads` by default. To
  use an external drive, change it to something like
  `"/Volumes/MyDrive/PocketLib"`.
- For a 128 GB card, change Wikipedia's `flavour` line to `["nopic"]`.
- To leave a collection out, delete its `[[collection]]` block.

**See whether it fits** (this writes nothing). Use the Python version
`get-tools.sh` named at the end, e.g. `python3.13`:

```sh
python3.13 cardbuilder.py plan --card /Volumes/POCKETLIB
```

**Build it.** One command downloads everything, checks every file against
Kiwix's published checksum, builds the search indexes and copies it all to
the card. `caffeinate -i` keeps the Mac awake while it runs:

```sh
caffeinate -i python3.13 cardbuilder.py all --card /Volumes/POCKETLIB --verify
```

If anything stops it (Wi-Fi, a drive unplugged, the Mac asleep), run the same
command again. It skips what's already finished.

**Eject the card** in Finder before you take it out, put it in the reader, and
open **Library** from Home.

### Optional extras

- **Ebooks:** copy EPUB files onto the card. [Standard Ebooks](https://standardebooks.org)
  has beautiful free editions. They appear under **eBooks** on the shelf.
- **Pet First Aid:** a pack made from the MSD Veterinary Manual's pet-owner
  pages. It's for personal use, so make your own copy. Don't share a card
  that holds it.
  `python3.13 webpack.py pets.toml ~/PocketLib/downloads`, then run
  `cardbuilder.py all` again.

## Updating

- **Firmware:** run `bash flash.sh` again. It always fetches the latest build.
- **New Wikipedia editions** (Kiwix publishes them every few months): run
  `cardbuilder.py all --card /Volumes/POCKETLIB --prune`. `--prune` deletes
  the old editions from the card; without it they're only listed.

## Going back

- `bash flash.sh stock` installs plain CrossPoint 1.6.5.
- `bash flash.sh restore` puts back your backup from step 1, Xteink's
  original software included.

## When something goes wrong

| What you see | What to do |
|---|---|
| `no reader found` or `could not open port` | The reader is asleep or the cable only charges. Press the power button and run the command again; try another cable. |
| `This reader doesn't have CrossPoint on it yet` | Do step 1's CrossPoint install first. |
| `pip: externally-managed-environment`, `command not found: esptool` | You don't need either. `flash.sh` sets up its own copy of esptool. |
| `This needs Python 3.11 or newer` | Install Python from python.org, then use `python3.13` (or your version) in place of `python3`. |
| A download stops with timeouts | Run the same command again: downloads resume where they stopped. |
| `Device not configured` while checking a file | An external USB drive disconnected under the load. Run it again, and if it keeps happening, plug the drive straight into the Mac. |
| The copy's progress line seems to repeat itself | It's wider than the window and wraps. Widen the window; the file names are changing underneath. |
| Pictures look wrong, or anything else misbehaves | Run `bash flash.sh log`, do the thing that goes wrong on the reader, press Control-], and [open an issue](https://github.com/noah-pi/pocket-library/issues) with what it printed. |
| The reader won't start | `bash flash.sh stock`, or `bash flash.sh restore`. If even that fails, see [fix-bricked-xteink.md](../fix-bricked-xteink.md). |

## For developers

The firmware builds with PlatformIO (`pio run -e x4pro-pocketlib-release`).
The project's working rules are in [`CLAUDE.md`](../../CLAUDE.md), the plan
in [`PLAN.md`](../../PLAN.md), design decisions and measurements in
[`DECISIONS.md`](../../DECISIONS.md), and a dated log in
[`PROGRESS.md`](../../PROGRESS.md). The ZIM reader (`lib/zim/`) is written
from the [openZIM specification](https://wiki.openzim.org/wiki/ZIM_file_format)
and has its own host tests. The card builder is described in
[`tools/cardbuilder/README.md`](../../tools/cardbuilder/README.md).

## Credits and licenses

- Firmware: CrossPoint and freeink-sdk (MIT); Pocket Library's additions
  are GPL-3.0-or-later.
- Collections: [Kiwix](https://kiwix.org) packages them. Wikipedia and its
  sister projects are CC BY-SA, MedlinePlus is a US government work, and
  WikiProjectMed's encyclopedia is from [mdwiki.org](https://mdwiki.org).
- The Pet First Aid pack's pages remain the MSD Veterinary Manual's
  copyright: for your own use only.
