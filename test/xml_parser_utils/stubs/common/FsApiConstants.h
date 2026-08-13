#pragma once

// Host-test stub for SdFat's common/FsApiConstants.h. HalStorage only needs the
// oflag_t alias and O_RDONLY default argument.

using oflag_t = int;

constexpr int O_RDONLY = 0x0000;
constexpr int O_WRONLY = 0x0001;
constexpr int O_RDWR = 0x0002;
constexpr int O_CREAT = 0x0100;
constexpr int O_APPEND = 0x0008;
constexpr int O_TRUNC = 0x0200;
