#pragma once

#include <WebServer.h>

// How long an upload waits for its next data before it is given up. The libraries' own limits,
// WebServer's HTTP_MAX_SEND_WAIT and the WebSocket library's WEBSOCKETS_TCP_TIMEOUT, are 5 s: too
// short for a weak link (around -85 dBm), whose stalls run longer while the sender's
// retransmissions back off. Waiting longer costs a frozen device while it waits, as nothing else
// runs - Back included - until data comes or the wait ends.
//
// One value for both upload paths. It is set in platformio.ini, as WEBSOCKETS_TCP_TIMEOUT, because
// only a build flag reaches inside the WebSocket library.
#ifndef WEBSOCKETS_TCP_TIMEOUT
#error "WEBSOCKETS_TCP_TIMEOUT is set in platformio.ini's build_flags"
#endif
constexpr unsigned long UPLOAD_STALL_TIMEOUT_MS = WEBSOCKETS_TCP_TIMEOUT;

// Called when an upload starts. The limit is the request's client's read timeout, which both the
// multipart and the raw body readers wait on; WebServer drops the client after the request, so the
// next one starts at 5 s again.
inline void tolerateUploadStalls(WebServer& server) { server.client().setTimeout(UPLOAD_STALL_TIMEOUT_MS); }
