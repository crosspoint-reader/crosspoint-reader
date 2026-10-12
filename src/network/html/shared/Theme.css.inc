/* The look every page shares: colours, type, the page frame and the common controls. A page
   includes this first in its styles and adds only what is its own. Pages are served from the SD
   card, so this costs no flash.

   Colours are tokens, light and dark. The older names (--card-bg, --label-color, ...) stay as
   aliases so page-specific rules written against them keep working. */
:root {
  --font-stack: system-ui, -apple-system, "Segoe UI", Roboto, Ubuntu, Cantarell, "Noto Sans", sans-serif;
  --bg: #f3f5f4;
  --surface: #ffffff;
  --surface-2: #f6f8f7;
  --text: #28302c;
  --text-strong: #17211c;
  --text-muted: #66716b;
  --border: #e1e6e3;
  --accent: #4d7d64;
  --accent-hover: #3f6a54;
  --accent-soft: #e5efe9;
  --accent-text: #3c6d54;
  --on-accent: #ffffff;
  --danger: #b2402d;
  --danger-hover: #943423;
  --danger-soft: #fbeae6;
  --warn-soft: #fff5dc;
  --warn-border: #ecd08a;
  --warn-text: #5f4b00;
  --ok-text: #2c7a4e;
  --control-bg: #ffffff;
  --control-border: #cfd6d2;
  --switch-off: #c4ccc8;
  --focus-ring: 0 0 0 3px rgba(77, 125, 100, 0.35);
  --shadow: 0 1px 2px rgba(16, 24, 20, 0.06), 0 1px 3px rgba(16, 24, 20, 0.08);
  --shadow-raised: 0 4px 16px rgba(16, 24, 20, 0.14);
  --radius: 10px;
  --radius-sm: 6px;

  --font-color: var(--text);
  --title-color: var(--text-strong);
  --card-bg: var(--surface);
  --label-color: var(--text-muted);
  --border-color: var(--border);
  --accent-color: var(--accent);
  --accent-hover-color: var(--accent-hover);
  --toggle-bg: var(--switch-off);
  --danger-color: var(--danger);
  --notice-bg: var(--warn-soft);
  --notice-border: var(--warn-border);
  --notice-color: var(--warn-text);
  --err-bg: var(--danger-soft);
  --err-color: var(--danger);
}
/* Dark: when the system asks for it, unless the header's switch chose Light, or whenever the
   switch chose Dark (shared/ThemeInit.html.inc sets data-theme before the page is drawn). The
   two blocks hold the same values; CSS has no way to share them. */
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --bg: #171a19;
    --surface: #222725;
    --surface-2: #2a302d;
    --text: #e3e8e5;
    --text-strong: #f3f6f4;
    --text-muted: #9fa9a4;
    --border: #353c38;
    --accent: #4d7d64;
    --accent-hover: #5b8f74;
    --accent-soft: #263a30;
    --accent-text: #8fc4a7;
    --danger: #d0604c;
    --danger-hover: #e0705c;
    --danger-soft: #3a2420;
    --warn-soft: #362f17;
    --warn-border: #6b5a22;
    --warn-text: #ecd690;
    --ok-text: #8fd4aa;
    --control-bg: #1c201e;
    --control-border: #46504b;
    --switch-off: #4a534f;
    --shadow: 0 1px 2px rgba(0, 0, 0, 0.4);
    --shadow-raised: 0 4px 18px rgba(0, 0, 0, 0.55);
    color-scheme: dark;
  }
}
:root[data-theme="dark"] {
  --bg: #171a19;
  --surface: #222725;
  --surface-2: #2a302d;
  --text: #e3e8e5;
  --text-strong: #f3f6f4;
  --text-muted: #9fa9a4;
  --border: #353c38;
  --accent: #4d7d64;
  --accent-hover: #5b8f74;
  --accent-soft: #263a30;
  --accent-text: #8fc4a7;
  --danger: #d0604c;
  --danger-hover: #e0705c;
  --danger-soft: #3a2420;
  --warn-soft: #362f17;
  --warn-border: #6b5a22;
  --warn-text: #ecd690;
  --ok-text: #8fd4aa;
  --control-bg: #1c201e;
  --control-border: #46504b;
  --switch-off: #4a534f;
  --shadow: 0 1px 2px rgba(0, 0, 0, 0.4);
  --shadow-raised: 0 4px 18px rgba(0, 0, 0, 0.55);
  color-scheme: dark;
}
:root[data-theme="light"] { color-scheme: light; }

