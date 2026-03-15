#pragma once

#include <memory>
#include <string>

class CrossPointWebServer;

class RemoteKeyboardNetworkSession {
 public:
  struct State {
    bool ready = false;
    bool apMode = false;
    bool usingExistingServer = false;
    std::string ssid;
    std::string ip;
    std::string url;
  };

  ~RemoteKeyboardNetworkSession();

  bool begin();
  void loop();
  void end();

  const State& snapshot() const { return state; }
  bool ownsServer() const { return ownedServer != nullptr; }

 private:
  bool startServerOnCurrentConnection();
  bool startAccessPointAndServer();

  std::unique_ptr<CrossPointWebServer> ownedServer;
  State state;
  bool startedAp = false;
};
