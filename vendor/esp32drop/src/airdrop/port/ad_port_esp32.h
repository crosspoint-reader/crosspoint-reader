#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../core/ad_ask.h"
#include "../core/ad_dnssd.h"
#include "../core/ad_status.h"

// The main task calls ad_poll(), which exclusively invokes storage callbacks.
// Data remains borrowed until callback return. Finish validates the archive;
// Abort removes partial output. Callbacks must not stop or navigate the activity.
enum class AdStreamEvent { Begin, Data, Finish, Abort };
using ad_stream_cb_t = bool (*)(void*, AdStreamEvent, const uint8_t*, uint32_t);
void ad_on_stream(ad_stream_cb_t cb, void* ctx);

// Prepare TLS before AWDL consumes internal heap. Stop joins the listener before
// releasing its resources; false means the caller must not destroy callback state.
bool ad_prepare_tls();
int ad_begin(uint32_t max_receive);
const char* ad_receiver_name();
bool ad_end();
void ad_poll();
bool ad_upload_pending();
bool ad_request_pending();
bool ad_request_answer(bool accept);
bool ad_request_info(AdAskInfo* out);
void ad_reject_too_large();
void ad_stats_read(AdStatus* out);
uint32_t ad_listen_stack_min_free();
