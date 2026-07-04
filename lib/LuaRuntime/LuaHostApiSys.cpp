#include "LuaHostApiSys.h"

#include "LuaEngine.h"
#include "LuaHostApiContext.h"

#include <CrossPointSettings.h>
#include <HalClock.h>
#include <Logging.h>

#include "LuaAppIdle.h"
#include "network/WifiAutoConnect.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <Arduino.h>
#include <WiFi.h>
#include <string>
#include <time.h>

namespace {

LuaEngine* currentEngine() { return getActiveLuaEngine(); }

int sysLog(lua_State* L) {
  const char* message = luaL_checkstring(L, 1);
  LOG_INF("LUA", "%s", message);
  return 0;
}

int sysExit(lua_State* L) {
  if (LuaEngine* engine = currentEngine()) {
    engine->requestExit();
  }
  return luaL_error(L, "exit");
}

int sysVersion(lua_State* L) {
  lua_pushstring(L, "1.0");
  return 1;
}

int sysDevice(lua_State* L) {
  lua_pushstring(L, "X4");
  return 1;
}

int sysBattery(lua_State* L) {
  lua_pushinteger(L, -1);
  return 1;
}

int sysMillis(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(millis()));
  return 1;
}

int sysSleepMs(lua_State* L) {
  const lua_Integer ms = luaL_checkinteger(L, 1);
  if (ms <= 0) {
    return 0;
  }

  unsigned long remaining = static_cast<unsigned long>(ms);
  while (remaining > 0) {
    const unsigned long chunk = remaining > 25 ? 25 : remaining;
    delay(chunk);
    remaining -= chunk;
    crosspointLuaIdleTick();
    if (LuaEngine* engine = currentEngine()) {
      if (engine->exitRequested()) {
        return 0;
      }
    }
  }
  return 0;
}

int sysTimeSynced(lua_State* L) {
  (void)L;
  const bool synced = SETTINGS.clockHasBeenSynced != 0 && halClock.hasValidUtcTime();
  lua_pushboolean(L, synced ? 1 : 0);
  return 1;
}

int sysTime(lua_State* L) {
  uint8_t utcHour = 0;
  uint8_t utcMinute = 0;
  uint8_t utcSecond = 0;
  if (!halClock.getUtcTime(utcHour, utcMinute, utcSecond)) {
    lua_pushnil(L);
    lua_pushnil(L);
    lua_pushnil(L);
    return 3;
  }

  uint8_t offsetQ = SETTINGS.clockUtcOffsetQ;
  if (offsetQ > 104) offsetQ = 48;
  const int offsetQuarterHours = static_cast<int>(offsetQ) - 48;
  int totalSeconds = static_cast<int>(utcHour) * 3600 + static_cast<int>(utcMinute) * 60 + static_cast<int>(utcSecond) +
                     offsetQuarterHours * 15 * 60;
  const int daySeconds = 24 * 3600;
  totalSeconds = ((totalSeconds % daySeconds) + daySeconds) % daySeconds;

  lua_pushinteger(L, totalSeconds / 3600);
  lua_pushinteger(L, (totalSeconds / 60) % 60);
  lua_pushinteger(L, totalSeconds % 60);
  return 3;
}

int sysUnixTime(lua_State* L) {
  const time_t now = time(nullptr);
  if (now < 978307200) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, static_cast<lua_Integer>(now));
  return 1;
}

int sysFormatTime(lua_State* L) {
  bool use12Hour = SETTINGS.clockFormat == 1;
  if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
    use12Hour = lua_toboolean(L, 1) != 0;
  }

  char buf[12];
  if (!halClock.formatTime(buf, sizeof(buf), SETTINGS.clockUtcOffsetQ, use12Hour)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, buf);
  return 1;
}

int sysNtpServer(lua_State* L) {
  (void)L;
  lua_pushstring(L, HalClock::primaryNtpServer());
  return 1;
}

int sysWifiConnected(lua_State* L) {
  (void)L;
  lua_pushboolean(L, WiFi.status() == WL_CONNECTED ? 1 : 0);
  return 1;
}

int sysSyncClock(lua_State* L) {
  (void)L;
  const char* ntpServer = HalClock::primaryNtpServer();

  if (WiFi.status() != WL_CONNECTED) {
    std::string ssid;
    const WifiAutoConnectResult connectResult = connectLastSaved(ssid);
    if (connectResult != WifiAutoConnectResult::Connected) {
      const char* err = "no_wifi";
      switch (connectResult) {
        case WifiAutoConnectResult::NoCredentials:
          err = "no_credentials";
          break;
        case WifiAutoConnectResult::Failed:
          err = "wifi_failed";
          break;
        case WifiAutoConnectResult::Timeout:
          err = "wifi_timeout";
          break;
        default:
          break;
      }
      lua_pushboolean(L, 0);
      lua_pushstring(L, err);
      lua_pushstring(L, ntpServer);
      return 3;
    }
  }

  const bool ok = halClock.syncFromNTP();
  if (!ok || !halClock.hasValidUtcTime()) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, ok ? "no_time" : "failed");
    lua_pushstring(L, ntpServer);
    return 3;
  }

  SETTINGS.clockHasBeenSynced = 1;
  SETTINGS.saveToFile();
  lua_pushboolean(L, 1);
  lua_pushnil(L);
  lua_pushstring(L, ntpServer);
  return 3;
}

int sysFormatTimeOffset(lua_State* L) {
  const int offsetQ = static_cast<int>(luaL_checkinteger(L, 1));
  bool use12Hour = SETTINGS.clockFormat == 1;
  if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
    use12Hour = lua_toboolean(L, 2) != 0;
  }

  if (offsetQ < 0 || offsetQ > 104) {
    lua_pushnil(L);
    return 1;
  }

  char buf[12];
  if (!halClock.formatTime(buf, sizeof(buf), static_cast<uint8_t>(offsetQ), use12Hour)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, buf);
  return 1;
}

const luaL_Reg kSysFunctions[] = {
    {"log", sysLog},
    {"exit", sysExit},
    {"version", sysVersion},
    {"device", sysDevice},
    {"battery", sysBattery},
    {"millis", sysMillis},
    {"sleep_ms", sysSleepMs},
    {"time_synced", sysTimeSynced},
    {"ntp_server", sysNtpServer},
    {"wifi_connected", sysWifiConnected},
    {"sync_clock", sysSyncClock},
    {"time", sysTime},
    {"unix_time", sysUnixTime},
    {"format_time", sysFormatTime},
    {"format_time_offset", sysFormatTimeOffset},
    {nullptr, nullptr},
};

}  // namespace

void registerCpSysApi(lua_State* L, LuaEngine* engine) {
  setActiveLuaEngine(engine);
  luaL_newlib(L, kSysFunctions);
  lua_setfield(L, -2, "sys");
}
