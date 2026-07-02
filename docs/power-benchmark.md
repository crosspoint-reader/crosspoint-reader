# Power Benchmark & Battery-Life Model (Xteink X3)

Purpose: a **repeatable** way to measure per-action energy on the X3 with the Nordic
Power Profiler Kit II (PPK2), and a **model** that turns those measurements into a
battery-life estimate for real usage profiles. Scoped to the **X3** (only device with
usable current instrumentation; X4 has no fuel gauge). See
[docs/file-formats.md](file-formats.md) for cache versioning that affects indexing runs.

> All current values in this doc that are **not** yet backed by a PPK2 capture are
> marked _(est.)_. They are datasheet-grade priors to be **replaced with measured
> numbers**. Do not cite an _(est.)_ value as fact.

---

## 1. Measurement conditions (fix these every run)

Battery life is a ratio of currents, so absolute rig calibration matters less than
**holding every variable constant between runs**. Lock all of the following:

| Variable | Fixed value | Why |
|---|---|---|
| Instrument | PPK2, **Source Meter mode** | supplies + measures; battery disconnected |
| Supply voltage | **3.70 V** (also sweep 3.5 / 4.0 for sensitivity) | LiPo mid-point |
| Tap point | battery terminals, upstream of charger + GPIO13 latch | latch behaves normally → real deep-sleep floor |
| Firmware build | **`slim`** or **`gh_release`** | `default` pins the clock awake (USB-CDC) and hides light-sleep |
| Serial cable | **unplugged for idle/sleep runs**; data-only (VBUS-cut) otherwise | enumerated USB keeps the C3 awake |
| Test EPUB | one fixed book, committed hash noted below | layout/indexing must match |
| Render settings | fixed font family+size, line spacing, paragraph spacing, margins | any change invalidates the section cache |
| Orientation | fixed (note which) | changes viewport → re-layout |
| Cache state | **warm** (pre-generated) unless the scenario is "cold index" | isolates the variable under test |
| Start SOC / temperature | note both; keep within a narrow band | current drifts with both |

Reference test book / settings for this benchmark: _TODO: fill in (filename, hash, font,
size, spacing, margins, orientation)._

Event segmentation: prefer a **GPIO marker → PPK2 logic pin** (high at event start, low
at end) for µs-aligned, non-perturbing boundaries. If no free test point, fall back to
**serial `millis()` timestamps** correlated to the trace (~tens-of-ms skew). Never use
serial prints as markers on idle/sleep runs.

---

## 2. Scenario suite

For each: capture **duration**, **average current**, **total charge** (integrate), and
**peak**. Energy = charge × supply voltage. Run each ≥3× and record mean/stddev.

| # | Scenario | What to trigger | Primary metric | Notes |
|---|---|---|---|---|
| S1 | **Idle, static page** | park on a page 60 s | avg mA | THE dominant term; run in `slim`, cable out |
| S2 | **Deep sleep floor** | lock, wait after latch-off | avg µA | steady-state at the sleep screen; validates tap point (should be ~µA) |
| S3 | **Page turn, fast refresh** | one turn | charge/turn, duration | most frequent active event |
| S4 | **Page turn, full refresh** | one turn forcing full refresh | charge/turn, duration | heavier panel drive |
| S5 | **E-ink refresh isolated** | refresh with no re-layout | charge, duration | panel-only floor |
| S6 | **Post-turn 160 MHz tail** | idle window right after a turn | duration @ high clock | 3 s @ ~160 MHz before downclock |
| S7 | **Chapter index (cold)** | open uncached chapter | charge, duration | CPU+SD; speed **and** power candidate |
| S8 | **Cold boot → home ready** | power on from off | charge, duration | one-time for daily reader |
| S9 | **Wake from sleep (Quick Resume)** | unlock, `sleepScreen=QUICK_RESUME` | charge, duration | boot work + half refresh |
| S10 | **Wake from sleep (sleep-screen)** | unlock, cover/moon sleep mode | charge, duration | boot + full refresh |
| S11 | **Go to sleep (Quick Resume)** | lock | charge | 48 KB framebuffer → SD |
| S12 | **Go to sleep (sleep-screen)** | lock | charge | renders cover + full refresh |
| S13 | (opt) **WiFi download / OTA** | fetch a book | charge, duration | only if in scope |

