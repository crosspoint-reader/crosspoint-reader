# Testing and Debugging

CrossPoint runs on real hardware, so debugging usually combines local build checks and on-device logs.

## Local checks

Make sure `clang-format` 21+ is installed and available in `PATH` before running the formatting step.
If needed, see [Getting Started](./getting-started.md).

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run
```

## Flash and monitor

Flash firmware:

```sh
pio run --target upload
```

Open serial monitor:

```sh
pio device monitor
```

Optional enhanced monitor:

```sh
python3 -m pip install pyserial colorama matplotlib
python3 scripts/debugging_monitor.py
```

## Test release workflows locally with `act`

Use [`act`](https://github.com/nektos/act) to dry-run the release workflows
after modifying them. Running locally with `act` is faster and more iterative
than pushing commits to GitHub and waiting for Actions to run. It will test the
entire workflow, including job conditions and release-notes generation, without
actually publishing releases or uploading assets.

For this repository, local `act` runs simulate different GitHub event types for
the two release workflows:

- `release.yml` is exercised with a simulated `release` event whose action is
  `published`
- `release_candidate.yml` is exercised with a simulated `push` event on
  a `release/**` branch

### What local `act` runs validate

Local runs are useful for validating:

- workflow wiring and job conditions
- PlatformIO release builds
- release-notes generation via `scripts/generate_release_notes.py`

Local `act` runs do **not** publish GitHub releases or upload release assets.
Those steps are skipped when `ACT=true`, so final release publication still
requires a real GitHub Actions run.

### Prerequisites

- `act` installed locally
- Docker available and running
  - Podman will most likely also work. Ensure the rootless user socket is
    configured and set `DOCKER_HOST` to its path (e.g.
    `export DOCKER_HOST=unix:///run/user/1000/podman/podman.sock`).
- `gh` CLI authenticated (`gh auth status`)
- event payload files in `.github/act/`

Included payload files:

- `.github/act/release-published.json` to simulate the `release.published` event
  used by `release.yml`
- `.github/act/release-candidate-push.json` to simulate the branch-push event
  used by `release_candidate.yml`

### Provide `GITHUB_TOKEN` safely

Release-notes generation expects `GITHUB_TOKEN`. Prefer exporting it from `gh`
instead of pasting a token directly into shell history:

```sh
export GITHUB_TOKEN="$(gh auth token)"
```

Then pass it to `act` by name:

```sh
act ... -s GITHUB_TOKEN
```

Unset it when finished:

```sh
unset GITHUB_TOKEN
```

### Run the stable release workflow locally

```sh
act release \
  -W .github/workflows/release.yml \
  -e .github/act/release-published.json \
  -s GITHUB_TOKEN
```

### Run the release-candidate workflow locally

```sh
act push \
  -W .github/workflows/release_candidate.yml \
  -e .github/act/release-candidate-push.json \
  -s GITHUB_TOKEN
```

### What still needs a real GitHub run

After a local `act` pass, a real GitHub Actions run is still required to verify:

- GitHub release creation
- asset upload
- workflow permissions and repository-token behavior
- the exact GitHub-hosted runner environment

## Useful bug report contents

- Firmware version and build environment
- Exact steps to reproduce
- Expected vs actual behavior
- Serial logs from boot through failure
- Whether issue reproduces after clearing `.crosspoint/` cache on SD card

## Common troubleshooting references

- [User Guide troubleshooting section](../../USER_GUIDE.md#7-troubleshooting-issues--escaping-bootloop)
- [Webserver troubleshooting](../troubleshooting.md)
