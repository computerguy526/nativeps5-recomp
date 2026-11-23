// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host test of the VFS read paths when the kernel's copy into guest memory keeps failing with EFAULT (a write watch
// closes a destination page again faster than nps5_host_write_refault opens it). ps4_vfs_read / ps4_vfs_pread must
// return the whole file (no short read before EOF, no EFAULT error) and leave the right bytes; with NPS5_KIO_BOUNCE=0
// (mode "off") the old behaviour is checked to fail (EFAULT, or a short count after an earlier partial copy), so the
// test does catch the bug.
// The "watch" here: one page of the destination is read-only; the access hooks never open it (begin is a no-op), the
// refault hook says "opened again" until the retry limit, and a SIGBUS/SIGSEGV handler opens the page on a user-mode
// store, as the real watch's fault handler does.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "nps5_mem.h"

ssize_t ps4_vfs_read(int fd, void *buf, size_t count);
ssize_t ps4_vfs_pread(int fd, void *buf, size_t count, off_t offset);

static uint8_t *g_dst;
static size_t g_len, g_page;
static volatile int g_opened; // user-mode stores the "watch" resolved
static int g_refaults;

void nps5_host_write_begin(void *p, size_t len) {
    (void)p;
    (void)len;
}
void nps5_host_write_end(void *p, size_t len) {
    (void)p;
    (void)len;
}
void nps5_host_read_prepare(const void *p, size_t len) {
    (void)p;
    (void)len;
}
bool nps5_host_access_active(void) { return true; }
bool nps5_host_write_refault(void *p, size_t len, unsigned *tries) {
    (void)p;
    (void)len;
    if (*tries >= NPS5_KIO_RETRIES) return false;
    ++*tries;
    g_refaults++;
    return true;
}

static void on_fault(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    uint8_t *a = (uint8_t *)si->si_addr;
    if (a >= g_dst && a < g_dst + g_len) {
        uint8_t *pg = (uint8_t *)((uintptr_t)a & ~(uintptr_t)(g_page - 1));
        mprotect(pg, g_page, PROT_READ | PROT_WRITE);
        g_opened++;
        return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

static void arm(void) {
    memset(g_dst, 0xee, g_len); // stale contents
    mprotect(g_dst + g_len / 2, g_page, PROT_READ); // a watched page in the middle
    g_opened = 0;
    g_refaults = 0;
}

int main(int argc, char **argv) {
    const bool off = argc > 1 && strcmp(argv[1], "off") == 0;
    if (off) setenv("NPS5_KIO_BOUNCE", "0", 1);
    g_page = (size_t)getpagesize();
    g_len = 8 * 1024 * 1024; // a multiple of the page size
    const char *td = getenv("TMPDIR");
    char tpl[1024];
    snprintf(tpl, sizeof tpl, "%s/kio_bounce_unit.XXXXXX", td && *td ? td : "/tmp");
    int fd = mkstemp(tpl);
    if (fd < 0) {
        perror("mkstemp");
        return 2;
    }
    uint8_t *src = malloc(g_len);
    for (size_t i = 0; i < g_len; i++)
        src[i] = (uint8_t)(i * 131u + (i >> 12));
    if (write(fd, src, g_len) != (ssize_t)g_len) {
        perror("write");
        return 2;
    }
    g_dst = mmap(NULL, g_len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);

    // pread
    arm();
    ssize_t r = ps4_vfs_pread(fd, g_dst, g_len, 0);
    if (off) {
        CHECK(r != (ssize_t)g_len, "off: pread should fail or come back short (old behaviour), got %zd of %zu", r,
              g_len);
        CHECK(g_refaults == (int)NPS5_KIO_RETRIES, "off: refaults %d", g_refaults);
    } else {
        CHECK(r == (ssize_t)g_len, "pread returned %zd of %zu", r, g_len);
        CHECK(memcmp(g_dst, src, g_len) == 0, "pread bytes differ");
        CHECK(g_refaults == (int)NPS5_KIO_RETRIES, "pread refaults %d, want %u", g_refaults, NPS5_KIO_RETRIES);
        CHECK(g_opened >= 1, "pread: the bounce copy never stored into the watched page");
    }

    // read (sequential): also the file position after the read
    arm();
    lseek(fd, 0, SEEK_SET);
    r = ps4_vfs_read(fd, g_dst, g_len);
    const off_t pos = lseek(fd, 0, SEEK_CUR);
    if (off) {
        CHECK(r != (ssize_t)g_len, "off: read should fail or come back short (old behaviour), got %zd", r);
    } else {
        CHECK(r == (ssize_t)g_len, "read returned %zd of %zu", r, g_len);
        CHECK(pos == (off_t)g_len, "file position after read %lld, want %zu", (long long)pos, g_len);
        CHECK(memcmp(g_dst, src, g_len) == 0, "read bytes differ");
        CHECK(g_opened >= 1, "read: the bounce copy never stored into the watched page");
        // a read past EOF after it: 0, not an error
        r = ps4_vfs_read(fd, g_dst, 16);
        CHECK(r == 0, "read at EOF returned %zd", r);
    }

    // pread from an offset, ending at EOF (asks more than the file has)
    if (!off) {
        arm();
        const size_t at = g_len / 4;
        r = ps4_vfs_pread(fd, g_dst, g_len, (off_t)at);
        CHECK(r == (ssize_t)(g_len - at), "pread at offset returned %zd, want %zu", r, g_len - at);
        CHECK(memcmp(g_dst, src + at, g_len - at) == 0, "pread at offset bytes differ");
    }

    close(fd);
    unlink(tpl);
    if (fails) {
        fprintf(stderr, "kio_bounce_unit (%s): %d failure(s)\n", off ? "off" : "on", fails);
        return 1;
    }
    printf("kio_bounce_unit (%s): ok\n", off ? "off" : "on");
    return 0;
}
