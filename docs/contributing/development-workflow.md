# Development Workflow

This page defines the expected local workflow before opening a pull request.

## 1) Fork and create a focused branch

- Fork the repository to your own GitHub account
- Clone your fork locally and add the upstream repository if needed
- Enable repo hooks once per clone: `git config core.hooksPath .githooks && chmod +x .githooks/pre-commit`

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

### Web UI development

The web UI can be developed locally without a device. Install Bun, then run:

```sh
bun install
bun run web:dev
```

The dev server serves the same HTML/CSS files used by the ESP32 web server and simulates the API, WebDAV, and WebSocket upload paths against a local SD-card directory at `.crosspoint-web-dev-sd`.
It binds to `127.0.0.1` by default because the simulator exposes file upload, WebDAV write/delete, and settings routes without authentication.

Useful commands:

```sh
bun run web:check
bun run web:test
```

`web:check` typechecks the local dev tooling. `web:test` starts an isolated simulator and verifies page loading, file operations, settings, Wi-Fi, OPDS, WebDAV, and WebSocket upload flows.

To test the UI against a real reader while still using local HTML/CSS with live reload:

```sh
CROSSPOINT_DEVICE=http://<device-ip> bun run web:dev
```

Use `WEB_PORT=<port>` if the default port is already in use.
Use `WEB_HOST=0.0.0.0` only when you intentionally need LAN access to the simulator.
When `CROSSPOINT_DEVICE` is set, the dev server proxies API and WebSocket traffic to that reader while continuing to serve local HTML/CSS, so treat the server as a local development tool rather than a public service.

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
