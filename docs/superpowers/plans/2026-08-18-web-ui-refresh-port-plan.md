# Web UI Refresh Port Implementation Plan

## Starting point

- Branch: `codex/web-ui-refresh`
- Target base: current `origin/develop` (`6a501bba`)
- PR head: `refs/t3/pr-1885` (`c105bea4`)
- Design: `docs/superpowers/specs/2026-08-18-web-ui-refresh-port-design.md`

## 1. Create the integration state

Merge the PR head into the current-develop branch without committing immediately:

```sh
git merge --no-commit --no-ff refs/t3/pr-1885
```

Resolve all conflict markers and preserve the current branch’s newer behavior. Keep the merge as a merge commit so the resulting history records that the stale PR was reconciled with current `develop`.

## 2. Resolve repository and CI conflicts

### `.github/workflows/ci.yml`

- Keep current build, clang-format, cppcheck, and unit-test jobs and their existing versions/steps.
- Add the PR’s Bun setup, frozen-lockfile install, web type/check job, and web flow-test job.
- Add `web-ui` to the required aggregate job without removing `unit-tests`.
- Keep current action versions and permissions unless the PR specifically requires a new entry.

### `.gitignore`

Keep both sides’ exclusions, including current agent/build state and the PR’s `node_modules`, local SD root, generated screenshots, and local tooling directories. Avoid broad patterns that hide tracked firmware assets.

### `docs/contributing/development-workflow.md`

If the current-develop wording overlaps the PR addition, preserve the current branch/repository instructions and retain the complete local web UI workflow, including host binding and environment variables.

## 3. Port the page refresh without regressing current behavior

Resolve the three HTML conflicts by behavior area rather than choosing one whole file:

- `src/network/html/HomePage.html`: retain current status fields and device/API rendering; add the PR shell, favicon, shared CSS link, nav, and persistent theme toggle.
- `src/network/html/SettingsPage.html`: retain all current setting rows, sequential Wi-Fi/OPDS loading, password-preservation behavior, and newer settings. Add the shared shell, theme persistence, refreshed controls, stepper, and custom confirmation overlay.
- `src/network/html/FilesPage.html`: retain current file listing, breadcrumb/path behavior, upload fallback and WebSocket behavior, EPUB optimizer/image-picker flow, cache-related requests, multi-select actions, logging, and error recovery. Add the PR shell, shared CSS, theme persistence, upload modal improvements, notifications, and refreshed file actions.

Use the PR’s `FontsPage.html` as the refreshed page, then verify its font-family validation and DOM-based rendering against current font API behavior.

## 4. Add and wire shared assets/tooling

- Add `src/network/html/app.css` and `favicon.ico` from the PR.
- Modify `scripts/build_html.py` to process HTML, JS, CSS, and ICO payloads with valid generated C identifiers and size constants.
- Regenerate the generated headers; do not hand-edit generated output.
- Add `/app.css` and `/favicon.ico` routes and the PR’s response helpers in `CrossPointWebServer.*`, adapting pointer types, current storage/watchdog helpers, and current endpoint behavior as needed.
- Add `package.json`, `bun.lock`, `tools/web-dev-server.ts`, and `tools/web-dev-flow-test.ts`.
- Ensure the local server continues to default to `127.0.0.1`, uses an explicit LAN opt-in, and mirrors the current API/file/WebDAV/WebSocket contracts.
- Retain the PR contributor documentation and screenshots unless current documentation structure requires a path-only adjustment.

## 5. Apply the approved palette

In `src/network/html/app.css`, update only dark-theme values to the approved blue-gray scale:

```css
--background: #1b1d21;
--surface: #25282d;
--surface-elevated: #2d3138;
--border: #3b4048;
--border-strong: #7d8793;
--foreground: #f3f4f6;
--muted: #a8adb5;
--primary-foreground: #1b1d21;
```

Replace remaining dark-only hard-coded near-black control values with tokens or matching blue-gray values. Leave the light theme and destructive palette unchanged; retain the modal scrim as an overlay.

## 6. Revalidate review findings in changed code

Inspect the PR’s CodeRabbit findings against the merged result and apply minimal fixes for confirmed issues:

- Never interpolate user-controlled filenames/paths into inline event-handler JavaScript or unescaped `innerHTML`.
- Escape conversion-log filename fragments before inserting them into HTML, or use DOM APIs where practical.
- Correct the failed-count style declaration.
- Use the cross-platform computed move target in the local server.
- Serialize per-client WebSocket binary chunk writes and clean upload state on success/error/disconnect.
- Add dialog role/ARIA/focus/Escape behavior to custom confirmation overlays.
- Normalize CSS keyword casing if the configured checker requires it.
- Pin Bun consistently with the committed lockfile/CI choice.

Do not adopt unrelated automated review suggestions that would require a broad refactor or conflict with current repository conventions.

## 7. Verify incrementally

After conflict resolution and after each major file group:

```sh
git diff --check
rg -n '<<<<<<<|=======|>>>>>>>' -- . ':!docs/contributing/assets'
```

Then run:

```sh
bun install --frozen-lockfile
bun run web:check
bun run web:test
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run
```

If a command is unavailable, record the exact limitation and run the closest repository-native check. Inspect generated header sizes and the final diff for dropped current-develop behavior, accidental light-theme changes, unsafe dynamic HTML, or stale conflict markers.

## 8. Final review and handoff

- Review the complete merge diff and `git status`.
- Run the repository’s code-review tooling if available, especially over the merged HTML/TypeScript/C++ changes.
- Summarize the resolved conflicts, palette changes, tests passed, and any hardware-only validation still needed.
- Keep the design and implementation-plan commits separate from the final integration commit(s).
