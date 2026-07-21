#pragma once

#include <cstring>
#include <map>
#include <string>

#include "esp_err.h"
#include "SimHttpFetch.h"

enum http_event { HTTP_EVENT_ON_DATA, HTTP_EVENT_ON_HEADER };

enum esp_http_client_method_t {
  HTTP_METHOD_GET,
  HTTP_METHOD_POST,
  HTTP_METHOD_PUT,
};

enum esp_http_client_auth_type_t {
  HTTP_AUTH_TYPE_NONE = 0,
  HTTP_AUTH_TYPE_BASIC,
  HTTP_AUTH_TYPE_DIGEST,
};

struct SimEspHttpClient;
typedef SimEspHttpClient *esp_http_client_handle_t;

struct esp_http_client_event_t {
  http_event event_id;
  esp_http_client_handle_t client;
  void *data;
  int data_len;
  void *user_data;
  const char *header_key;
  const char *header_value;
};

typedef esp_err_t (*http_event_handler_cb)(esp_http_client_event_t *evt);

struct esp_http_client_config_t {
  const char *url = nullptr;
  http_event_handler_cb event_handler = nullptr;
  int timeout_ms = 0;
  int buffer_size = 0;
  int buffer_size_tx = 0;
  void *user_data = nullptr;
  esp_http_client_method_t method = HTTP_METHOD_GET;
  bool skip_cert_common_name_check = false;
  esp_err_t (*crt_bundle_attach)(void *conf) = nullptr;
  const char *cert_pem = nullptr;
  int cert_len = 0;
  bool keep_alive_enable = false;
  const char *username = nullptr;
  const char *password = nullptr;
  esp_http_client_auth_type_t auth_type = HTTP_AUTH_TYPE_NONE;
};

struct SimEspHttpClient {
  esp_http_client_config_t config{};
  std::map<std::string, std::string> headers;
  std::string postField;
  int statusCode = 0;
  int contentLength = -1;
  std::string responseBody;
  size_t bodyOffset = 0;
  bool opened = false;
};

namespace sim_http_client_detail {
inline const char *methodName(esp_http_client_method_t method) {
  switch (method) {
  case HTTP_METHOD_POST:
    return "POST";
  case HTTP_METHOD_PUT:
    return "PUT";
  case HTTP_METHOD_GET:
  default:
    return "GET";
  }
}
} // namespace sim_http_client_detail

inline esp_err_t esp_http_client_set_header(esp_http_client_handle_t handle,
                                            const char *name,
                                            const char *value) {
  if (!handle || !name)
    return ESP_FAIL;
  handle->headers[name] = value ? value : "";
  return ESP_OK;
}

inline esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t handle,
                                                const char *data, int len) {
  if (!handle)
    return ESP_FAIL;
  handle->postField.assign(data ? data : "", len > 0 ? static_cast<size_t>(len) : 0);
  return ESP_OK;
}

inline bool esp_http_client_is_chunked_response(esp_http_client_handle_t) {
  return false;
}

inline int esp_http_client_get_content_length(esp_http_client_handle_t handle) {
  return handle ? handle->contentLength : -1;
}

inline esp_err_t esp_http_client_get_chunk_length(esp_http_client_handle_t,
                                                  int *len) {
  if (len)
    *len = 0;
  return ESP_OK;
}

inline int esp_http_client_get_status_code(esp_http_client_handle_t handle) {
  return handle ? handle->statusCode : 0;
}

inline esp_http_client_handle_t
esp_http_client_init(const esp_http_client_config_t *config) {
  if (!config || !config->url)
    return nullptr;
  auto *handle = new SimEspHttpClient();
  handle->config = *config;
  return handle;
}

