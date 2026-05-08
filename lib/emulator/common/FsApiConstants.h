#pragma once
#include <fcntl.h>

using oflag_t = int;

#ifndef O_READ
#define O_READ O_RDONLY
#endif
#ifndef O_WRITE
#define O_WRITE O_WRONLY
#endif
