#pragma once

#include <ArduinoJson.h>
#include <WString.h>

#include "RecentBooksStore.h"
#include "util/BookProgressDataStore.h"

namespace network {

void appendBookProgressJson(JsonObject target, const BookProgressDataStore::ProgressData& progress);
String buildRecentBookJson(const RecentBook& book, bool includePokemon);

}  // namespace network
