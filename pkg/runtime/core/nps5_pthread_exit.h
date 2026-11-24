// POSIX pthread_exit routing (modules/libScePosix/ps4_posix.c shim_pthread_exit).
// NPS5_PTHREAD_EXIT_SCE=1 sends a guest pthread_exit through scePthreadExit (TLS destructors, cleanup handlers,
// return value, wake-up of the joiner); anything else keeps the bare host pthread_exit the other titles were
// validated with. Pure logic: unit-tested in tests/threading/pthread_exit_unit.c.
#ifndef NPS5_PTHREAD_EXIT_H
#define NPS5_PTHREAD_EXIT_H
#include <stddef.h>

static inline int nps5_pthread_exit_sce_enabled(const char *env) {
    return env != NULL && env[0] == '1';
}

#endif
