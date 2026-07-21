#pragma once
#include <fcntl.h>
typedef int oflag_t;

#ifndef O_AT_END
#define O_AT_END O_APPEND
#endif

#ifndef O_READ
#define O_READ O_RDONLY
#endif

#ifndef O_WRITE
#define O_WRITE O_WRONLY
#endif
