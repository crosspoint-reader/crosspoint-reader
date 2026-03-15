# Development Workflow

This page defines the expected local workflow before opening a pull request.

## 1) Fork and create a focused branch

- Fork the repository to your own GitHub account
- Clone your fork locally and add the upstream repository if needed

- Branch from `master`
- Keep each PR focused on one fix or feature area

## 2) Implement with scope in mind

- Confirm your idea is in project scope: [SCOPE.md](../../SCOPE.md)
- Prefer incremental changes over broad refactors

## 3) Run local checks

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run
```

CI enforces formatting, static analysis, and build checks.
Use clang-format 21+ locally to match CI.
If `clang-format` is missing or too old locally, see [Getting Started](./getting-started.md).

### Flashing from WSL to an Xteink X4

If you build inside WSL and want to flash an Xteink X4 over USB-C, the device must be attached into your WSL distro before PlatformIO can see it.

1. Install `usbipd-win` on Windows.
2. Plug in the X4 and list USB devices from Windows PowerShell:

```powershell
usbipd list
```

3. Find the X4 serial/JTAG device, then attach it to WSL:

```powershell
usbipd attach --wsl --busid <BUSID> --auto-attach
```

If you need to target a specific distro:

```powershell
usbipd attach --wsl --distribution Ubuntu --busid <BUSID> --auto-attach
```

4. In WSL, confirm PlatformIO can see the serial port:

```sh
pio device list
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

For the X4 this is commonly exposed as `/dev/ttyACM0`.

5. If upload fails with `Permission denied`, add your user to the `dialout` group and restart WSL:

```sh
sudo usermod -aG dialout $USER
```

Then from Windows PowerShell:

```powershell
wsl --shutdown
```

Open WSL again and verify your groups:

```sh
groups
```

As a temporary test-only workaround, you can also grant access to the current device node:

```sh
sudo chmod 666 /dev/ttyACM0
```

6. Upload the firmware:

```sh
pio run -e default -t upload --upload-port /dev/ttyACM0
```

If the ESP32-C3 does not enter the bootloader automatically, hold `BOOT`, tap `RESET`, and retry the upload command.

## 4) Open the PR

- Use a semantic title (example: `fix: avoid crash when opening malformed epub`)
- Fill out `.github/PULL_REQUEST_TEMPLATE.md`
- Describe the problem, approach, and any tradeoffs
- Include reproduction and verification steps for bug fixes

## 5) Review etiquette

- Be explicit and concise in responses
- Keep discussions technical and respectful
- Assume good intent and focus on code-level feedback

For community expectations, see [GOVERNANCE.md](../../GOVERNANCE.md).