**Boot/wake sub-markers (S7–S10):** put GPIO markers at each phase — SD mount → settings/
state load → font setup → reader re-open/render → first paint — to find which phase
dominates. Wake is a **full cold boot** (`HalPowerManager.cpp:79-94`: MCU fully powered
off, no RAM/RTC retention), so this breakdown is where the responsiveness wins hide.

---

## 3. Results table (fill from PPK2)

| ID | Scenario | Duration (s) | Avg current (mA) | Charge (mC = mA·s) | Notes / build / date |
|----|----------|-------------|------------------|--------------------|----------------------|
| S1 | Idle static (slim) | | 9.68 | | slim, cable out, 2026-07-02 — awake on a reading page, NOT the sleep screen |
| S1b | Idle static + Opp 1 light-sleep | | 3.45 | | slim, cable out, 2026-07-02 — 50 ms timer-paced light sleep; ~2.8× vs S1; stable after persistent latch-hold fix (runs: 3.38/3.45/3.47) |
| S1c | Idle static + USB-poll throttle | | 2.78 | | slim, cable out, 2026-07-02 — BQ27220 USB-detect poll 50 ms → 1 s; tilt off; 3.5× vs S1 |
| S2 | Deep sleep | | 0.0128 | | 12.77 µA — idling at the sleep-screen (latch off, image is bistable); slim, cable out, 2026-07-02 |
| S3 | Turn, fast | | | 100.75 | slim, cable out, 2026-07-02 — window included the (since-removed) 160 MHz tail |
| S3b | Turn, fast + Opp 1 + tail fix | ~1.5 | | ~38 | slim, cable out, 2026-07-02 — press→sawtooth window 39.74 mC incl. idle edges; tail eliminated (render consumes the response window, light sleep engages on lock release) |
| S3c | Turn, fast + Opp 2 (BUSY-wait downclock) | ~1.5 | | 28.98 | slim, cable out, 2026-07-02 — CPU at 10 MHz during the refresh BUSY wait via EInkDisplay busy-wait hooks |
| S4 | Turn, full | | | | |
| S5 | Refresh only | | | | |
| S6 | 160 MHz tail | ~2 | 21.22 | ~42 | slim + Opp 1 build, cable out, 2026-07-02 — post-turn window before the 3 s idle threshold |
| S7 | Chapter index (cold) | | | | |
| S8 | Cold boot | | | | |
| S9 | Wake (Quick Resume) | | | | |
| S10 | Wake (sleep-screen) | | | 262.32 | slim, cable out, 2026-07-02 — window included the (since-removed) post-paint tail |
| S10b | Wake (sleep-screen) + Opp 1 + tail | | | 215 | slim, cable out, 2026-07-02 — drop vs S10 ≈ the eliminated post-paint tail |
| S11 | Sleep (Quick Resume) | | | | |
| S12 | Sleep (sleep-screen) | | | 143.81 | slim, cable out, 2026-07-02 |
| S12b | Sleep (sleep-screen) + Opp 1 + tail | | | 142.66 | slim, cable out, 2026-07-02 — unchanged, as expected (path never idles) |

**S1c floor decomposition (2026-07-02, zoomed trace):** true floor 2.00 mA; wake slices
8 mA × 4.0 ms every 50 ms (≈0.5 mA); regulator burst spikes ~15–20 mA every ~5.6 ms
(PFM light-load delivery); **SD card idle ≈ 0.4 mA** (eject A/B mid-capture). Remaining
~1 mA: MCU light sleep + flash standby (~0.2), DS3231/gauge/IMU/e-ink standby (~0.4),
regulator + charger quiescent (rest) — not firmware-addressable.

**Wake-slice decomposition (2026-07-02, duplicate-ADC probe: +0.18 mA, slice 4.0→5.6 ms):**
one button-ADC pair (Arduino `analogRead` ×2 at 10 MHz) ≈ 1.6 ms ≈ 0.18 mA of the
0.5 mA slice cost; the remaining ~2.4 ms is light-sleep entry/exit + loop overhead.
Ceiling for a raw one-shot ADC optimization: ~0.15 mA (needs an open-x4-sdk
InputManager change) — deferred in favor of Opportunity 2 (refresh busy-wait,
~10 mC/turn).

