# Web UI Refresh Port and Dark Theme Design

Date: 2026-08-18
Status: Approved for implementation

## Context

Pull request #1885 (`c105bea4`) adds a complete web UI refresh, shared CSS, a favicon, a Bun-based local development server, API-flow tests, and CI coverage. It was based on `e64155ed`, while the current upstream `develop` is `6a501bba` and contains newer firmware and web-server changes.

The refresh has not already landed in newer `develop` history: current pages still use the older inline-style layout, and current `develop` has no `app.css`, Bun dev server, or web-flow test harness. The PR is therefore still a legitimate feature port, but it must preserve post-PR changes such as upload/optimizer behavior, cache handling, device/API additions, and current CI/unit-test requirements.

The maintainer’s outstanding UI request is to make the dark background less intense than the PR’s near-black `#050505`/`#0a0a0a` palette.

## Goals

1. Port the complete PR onto current `develop`.
2. Resolve merge conflicts without dropping newer behavior.
3. Apply a softer blue-gray dark theme while leaving the light theme unchanged.
4. Retain the PR’s local development server, flow tests, asset generation, documentation, and CI integration.
5. Revalidate the changed code and repair confirmed, directly relevant review findings.

## Non-goals

- Redesigning the firmware web API or changing its endpoint contracts.
- Replacing current upload, EPUB optimizer, cache, Wi-Fi, OPDS, or font behavior with older PR behavior.
- Broad refactoring unrelated to the UI refresh or its local tooling.
- Changing the light theme or hardware behavior.

## Integration approach

Use current `origin/develop` as the target and merge PR #1885’s head. The five actual textual conflicts and their resolutions are:

- `.github/workflows/ci.yml`: retain current unit-test and required-check jobs, then add the PR’s `web-ui` check and test job.
- `.gitignore`: retain current agent/build exclusions and add the PR’s Bun/dev-server, generated screenshot, and local SD-root exclusions.
- `src/network/html/FilesPage.html`: adopt the refreshed shell and shared stylesheet while preserving current file listing, upload, optimizer, cache, multi-select, and error-flow behavior.
- `src/network/html/HomePage.html`: adopt the refreshed shell/theme controls while preserving current status data and rendering behavior.
- `src/network/html/SettingsPage.html`: adopt the refreshed shell/theme controls while preserving current settings, sequential Wi-Fi/OPDS loading, password handling, and current setting coverage.

The non-conflicting PR additions remain part of the port: `app.css`, favicon assets and route, Bun package files, `tools/web-dev-server.ts`, `tools/web-dev-flow-test.ts`, screenshots/docs, CSS/ICO asset generation, and the related contributor workflow documentation. C++ changes that merge textually will still be reviewed against current APIs and ownership/lifecycle behavior.

## Visual design

The shared stylesheet will use these dark-theme tokens:

| Token | Value |
| --- | --- |
| `--background` | `#1b1d21` |
| `--surface` | `#25282d` |
| `--surface-elevated` | `#2d3138` |
| `--border` | `#3b4048` |
| `--border-strong` | `#7d8793` |
| `--foreground` | `#f3f4f6` |
| `--muted` | `#a8adb5` |
| `--primary-foreground` | `#1b1d21` |

Other dark-theme values will be derived from these tokens where possible. Remaining hard-coded near-black control values will be removed or replaced so toggles, hover states, and buttons do not remain stark black. Destructive colors remain distinct. The modal scrim remains a dark overlay rather than becoming the page background. The light-theme token set is unchanged.

## Behavior and safety

- The refreshed shell, bottom navigation, responsive constrained desktop layout, theme persistence, forms, modals, and upload flows apply consistently across Home, Files, Settings, and Fonts.
- The local development server binds to `127.0.0.1` by default; LAN exposure requires the documented explicit host override.
- Existing firmware endpoint paths and upload protocols remain compatible.
- Dynamic file and font names must not become executable HTML or inline JavaScript. Confirmed review findings in changed paths will be fixed with minimal local changes.
- Custom confirmation overlays will retain dialog semantics, focus behavior, and Escape-key handling.
- WebSocket binary upload processing will remain ordered per client and clean up state on completion or failure.

## Verification

1. Regenerate HTML/CSS/ICO payload headers through the repository’s asset-generation path.
2. Run `git diff --check`.
3. Run `bun run web:check` and `bun run web:test` with the PR lockfile.
4. Run PlatformIO static analysis and firmware build (`pio check` and `pio run`) when the installed toolchain supports them.
5. Inspect the final diff for preserved current-develop behavior, unresolved conflict markers, accidental light-theme changes, and remaining unsafe dynamic HTML/event-handler interpolation.
6. Report any hardware-only checks that cannot be exercised in the local environment.

## Risks and mitigations

- The Files page is large and has evolved substantially since the PR base. Resolve it by behavior area, then use the local flow test to cover file CRUD, uploads, WebDAV, and conversion paths.
- CI has evolved independently. Keep the current required checks and add the web job without weakening existing gates.
- Generated firmware assets can become stale or exceed size limits. Regenerate them and run the firmware build before handoff.
- The blue-gray palette could reduce contrast if applied inconsistently. Keep foreground/border tokens explicit and inspect both themes through the local test surfaces.
