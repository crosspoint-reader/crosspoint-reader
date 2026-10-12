#pragma once

#include <memory>
#include <string>

#include "WifiLinkWatch.h"
#include "network/CrossPointWebServer.h"

class GfxRenderer;
class MappedInputManager;

// The life of a web server session, shared by the activities that run one: File Transfer (on a
// network, or as a hotspot) and Calibre Wireless. They differ in their screens; how the session
// starts, serves, watches its link and ends is the same, and lives here so it stays the same.
//
//   prepareRadio()  before the radio comes up
//   start()         once the network is up: mDNS, the server, Back during an upload, the link watch
//   serve()         every loop: requests, Back and the touch Exit button
//   link()          joined networks: whether the link is up, reconnecting, or gone
//   end()           from onExit(): everything down, then the restart every network session ends in
class ServerSession {
 public:
  // The name the reader answers to on the network: http://crosspoint.local/.
  static constexpr const char* HOSTNAME = "crosspoint";

  ServerSession(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* logTag)
      : renderer(renderer), mappedInput(mappedInput), tag(logTag), linkWatch(logTag) {}
  ServerSession(const ServerSession&) = delete;
  ServerSession& operator=(const ServerSession&) = delete;

  // Unloads the SD reading font and trims what trimMemoryForNetworkSession() covers, and runs the
  // CPU at the speed the radio needs. Must run before any WiFi bring-up; idempotent, and worth
  // repeating before start() when screens drawn since have refilled the glyph cache.
  void prepareRadio();

  // False when the server could not be created or did not start. `hotspot`: the reader is the
  // network, so there is no link to watch.
  bool start(bool hotspot);

  // Serves up to `rounds` passes of requests. False when the user asked to leave: Back, also in
  // the middle of an upload, or the touch Exit button.
  bool serve(int rounds);

  // Joined networks only; see WifiLinkWatch. `ip` is updated if the link comes back on another
  // address.
  WifiLinkWatch::Link link(std::string& ip) { return linkWatch.poll(ip); }

  bool running() const { return server && server->isRunning(); }
  CrossPointWebServer* webServer() const { return server.get(); }

  // Stops the server, mDNS and the link watch. If WiFi was brought up, disconnects it and restarts
  // to Home, and does not return. Anything of the caller's own on the network (the hotspot's DNS
  // server) goes before this.
  void end();

 private:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  const char* tag;
  std::unique_ptr<CrossPointWebServer> server;
  WifiLinkWatch linkWatch;
  unsigned long lastServeEnd = 0;
  // Set by the upload cancel check when Back is seen while an HTTP upload holds handleClient().
  // A flag rather than leaving it to the Back checks in serve(): the press the check saw may sit in
  // this tick's snapshot, which the main loop's own update() overwrites before serve() looks.
  bool leaveRequested = false;
};
