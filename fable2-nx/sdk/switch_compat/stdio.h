// Nintendo Switch / newlib compatibility for the few POSIX stdio names that
// Horizon's libc does not expose to strict C++ (-std=c++23). The functions are
// implemented by newlib; only the declarations are missing there.
#pragma once

#include_next <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

int fileno(FILE* stream);
FILE* fdopen(int fd, const char* mode);
int putc_unlocked(int c, FILE* stream);

#ifdef __cplusplus
}
#endif
