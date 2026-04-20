#pragma once

class BookFusionTokenStore;

namespace BookFusionJsonIO {
bool save(const BookFusionTokenStore& store, const char* path);
bool load(BookFusionTokenStore& store, const char* json);
}  // namespace BookFusionJsonIO