Superseded captures (default build, USB attached — power path contaminated by charger):
S1 9.79 mA · S2 22.5 µA · S3 90.63 mA·s · S10 241.80 mA·s · S12 154.51 mA·s.

**Current priors** _(est., replace with measured)_: idle static ≈ 8 mA; idle w/ light-sleep
(Opp 1 target) ≈ 0.5 mA; deep sleep ≈ 0.03 mA; fast turn ≈ 55 mA × 1.5 s; 160 MHz tail
≈ 20 mA × 3 s; wake (Quick Resume) ≈ 150–200 mA·s; wake (sleep-screen) ≈ 280 mA·s;
sleep (Quick Resume) ≈ 30 mA·s; sleep (sleep-screen) ≈ 110 mA·s.

---

## 4. Battery-life model

Symbols: `D` = dwell seconds/page, `t_r`/`I_r` = refresh time/current, `t_t`/`I_t` =
160 MHz-tail time/current, `I_idle` = static-page idle current, `I_ds` = deep-sleep
current, `E_wake`/`E_sleep` = charge per wake/sleep transition (mA·s), `C` = battery
capacity (mAh, **unknown for X3 — parameterize**).

**Per-page charge (mA·s):**

```
E_page = I_r·t_r  +  I_t·t_t  +  I_idle·(D − t_r − t_t)
```

**Continuous reader — session average current (mA):**

```
avg = E_page / D
```

**Commuter — locks every N pages:**

```
E_cycle = N·E_page + E_wake + E_sleep
avg     = E_cycle / (N·D)
```

**Daily energy (mA·h)** for `H` reading hours + `(24−H)` standby hours:

```
E_day = avg·H + I_ds·(24 − H)
```

**Battery life:** `days_per_charge = C / E_day`  •  `active_hours_per_charge = C / avg`

### Worked example (priors, C left symbolic)

Continuous reader, `D = 30 s`, priors above:

- `E_page = 55·1.5 + 20·3 + 8·(30−1.5−3) = 82.5 + 60 + 204 = 346.5 mA·s`
- `avg = 346.5 / 30 ≈ 11.6 mA` → shares: idle **59%**, tail 17%, refresh 24%.
- With Opp 1 (`I_idle` 8→0.5): `E_page ≈ 82.5 + 60 + 12.75 = 155 mA·s` → `avg ≈ 5.2 mA`
  (and lower once the tail is also slept). **≈ 2–3× active reading time.**

Commuter, `N = 5`, `D = 30 s`, Quick Resume (`E_wake≈165`, `E_sleep≈30`):

- `E_cycle = 5·346.5 + 165 + 30 = 1927.5 mA·s` over 150 s → `avg ≈ 12.9 mA`
  (≈ **+11%** vs. continuous; sleep-screen mode ≈ +22%).
- Idle is still the biggest slice even here.

### Light-sleep vs deep-sleep break-even (per lock of gap `G`)

Deep-sleeping a lock is only worth it when the standby saving beats the wake cost:

```
G* = E_wake / (I_idle_lightsleep − I_ds)
```

- **Today** (`I_idle` 8 mA): `G* ≈ 165 / (8 − 0.03) ≈ 20 s` → locks shorter than ~20 s
  _lose_ energy vs. staying awake (and resume slower).
- **After Opp 1** (`I_idle` 0.5 mA): `G* ≈ 165 / (0.5 − 0.03) ≈ 350 s ≈ 6 min` → short
  locks should stay in **light sleep** (instant resume, less energy); reserve **deep
  sleep** for long "put it away" gaps.

---

## 5. How to read the results

1. **Idle static (S1) is ~60% of a reading session** and sets the break-even above — it is
   the single most important number. Measure it first, in `slim`, cable out.
2. **Long-duration × high-current** actions (S7 chapter index, S4 full refresh, S8/S9 boot/
   wake) are **double wins**: optimizing them cuts energy _and_ improves responsiveness.
3. **Wake = cold boot**, so its cost scales with how often the user locks. Negligible for a
   once-a-day continuous reader; second-largest cost for a commuter locking every few pages.
4. Compare builds A/B on the **same rig, same conditions** — relative deltas are trustworthy
   even if absolute mA carries rig error.

Related planning notes live in the power-optimization effort (light-sleep Opportunities 1 & 2).