/* ---- Base ------------------------------------------------------------------------------------ */
*, *::before, *::after { box-sizing: border-box; }
html { scrollbar-gutter: stable; -webkit-text-size-adjust: 100%; }
body {
  margin: 0;
  font-family: var(--font-stack);
  font-size: 15px;
  line-height: 1.5;
  background: var(--bg);
  color: var(--text);
}
h1, h2, h3 { color: var(--text-strong); line-height: 1.25; }
h1 { font-size: 1.6rem; margin: 0 0 4px; }
h2 { font-size: 1.15rem; margin: 0 0 12px; }
h3 { font-size: 1rem; margin: 0 0 8px; }
a { color: var(--accent-text); }
:focus-visible { outline: none; box-shadow: var(--focus-ring); border-radius: var(--radius-sm); }
.hidden, [hidden] { display: none !important; }
.muted { color: var(--text-muted); }
@media (prefers-reduced-motion: reduce) {
  *, *::before, *::after { transition: none !important; animation: none !important; }
}

/* ---- Page frame: header with the nav bar, then the page ----------------------------------- */
.app-header {
  background: var(--surface);
  border-bottom: 1px solid var(--border);
  position: sticky;
  top: 0;
  z-index: 20;
}
.app-header-inner {
  max-width: 1120px;
  margin: 0 auto;
  padding: 0 16px;
  display: flex;
  align-items: center;
  gap: 16px;
  min-height: 56px;
}
.brand {
  font-weight: 700;
  font-size: 1.05rem;
  color: var(--text-strong);
  text-decoration: none;
  white-space: nowrap;
}
/* One width for the header and every page, so their left edges line up. Text-heavy content can sit
   in .page-narrow, which keeps that left edge but limits the line length. */
.theme-toggle {
  flex: none;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 36px;
  height: 36px;
  padding: 0;
  border: 1px solid transparent;
  border-radius: var(--radius-sm);
  background: transparent;
  color: var(--text-muted);
  cursor: pointer;
}
.theme-toggle:hover { background: var(--surface-2); color: var(--text); }
.theme-toggle svg { width: 18px; height: 18px; }
.page { max-width: 1120px; margin: 0 auto; padding: 24px 16px 96px; }
.page-narrow > * { max-width: 820px; }
.page-head { margin-bottom: 20px; }
.page-intro { margin: 0; color: var(--text-muted); }

/* ---- Cards and sections ------------------------------------------------------------------- */
.card {
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  box-shadow: var(--shadow);
  padding: 20px;
  margin: 0 0 16px;
}
.card > h2:first-child { margin-top: 0; }
.section-title {
  font-size: 0.78rem;
  font-weight: 600;
  letter-spacing: 0.06em;
  text-transform: uppercase;
  color: var(--text-muted);
  margin: 20px 0 4px;
}
.empty { color: var(--text-muted); text-align: center; padding: 16px 0; margin: 0; }

