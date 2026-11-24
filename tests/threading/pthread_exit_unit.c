// Unit test of the NPS5_PTHREAD_EXIT_SCE routing decision (core/nps5_pthread_exit.h) and of the two exit paths
// it selects, on host threads: the SCE path runs the recorded "scePthreadExit" step (joiner wake-up in the
// runtime) before the thread ends and the joiner is released; the switch-off path takes the bare host exit
// and never runs it. Pure logic plus pthreads, no guest code.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "nps5_pthread_exit.h"
static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)
static volatile int g_sce_calls;
static const char *g_env;
static void *thr(void *arg) {
    (void)arg;
    if (nps5_pthread_exit_sce_enabled(g_env)) g_sce_calls++; /* shim_scePthreadExit: wakes the joiner, then exits */
    pthread_exit((void *)0x42);
    return NULL;
}
static int run_and_join(const char *env, void **ret) {
    g_env = env;
    g_sce_calls = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, thr, NULL) != 0) return -1;
    return pthread_join(t, ret);
}
int main(void) {
    CHECK(!nps5_pthread_exit_sce_enabled(NULL));
    CHECK(!nps5_pthread_exit_sce_enabled(""));
    CHECK(!nps5_pthread_exit_sce_enabled("0"));
    CHECK(!nps5_pthread_exit_sce_enabled("true"));
    CHECK(nps5_pthread_exit_sce_enabled("1"));
    CHECK(nps5_pthread_exit_sce_enabled("1x"));
    void *ret = NULL;
    CHECK(run_and_join("1", &ret) == 0 && ret == (void *)0x42 && g_sce_calls == 1); /* join returns, SCE path ran */
    ret = NULL;
    CHECK(run_and_join(NULL, &ret) == 0 && ret == (void *)0x42 && g_sce_calls == 0); /* off: host exit only */
    CHECK(run_and_join("0", &ret) == 0 && g_sce_calls == 0);
    printf(fails ? "pthread_exit_unit FAILED (%d)\n" : "pthread_exit_unit OK\n", fails);
    return fails != 0;
}
