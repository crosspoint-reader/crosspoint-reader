#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "String.h"

enum HTTPMethod { HTTP_ANY, HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE };

class WebServer {
 public:
  using THandlerFunction = std::function<void()>;

  struct Response {
    int statusCode = -1;
    String contentType;
    String body;
  };

  void on(const char* uri, HTTPMethod method, THandlerFunction handler) {
    routes_.push_back(Route{String(uri), method, std::move(handler)});
  }

  void on(const String& uri, HTTPMethod method, THandlerFunction handler) {
    routes_.push_back(Route{uri, method, std::move(handler)});
  }

  void setRequest(HTTPMethod method, const String& uri) {
    requestMethod_ = method;
    requestUri_ = uri;
    requestArgs_.clear();
    response_ = Response{};
  }

  void setArg(const String& name, const String& value) { requestArgs_[name.toStdString()] = value; }

  void setBody(const String& body) {
    if (body.isEmpty()) {
      requestArgs_.erase("plain");
      return;
    }
    requestArgs_["plain"] = body;
  }

  bool hasArg(const String& name) const { return requestArgs_.count(name.toStdString()) > 0; }

  String arg(const String& name) const {
    const auto it = requestArgs_.find(name.toStdString());
    return it == requestArgs_.end() ? String() : it->second;
  }

  bool dispatch() {
    response_ = Response{};
    for (const auto& route : routes_) {
      if (route.method == requestMethod_ && route.uri == requestUri_) {
        route.handler();
        return true;
      }
    }

    send(404, "text/plain", "Not found");
    return false;
  }

  bool hasRoute(const String& uri, HTTPMethod method) const {
    for (const auto& route : routes_) {
      if (route.method == method && route.uri == uri) {
        return true;
      }
    }
    return false;
  }

  void send(const int statusCode, const char* contentType, const String& body) {
    response_.statusCode = statusCode;
    response_.contentType = String(contentType);
    response_.body = body;
  }

  void send(const int statusCode, const String& contentType, const String& body) {
    send(statusCode, contentType.c_str(), body);
  }

  const Response& response() const { return response_; }

 private:
  struct Route {
    String uri;
    HTTPMethod method;
    THandlerFunction handler;
  };

  HTTPMethod requestMethod_ = HTTP_ANY;
  String requestUri_;
  std::map<std::string, String> requestArgs_;
  std::vector<Route> routes_;
  Response response_;
};
