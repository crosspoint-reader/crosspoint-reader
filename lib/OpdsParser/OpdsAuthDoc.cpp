#include "OpdsAuthDoc.h"

#include <StreamingJsonParser.h>

#include <cstring>

#include "OpdsEntry.h"

namespace {

// SAX state for the authentication document. Field order inside an
// authentication object is not guaranteed (`type` may follow `links`), so
// each object accumulates into `current` and is applied on object end.
struct AuthDocCtx {
  OpdsAuthDoc* out = nullptr;

  uint8_t depth = 0;
  // Depth of the "authentication" array; 0 while outside it.
  uint8_t authArrayDepth = 0;
  // Depth of the current authentication object.
  uint8_t authObjectDepth = 0;
  // Depth of the current link object inside the authentication object.
  uint8_t linkObjectDepth = 0;
  bool inAuthLinks = false;

  char pendingKey[24] = {0};

  struct {
    bool isBasic = false;
    bool isOauthPassword = false;
    bool isOauthImplicit = false;
    std::string authenticateHref;
    std::string refreshHref;
  } current;

  struct {
    std::string href;
    bool relAuthenticate = false;
    bool relRefresh = false;
  } link;
};

void applyRel(AuthDocCtx& ctx, const char* rel) {
  if (strcmp(rel, "authenticate") == 0) ctx.link.relAuthenticate = true;
  if (strcmp(rel, "refresh") == 0) ctx.link.relRefresh = true;
}

void onKey(void* ud, const char* key, size_t len) {
  auto& ctx = *static_cast<AuthDocCtx*>(ud);
  const size_t max = sizeof(ctx.pendingKey) - 1;
  if (len > max) len = max;
  memcpy(ctx.pendingKey, key, len);
  ctx.pendingKey[len] = '\0';
}

void onString(void* ud, const char* value, size_t len) {
  auto& ctx = *static_cast<AuthDocCtx*>(ud);
  if (ctx.linkObjectDepth != 0 && ctx.depth == ctx.linkObjectDepth) {
    if (strcmp(ctx.pendingKey, "href") == 0) {
      ctx.link.href.assign(value, len < OpdsLimits::MAX_HREF_CHARS ? len : OpdsLimits::MAX_HREF_CHARS);
    } else if (strcmp(ctx.pendingKey, "rel") == 0) {
      applyRel(ctx, value);
    }
  } else if (ctx.linkObjectDepth != 0 && ctx.depth == ctx.linkObjectDepth + 1) {
    // Inside a "rel" array of the link object.
    if (strcmp(ctx.pendingKey, "rel") == 0) applyRel(ctx, value);
  } else if (ctx.authObjectDepth != 0 && ctx.depth == ctx.authObjectDepth && strcmp(ctx.pendingKey, "type") == 0) {
    if (strstr(value, "auth/basic") != nullptr) ctx.current.isBasic = true;
    if (strstr(value, "auth/oauth/password") != nullptr) ctx.current.isOauthPassword = true;
    if (strstr(value, "auth/oauth/implicit") != nullptr) ctx.current.isOauthImplicit = true;
  }
}

void onObjectStart(void* ud) {
  auto& ctx = *static_cast<AuthDocCtx*>(ud);
  ++ctx.depth;
  if (ctx.authArrayDepth != 0 && ctx.depth == ctx.authArrayDepth + 1 && ctx.authObjectDepth == 0) {
    ctx.authObjectDepth = ctx.depth;
    ctx.current = {};
  } else if (ctx.inAuthLinks && ctx.linkObjectDepth == 0 && ctx.depth == ctx.authObjectDepth + 2) {
    ctx.linkObjectDepth = ctx.depth;
    ctx.link = {};
  }
  ctx.pendingKey[0] = '\0';
}

void onObjectEnd(void* ud) {
  auto& ctx = *static_cast<AuthDocCtx*>(ud);
  if (ctx.linkObjectDepth != 0 && ctx.depth == ctx.linkObjectDepth) {
    if (!ctx.link.href.empty()) {
      if (ctx.link.relAuthenticate && ctx.current.authenticateHref.empty()) {
        ctx.current.authenticateHref = std::move(ctx.link.href);
      } else if (ctx.link.relRefresh && ctx.current.refreshHref.empty()) {
        ctx.current.refreshHref = std::move(ctx.link.href);
      }
    }
    ctx.linkObjectDepth = 0;
  } else if (ctx.authObjectDepth != 0 && ctx.depth == ctx.authObjectDepth) {
    if (ctx.current.isBasic) ctx.out->hasBasic = true;
    if (ctx.current.isOauthImplicit) ctx.out->hasOauthImplicit = true;
    if (ctx.current.isOauthPassword) {
      ctx.out->hasOauthPassword = true;
      if (ctx.out->tokenUrl.empty()) ctx.out->tokenUrl = std::move(ctx.current.authenticateHref);
      if (ctx.out->refreshUrl.empty()) ctx.out->refreshUrl = std::move(ctx.current.refreshHref);
    }
    ctx.authObjectDepth = 0;
  }
  if (ctx.depth > 0) --ctx.depth;
  ctx.pendingKey[0] = '\0';
}

void onArrayStart(void* ud) {
  auto& ctx = *static_cast<AuthDocCtx*>(ud);
  ++ctx.depth;
  if (ctx.authArrayDepth == 0 && ctx.depth == 2 && strcmp(ctx.pendingKey, "authentication") == 0) {
    ctx.authArrayDepth = ctx.depth;
  } else if (ctx.authObjectDepth != 0 && ctx.depth == ctx.authObjectDepth + 1 && strcmp(ctx.pendingKey, "links") == 0) {
    ctx.inAuthLinks = true;
  }
}

void onArrayEnd(void* ud) {
  auto& ctx = *static_cast<AuthDocCtx*>(ud);
  if (ctx.authArrayDepth != 0 && ctx.depth == ctx.authArrayDepth) ctx.authArrayDepth = 0;
  if (ctx.inAuthLinks && ctx.depth == ctx.authObjectDepth + 1) ctx.inAuthLinks = false;
  if (ctx.depth > 0) --ctx.depth;
  ctx.pendingKey[0] = '\0';
}

// Context for extractJsonStringField: match a key at depth 1 only.
struct FieldCtx {
  const char* field = nullptr;
  std::string* out = nullptr;
  uint8_t depth = 0;
  bool keyMatches = false;
  bool found = false;
};

void fieldOnKey(void* ud, const char* key, size_t) {
  auto& ctx = *static_cast<FieldCtx*>(ud);
  ctx.keyMatches = ctx.depth == 1 && strcmp(key, ctx.field) == 0;
}

void fieldOnString(void* ud, const char* value, size_t len) {
  auto& ctx = *static_cast<FieldCtx*>(ud);
  if (ctx.keyMatches && !ctx.found) {
    ctx.out->assign(value, len);
    ctx.found = true;
  }
  ctx.keyMatches = false;
}

void fieldOnObjectStart(void* ud) { ++static_cast<FieldCtx*>(ud)->depth; }
void fieldOnObjectEnd(void* ud) {
  auto& ctx = *static_cast<FieldCtx*>(ud);
  if (ctx.depth > 0) --ctx.depth;
}

}  // namespace

bool parseOpdsAuthDocument(const char* json, const size_t len, OpdsAuthDoc& out) {
  out = OpdsAuthDoc{};
  AuthDocCtx ctx;
  ctx.out = &out;
  JsonCallbacks callbacks{&ctx,    &onKey,         &onString,    nullptr,       nullptr,
                          nullptr, &onObjectStart, &onObjectEnd, &onArrayStart, &onArrayEnd};
  StreamingJsonParser parser(callbacks);
  parser.feed(json, len);
  if (parser.hasError()) return false;
  return out.hasBasic || out.hasOauthPassword || out.hasOauthImplicit;
}

bool extractJsonStringField(const char* json, const size_t len, const char* field, std::string& out) {
  FieldCtx ctx;
  ctx.field = field;
  ctx.out = &out;
  JsonCallbacks callbacks{&ctx,    &fieldOnKey,         &fieldOnString,    nullptr, nullptr,
                          nullptr, &fieldOnObjectStart, &fieldOnObjectEnd, nullptr, nullptr};
  StreamingJsonParser parser(callbacks);
  parser.feed(json, len);
  return ctx.found && !parser.hasError();
}