inline esp_err_t esp_http_client_perform(esp_http_client_handle_t handle) {
  if (!handle || !handle->config.url)
    return ESP_FAIL;

  using namespace sim_http_client_detail;

  const char *method = methodName(handle->config.method);
  sim_http_fetch::Response response;
  if (!sim_http_fetch::fetch(handle->config.url, method, handle->headers, "",
                             handle->postField.empty() ? nullptr : handle->postField.c_str(), response))
    return ESP_FAIL;

  handle->statusCode = response.statusCode;
  handle->contentLength = static_cast<int>(response.body.size());

  if (handle->config.event_handler && !response.body.empty()) {
    esp_http_client_event_t event{};
    event.event_id = HTTP_EVENT_ON_DATA;
    event.client = handle;
    event.data = const_cast<char *>(response.body.data());
    event.data_len = static_cast<int>(response.body.size());
    event.user_data = handle->config.user_data;
    handle->config.event_handler(&event);
  }

  return ESP_OK;
}

inline esp_err_t esp_http_client_open(esp_http_client_handle_t handle,
                                      int /*write_len*/) {
  if (!handle || !handle->config.url)
    return ESP_FAIL;
  using namespace sim_http_client_detail;
  sim_http_fetch::Response response;
  std::string basicAuth;
  if (handle->config.username && handle->config.auth_type != HTTP_AUTH_TYPE_NONE) {
    basicAuth = handle->config.username;
    basicAuth += ':';
    if (handle->config.password)
      basicAuth += handle->config.password;
  }
  if (!sim_http_fetch::fetch(handle->config.url, methodName(handle->config.method),
                             handle->headers, basicAuth,
                             handle->postField.empty() ? nullptr : handle->postField.c_str(),
                             response))
    return ESP_FAIL;
  handle->statusCode = response.statusCode;
  handle->responseBody = std::move(response.body);
  handle->contentLength = static_cast<int>(handle->responseBody.size());
  handle->bodyOffset = 0;
  handle->opened = true;
  return ESP_OK;
}

inline int64_t esp_http_client_fetch_headers(esp_http_client_handle_t handle) {
  if (!handle || !handle->opened)
    return -1;
  return static_cast<int64_t>(handle->responseBody.size());
}

inline esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t /*handle*/) {
  // curl -L already follows redirects; no-op in simulator
  return ESP_OK;
}

inline esp_err_t esp_http_client_close(esp_http_client_handle_t handle) {
  if (!handle)
    return ESP_FAIL;
  handle->responseBody.clear();
  handle->bodyOffset = 0;
  handle->opened = false;
  return ESP_OK;
}

inline int esp_http_client_read(esp_http_client_handle_t handle, char *buf,
                                int len) {
  if (!handle || !handle->opened || !buf || len <= 0)
    return -1;
  size_t remaining = handle->responseBody.size() - handle->bodyOffset;
  size_t toRead = std::min(static_cast<size_t>(len), remaining);
  if (toRead == 0)
    return 0;
  std::memcpy(buf, handle->responseBody.data() + handle->bodyOffset, toRead);
  handle->bodyOffset += toRead;
  return static_cast<int>(toRead);
}

inline bool
esp_http_client_is_complete_data_received(esp_http_client_handle_t handle) {
  if (!handle || !handle->opened)
    return false;
  return handle->bodyOffset >= handle->responseBody.size();
}

inline esp_err_t
esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t,
                                             int *esp_tls_code,
                                             int *esp_tls_flags) {
  if (esp_tls_code)
    *esp_tls_code = 0;
  if (esp_tls_flags)
    *esp_tls_flags = 0;
  return ESP_OK;
}

inline esp_err_t esp_http_client_cleanup(esp_http_client_handle_t handle) {
  delete handle;
  return ESP_OK;
}

inline const char *esp_err_to_name(esp_err_t err) {
  switch (err) {
  case ESP_OK:
    return "ESP_OK";
  case ESP_FAIL:
    return "ESP_FAIL";
  default:
    return "ESP_ERR";
  }
}

extern "C" {
inline esp_err_t esp_crt_bundle_attach(void *) { return ESP_OK; }
}
