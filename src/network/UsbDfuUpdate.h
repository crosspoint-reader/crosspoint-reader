#pragma once
#include <cstddef>
#include <cstdint>

namespace usb_dfu {
enum class Stage { Waiting, Receiving, Verifying, Failed, Complete };
using Prepare = bool (*)(void*);
using Progress = void (*)(Stage, size_t, size_t, void*);
bool run(Progress progress, void* context, Prepare prepare = nullptr, bool (*cancelled)() = nullptr,
         bool background = false, void (*linkState)(bool) = nullptr);
}  // namespace usb_dfu
