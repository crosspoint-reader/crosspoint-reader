#pragma once

#include <WebServer.h>

// How long an upload waits for its next data before WebServer gives up on it. WebServer's own
// limit, HTTP_MAX_SEND_WAIT, is 5 s: too short for a weak link (around -85 dBm), whose stalls run
// longer while the sender's retransmissions back off. Waiting longer costs a frozen device while it
// waits, as nothing else runs - Back included - until data comes or the wait ends.
constexpr unsigned long UPLOAD_STALL_TIMEOUT_MS = 10000;

// Called when an upload starts. The limit is the request's client's read timeout, which both the
// multipart and the raw body readers wait on; WebServer drops the client after the request, so the
// next one starts at 5 s again.
inline void tolerateUploadStalls(WebServer& server) { server.client().setTimeout(UPLOAD_STALL_TIMEOUT_MS); }
