-- World Clock: local time plus UTC, EST, CST, and NPT.

local APP_NAME = "World Clock"
local APP_VERSION = "1.1.9"

-- Preset offsets (biased quarter-hours): UTC+0, UTC-5, UTC-6, NPT +5:45
local HOME_PRESETS = {48, 28, 24, 71}
local HOME_PRESET_LABELS = {"UTC+0", "UTC-5", "UTC-6", "UTC+5:45"}

local function get_home_offset_q()
  local saved = cp.settings.get("home_offset_q")
  if saved then
    return saved
  end
  local global = cp.settings.global_get("clock_utc_offset_q")
  if global and global ~= 48 then
    return global
  end
  -- X4 has no device UI for clock UTC offset (defaults to UTC+0); use US Central
  -- daylight (UTC-5) until the user picks a home zone with Left on the Local row.
  return 28
end

local function save_home_offset_q(offset_q)
  cp.settings.set("home_offset_q", offset_q)
end

local function cycle_home_offset(delta)
  local current = get_home_offset_q()
  local idx = 1
  for i, q in ipairs(HOME_PRESETS) do
    if q == current then
      idx = i
      break
    end
  end
  idx = ((idx - 1 + delta) % #HOME_PRESETS) + 1
  save_home_offset_q(HOME_PRESETS[idx])
end

local function home_preset_label(offset_q)
  for i, q in ipairs(HOME_PRESETS) do
    if q == offset_q then
      return HOME_PRESET_LABELS[i]
    end
  end
  return "UTC?"
end

local zones = {
  { label = "Local", offset_q = nil },
  { label = "UTC", offset_q = 48 },
  { label = "EST", offset_q = 28 },
  { label = "CST", offset_q = 24 },
  { label = "NPT", offset_q = 71 },
}

local selected = 1
local use_12h = false
local sync_status = nil -- nil | "connecting" | "syncing" | err codes from cp.sys.sync_clock | "no_api"
local last_ntp = "pool.ntp.org"

local function ntp_server()
  if type(cp.sys.ntp_server) == "function" then
    return cp.sys.ntp_server()
  end
  return "pool.ntp.org"
end

local function wifi_status_line()
  if type(cp.sys.wifi_connected) ~= "function" then
    return "Wi-Fi: flash new firmware"
  end
  if cp.sys.wifi_connected() then
    return "Wi-Fi: connected"
  end
  return "Wi-Fi: not connected"
end

local function poll_exit()
  local btn = cp.input.poll()
  if btn == "back" then
    cp.sys.exit()
  end
  return btn
end

-- Wait up to max_ms for a button; returns "none" on timeout (for clock refresh).
local function wait_button(max_ms)
  local elapsed = 0
  while elapsed < max_ms do
    local btn = cp.input.poll()
    if btn == "back" then
      cp.sys.exit()
    end
    if btn ~= "none" then
      return btn
    end
    cp.sys.sleep_ms(50)
    elapsed = elapsed + 50
  end
  return "none"
end

local function zone_time(zone)
  if zone.offset_q == nil then
    return cp.sys.format_time_offset(get_home_offset_q(), use_12h)
  end
  return cp.sys.format_time_offset(zone.offset_q, use_12h)
end

local function draw_header()
  cp.display.center(20, APP_NAME)
  cp.display.center(38, "v" .. APP_VERSION)
end

local function draw_not_synced()
  cp.display.center(64, "Clock not synced.")
  cp.display.center(86, wifi_status_line())
  cp.display.center(104, "NTP: " .. last_ntp)

  if sync_status == "no_credentials" then
    cp.display.center(126, "Save Wi-Fi in Settings > System first")
  elseif sync_status == "wifi_failed" then
    cp.display.center(126, "Wi-Fi connection failed")
  elseif sync_status == "wifi_timeout" then
    cp.display.center(126, "Wi-Fi connection timed out")
  elseif sync_status == "no_wifi" then
    cp.display.center(126, "Flash firmware with Wi-Fi auto-connect")
  elseif sync_status == "no_api" then
    cp.display.center(126, "Needs cp.sys.sync_clock")
  elseif sync_status == "no_time" then
    cp.display.center(126, "Time still invalid after sync")
  elseif sync_status == "failed" then
    cp.display.center(126, "NTP request failed")
  elseif sync_status == nil and cp.sys.wifi_connected and cp.sys.wifi_connected() then
    cp.display.center(126, "Press Confirm to retry sync")
  end

  local retry = sync_status == "failed" or sync_status == "wifi_failed" or sync_status == "wifi_timeout" or sync_status == "no_time"
  cp.display.center(260, retry and "Confirm = retry" or "Confirm = sync now")
  cp.display.center(278, "Back = exit")
end

local function draw()
  cp.display.clear()
  draw_header()

  if sync_status == "connecting" or sync_status == "syncing" then
    cp.display.center(98, "Connecting Wi-Fi & syncing...")
    cp.display.center(120, "NTP: " .. last_ntp)
    cp.display.center(278, "Back = exit")
    cp.display.refresh()
    return
  end

  if not cp.sys.time_synced() then
    draw_not_synced()
    cp.display.refresh()
    return
  end

  sync_status = nil

  local y = 64
  for i, zone in ipairs(zones) do
    local prefix = (i == selected) and "> " or "  "
    local time_str = zone_time(zone) or "--:--"
    local label = zone.label
    if zone.offset_q == nil then
      label = "Local (" .. home_preset_label(get_home_offset_q()) .. ")"
    end
    local line = prefix .. label
    while #line < 12 do
      line = line .. " "
    end
    cp.display.center(y, line .. time_str)
    y = y + 22
  end
  cp.display.center(y + 8, selected == 1 and "Left = home zone" or "Left = sync")
  cp.display.center(y + 26, "Up/Down/Right = zone")
  cp.display.center(y + 44, "Confirm = 12h/24h")
  cp.display.center(278, "Back = exit")
  cp.display.refresh()
end

local function sync_clock()
  if type(cp.sys.sync_clock) ~= "function" then
    sync_status = "no_api"
    return false
  end
  last_ntp = ntp_server()
  sync_status = "connecting"
  draw()
  local ok, err, ntp = cp.sys.sync_clock()
  if ntp and ntp ~= "" then
    last_ntp = ntp
  end
  if ok then
    sync_status = nil
    return true
  end
  sync_status = err or "failed"
  return false
end

local function handle_button(btn)
  if btn == "up" and cp.sys.time_synced() then
    selected = ((selected - 2) % #zones) + 1
  elseif btn == "down" and cp.sys.time_synced() then
    selected = (selected % #zones) + 1
  elseif btn == "right" and cp.sys.time_synced() then
    selected = (selected % #zones) + 1
  elseif btn == "left" and cp.sys.time_synced() then
    if selected == 1 then
      cycle_home_offset(1)
    else
      sync_clock()
    end
  elseif btn == "confirm" then
    if cp.sys.time_synced() then
      use_12h = not use_12h
    else
      sync_clock()
    end
  end
end

if cp.settings.global_get("clock_format") == 1 then
  use_12h = true
end

last_ntp = ntp_server()

if not cp.sys.time_synced() then
  sync_clock()
end

-- Draw immediately so AppRunner "Loading..." is replaced; poll after each paint.
while true do
  draw()
  local btn = wait_button(30000)
  if btn ~= "none" then
    handle_button(btn)
  end
end
