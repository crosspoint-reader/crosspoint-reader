#pragma once
#include <cstddef>
#include <string>

/**
 * Parsed OPDS Authentication Document (application/opds-authentication+json),
 * served as the body of a 401 response per Authentication for OPDS 1.0.
 * Lists the Authentication Flows a catalog supports.
 */
struct OpdsAuthDoc {
  bool hasBasic = false;          // http://opds-spec.org/auth/basic
  bool hasOauthPassword = false;  // http://opds-spec.org/auth/oauth/password
  bool hasOauthImplicit = false;  // http://opds-spec.org/auth/oauth/implicit (needs a browser)
  std::string tokenUrl;           // "authenticate" link of the OAuth password flow
  std::string refreshUrl;         // optional "refresh" link of the OAuth password flow
};

/**
 * Parse an authentication document. Returns false on malformed JSON or when
 * no authentication flows were found.
 */
bool parseOpdsAuthDocument(const char* json, size_t len, OpdsAuthDoc& out);

/**
 * Extract a top-level string field from a small JSON document (e.g.
 * "access_token" from an OAuth token response). Returns false if absent.
 */
bool extractJsonStringField(const char* json, size_t len, const char* field, std::string& out);