/* ---- Buttons ------------------------------------------------------------------------------ */
.btn {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  font: inherit;
  font-weight: 600;
  line-height: 1.2;
  padding: 9px 16px;
  border-radius: var(--radius-sm);
  border: 1px solid transparent;
  cursor: pointer;
  text-decoration: none;
  background: var(--surface-2);
  color: var(--text);
  border-color: var(--control-border);
}
.btn:hover { background: var(--border); }
.btn:disabled { opacity: 0.55; cursor: not-allowed; }
.btn-primary { background: var(--accent); color: var(--on-accent); border-color: var(--accent); }
.btn-primary:hover { background: var(--accent-hover); border-color: var(--accent-hover); }
.btn-danger { background: transparent; color: var(--danger); border-color: var(--danger); }
.btn-danger:hover { background: var(--danger); color: #fff; }
.btn-ghost { background: transparent; border-color: transparent; color: var(--accent-text); }
.btn-ghost:hover { background: var(--accent-soft); }
.btn-small { padding: 6px 12px; font-size: 0.875rem; }

/* ---- Form controls ------------------------------------------------------------------------ */
input[type="text"], input[type="password"], input[type="number"], input[type="search"], input[type="url"],
select, textarea {
  font: inherit;
  color: var(--text);
  background: var(--control-bg);
  border: 1px solid var(--control-border);
  border-radius: var(--radius-sm);
  padding: 8px 10px;
  min-height: 38px;
  max-width: 100%;
}
input:focus-visible, select:focus-visible, textarea:focus-visible { border-color: var(--accent); }
select { padding-right: 28px; }

/* The browser's file picker button, drawn like .btn. */
input[type="file"] { font: inherit; color: var(--text-muted); max-width: 100%; }
input[type="file"]::file-selector-button {
  font: inherit;
  font-weight: 600;
  margin-right: 12px;
  padding: 8px 14px;
  border-radius: var(--radius-sm);
  border: 1px solid var(--control-border);
  background: var(--surface-2);
  color: var(--text);
  cursor: pointer;
}
input[type="file"]::file-selector-button:hover { background: var(--border); }

/* An on/off switch: <label class="switch"><input type="checkbox" role="switch"><span></span></label> */
.switch { position: relative; display: inline-block; width: 46px; height: 26px; flex: none; }
.switch input { position: absolute; opacity: 0; width: 100%; height: 100%; margin: 0; cursor: pointer; z-index: 1; }
.switch span {
  position: absolute;
  inset: 0;
  background: var(--switch-off);
  border-radius: 13px;
  transition: background 0.15s;
}
.switch span::before {
  content: "";
  position: absolute;
  width: 20px;
  height: 20px;
  left: 3px;
  top: 3px;
  background: #fff;
  border-radius: 50%;
  box-shadow: 0 1px 2px rgba(0, 0, 0, 0.25);
  transition: transform 0.15s;
}
.switch input:checked + span { background: var(--accent); }
.switch input:checked + span::before { transform: translateX(20px); }
.switch input:focus-visible + span { box-shadow: var(--focus-ring); }

/* A short choice as buttons: <div class="segmented" role="radiogroup"><label><input type="radio"><span>..</span></label>...</div> */
.segmented {
  display: inline-flex;
  flex-wrap: wrap;
  border: 1px solid var(--control-border);
  border-radius: var(--radius-sm);
  overflow: hidden;
  background: var(--control-bg);
}
.segmented label { position: relative; }
.segmented input { position: absolute; opacity: 0; inset: 0; margin: 0; cursor: pointer; }
.segmented span {
  display: block;
  padding: 7px 12px;
  font-size: 0.9rem;
  white-space: nowrap;
  border-left: 1px solid var(--control-border);
}
.segmented label:first-child span { border-left: 0; }
.segmented input:checked + span { background: var(--accent); color: var(--on-accent); }
.segmented input:focus-visible + span { box-shadow: inset var(--focus-ring); }

/* ---- Messages ----------------------------------------------------------------------------- */
.notice {
  border: 1px solid var(--warn-border);
  background: var(--warn-soft);
  color: var(--warn-text);
  border-radius: var(--radius-sm);
  padding: 10px 14px;
}
.badge {
  display: inline-block;
  font-size: 0.75rem;
  font-weight: 600;
  padding: 2px 8px;
  border-radius: 999px;
  background: var(--accent-soft);
  color: var(--accent-text);
  vertical-align: middle;
}
.badge-muted { background: var(--surface-2); color: var(--text-muted); border: 1px solid var(--border); font-weight: 500; }
.badge-warn { background: var(--warn-soft); color: var(--warn-text); }
.badge-error { background: var(--danger-soft); color: var(--danger); }
.card > .notice { margin: 0 0 16px; }

/* A label and its value on one line, as System Info, Diagnostics and Reading Stats list them. */
.info-row {
  display: flex;
  justify-content: space-between;
  align-items: baseline;
  gap: 12px;
  padding: 9px 0;
}
.info-row + .info-row { border-top: 1px solid var(--border); }
.info-row .label { color: var(--text-muted); flex-shrink: 0; max-width: 50%; }
.info-row .value { color: var(--text-strong); font-weight: 500; text-align: right; overflow-wrap: anywhere;
  font-variant-numeric: tabular-nums; }

/* Short-lived feedback at the bottom of the window; role="status" so screen readers hear it. */
.toast {
  position: fixed;
  left: 50%;
  bottom: 24px;
  transform: translateX(-50%) translateY(20px);
  opacity: 0;
  pointer-events: none;
  background: var(--text-strong);
  color: var(--bg);
  padding: 10px 18px;
  border-radius: var(--radius-sm);
  box-shadow: var(--shadow-raised);
  transition: opacity 0.2s, transform 0.2s;
  z-index: 50;
  max-width: calc(100% - 32px);
}
.toast.show { opacity: 1; transform: translateX(-50%) translateY(0); }
.toast.error { background: var(--danger); color: #fff; }

.loader {
  width: 36px;
  height: 36px;
  border: 4px solid var(--border);
  border-top-color: var(--accent);
  border-radius: 50%;
  margin: 32px auto;
  animation: spin 0.9s linear infinite;
}
@keyframes spin { to { transform: rotate(360deg); } }
