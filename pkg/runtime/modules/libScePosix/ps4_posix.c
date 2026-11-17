#include "nps5_sleep.h"
#include "ps4_posix.h"
#include "ps4_threading.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <pthread.h>
#include "nps5_kio.h"

static inline void *guest_to_host(GuestContext *ctx, uint64_t addr) {
    return addr ? (void *)(ctx->mem_base + addr) : NULL;
}

void shim_socket(GuestContext *ctx) {
    int domain = (int)ctx->rdi;
    int type = (int)ctx->rsi;
    int protocol = (int)ctx->rdx;
    int ret = socket(domain, type, protocol);
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = (uint64_t)ret;
    }
    SHIM_RETURN();
}

void shim_connect(GuestContext *ctx) {
    int sockfd = (int)ctx->rdi;
    const struct sockaddr *addr = (const struct sockaddr *)guest_to_host(ctx, ctx->rsi);
    socklen_t addrlen = (socklen_t)ctx->rdx;
    int ret = connect(sockfd, addr, addrlen);
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = 0;
    }
    SHIM_RETURN();
}

void shim_bind(GuestContext *ctx) {
    int sockfd = (int)ctx->rdi;
    const struct sockaddr *addr = (const struct sockaddr *)guest_to_host(ctx, ctx->rsi);
    socklen_t addrlen = (socklen_t)ctx->rdx;
    int ret = bind(sockfd, addr, addrlen);
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = 0;
    }
    SHIM_RETURN();
}

void shim_listen(GuestContext *ctx) {
    int sockfd = (int)ctx->rdi;
    int backlog = (int)ctx->rsi;
    int ret = listen(sockfd, backlog);
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = 0;
    }
    SHIM_RETURN();
}

void shim_accept(GuestContext *ctx) {
    int sockfd = (int)ctx->rdi;
    struct sockaddr *addr = (struct sockaddr *)guest_to_host(ctx, ctx->rsi);
    socklen_t *addrlen = (socklen_t *)guest_to_host(ctx, ctx->rdx);
    int ret = nps5_kaccept(sockfd, addr, addrlen);
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = (uint64_t)ret;
    }
    SHIM_RETURN();
}

void shim_send(GuestContext *ctx) {
    int sockfd = (int)ctx->rdi;
    const void *buf = guest_to_host(ctx, ctx->rsi);
    size_t len = (size_t)ctx->rdx;
    int flags = (int)ctx->rcx;
    ssize_t ret = nps5_ksend(sockfd, buf, len, flags);
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = (uint64_t)ret;
    }
    SHIM_RETURN();
}

void shim_recv(GuestContext *ctx) {
    int sockfd = (int)ctx->rdi;
    void *buf = guest_to_host(ctx, ctx->rsi);
    size_t len = (size_t)ctx->rdx;
    int flags = (int)ctx->rcx;
    ssize_t ret = nps5_krecv(sockfd, buf, len, flags);
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = (uint64_t)ret;
    }
    SHIM_RETURN();
}

void shim_sendto(GuestContext *ctx) {
    int sockfd = (int)ctx->rdi;
    const void *buf = guest_to_host(ctx, ctx->rsi);
    size_t len = (size_t)ctx->rdx;
    int flags = (int)ctx->rcx;
    const struct sockaddr *dest_addr = (const struct sockaddr *)guest_to_host(ctx, ctx->r8);
    socklen_t addrlen = (socklen_t)ctx->r9;
    ssize_t ret = nps5_ksendto(sockfd, buf, len, flags, dest_addr, addrlen);
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = (uint64_t)ret;
    }
    SHIM_RETURN();
}

void shim_recvfrom(GuestContext *ctx) {
    int sockfd = (int)ctx->rdi;
    void *buf = guest_to_host(ctx, ctx->rsi);
    size_t len = (size_t)ctx->rdx;
    int flags = (int)ctx->rcx;
    struct sockaddr *src_addr = (struct sockaddr *)guest_to_host(ctx, ctx->r8);
    socklen_t *addrlen = (socklen_t *)guest_to_host(ctx, ctx->r9);
    ssize_t ret = nps5_krecvfrom(sockfd, buf, len, flags, src_addr, addrlen);
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = (uint64_t)ret;
    }
    SHIM_RETURN();
}

struct guest_msghdr {
    uint64_t msg_name;
    uint32_t msg_namelen;
    uint32_t _pad1;
    uint64_t msg_iov;
    int32_t  msg_iovlen;
    uint32_t _pad2;
    uint64_t msg_control;
    uint32_t msg_controllen;
    int32_t  msg_flags;
};

struct guest_iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

// m8: the guest's msghdr and iovec array are read with the non-faulting helpers: a pointer into an unmapped hole
// is EFAULT, not a host SIGSEGV; the iovec count is bounded (a wild count was an unchecked malloc). The name,
// control and iov_base pointers go to the kernel, which checks them itself.
#define NPS5_MAX_MSG_IOV 1024
static int load_guest_msg(GuestContext *ctx, uint64_t msg_addr, struct guest_msghdr *gmsg, struct msghdr *hmsg,
                          struct iovec *stack_iov, struct iovec **hiov_out) {
    *hiov_out = stack_iov;
    if (!msg_addr || !RECOMP_MEM_OK(ctx) || !nps5_guest_safe_read(msg_addr, gmsg, sizeof *gmsg)) return EFAULT;
    if (gmsg->msg_iovlen < 0 || gmsg->msg_iovlen > NPS5_MAX_MSG_IOV) return EMSGSIZE;
    memset(hmsg, 0, sizeof(*hmsg));
    hmsg->msg_name = gmsg->msg_name ? (void *)(ctx->mem_base + gmsg->msg_name) : NULL;
    hmsg->msg_namelen = gmsg->msg_namelen;
    hmsg->msg_iovlen = gmsg->msg_iovlen;
    hmsg->msg_control = gmsg->msg_control ? (void *)(ctx->mem_base + gmsg->msg_control) : NULL;
    hmsg->msg_controllen = gmsg->msg_controllen;
    hmsg->msg_flags = gmsg->msg_flags;
    if (gmsg->msg_iov && gmsg->msg_iovlen > 0) {
        struct guest_iovec gstack[16], *giov = gstack;
        if (gmsg->msg_iovlen > 16 && !(giov = (struct guest_iovec *)malloc(sizeof(*giov) * gmsg->msg_iovlen))) return ENOMEM;
        if (!nps5_guest_safe_read(gmsg->msg_iov, giov, sizeof(*giov) * gmsg->msg_iovlen)) {
            if (giov != gstack) free(giov);
            return EFAULT;
        }
        struct iovec *hiov = stack_iov;
        if (gmsg->msg_iovlen > 16 && !(hiov = (struct iovec *)malloc(sizeof(struct iovec) * gmsg->msg_iovlen))) {
            if (giov != gstack) free(giov);
            return ENOMEM;
        }
        for (int i = 0; i < gmsg->msg_iovlen; i++) {
            hiov[i].iov_base = giov[i].iov_base ? (void *)(ctx->mem_base + giov[i].iov_base) : NULL;
            hiov[i].iov_len = (size_t)giov[i].iov_len;
        }
        if (giov != gstack) free(giov);
        hmsg->msg_iov = hiov;
        *hiov_out = hiov;
    }
    return 0;
}

void shim_sendmsg(GuestContext *ctx) {
    int sockfd = (int)ctx->rdi;
    uint64_t msg_addr = ctx->rsi;
    int flags = (int)ctx->rdx;
    struct guest_msghdr gm;
    struct msghdr hmsg;
    struct iovec stack_iov[16], *hiov;
    int err = load_guest_msg(ctx, msg_addr, &gm, &hmsg, stack_iov, &hiov);
    if (err) {
        set_guest_errno(ctx, err);
        ctx->rax = (uint64_t)-1;
        SHIM_RETURN();
    }

    ssize_t ret = nps5_ksendmsg(sockfd, &hmsg, flags);
    if (hiov != stack_iov) {
        free(hiov);
    }
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = (uint64_t)ret;
    }
    SHIM_RETURN();
}

void shim_recvmsg(GuestContext *ctx) {
    int sockfd = (int)ctx->rdi;
    uint64_t msg_addr = ctx->rsi;
    int flags = (int)ctx->rdx;
    struct guest_msghdr gm;
    struct msghdr hmsg;
    struct iovec stack_iov[16], *hiov;
    int err = load_guest_msg(ctx, msg_addr, &gm, &hmsg, stack_iov, &hiov);
    if (err) {
        set_guest_errno(ctx, err);
        ctx->rax = (uint64_t)-1;
        SHIM_RETURN();
    }
    struct guest_msghdr *gmsg = (struct guest_msghdr *)(ctx->mem_base + msg_addr); // validated readable above; written back below

    ssize_t ret = nps5_krecvmsg(sockfd, &hmsg, flags);
    if (ret >= 0) {
        gmsg->msg_namelen = hmsg.msg_namelen;
        gmsg->msg_controllen = hmsg.msg_controllen;
        gmsg->msg_flags = hmsg.msg_flags;
    }
    if (hiov != stack_iov) {
        free(hiov);
    }
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = (uint64_t)ret;
    }
    SHIM_RETURN();
}

void shim_setsockopt(GuestContext *ctx) {
    int sockfd = (int)ctx->rdi;
    int level = (int)ctx->rsi;
    int optname = (int)ctx->rdx;
    const void *optval = guest_to_host(ctx, ctx->rcx);
    socklen_t optlen = (socklen_t)ctx->r8;
    int ret = setsockopt(sockfd, level, optname, optval, optlen);
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = 0;
    }
    SHIM_RETURN();
}

void shim_getsockopt(GuestContext *ctx) {
    int sockfd = (int)ctx->rdi;
    int level = (int)ctx->rsi;
    int optname = (int)ctx->rdx;
    void *optval = guest_to_host(ctx, ctx->rcx);
    socklen_t *optlen = (socklen_t *)guest_to_host(ctx, ctx->r8);
    int ret = nps5_kgetsockopt(sockfd, level, optname, optval, optlen);
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = 0;
    }
    SHIM_RETURN();
}

void shim_getsockname(GuestContext *ctx) {
    int sockfd = (int)ctx->rdi;
    struct sockaddr *addr = (struct sockaddr *)guest_to_host(ctx, ctx->rsi);
    socklen_t *addrlen = (socklen_t *)guest_to_host(ctx, ctx->rdx);
    int ret = nps5_kgetsockname(sockfd, addr, addrlen);
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = 0;
    }
    SHIM_RETURN();
}

void shim_getpeername(GuestContext *ctx) {
    int sockfd = (int)ctx->rdi;
    struct sockaddr *addr = (struct sockaddr *)guest_to_host(ctx, ctx->rsi);
    socklen_t *addrlen = (socklen_t *)guest_to_host(ctx, ctx->rdx);
    int ret = nps5_kgetpeername(sockfd, addr, addrlen);
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = 0;
    }
    SHIM_RETURN();
}

void shim_shutdown(GuestContext *ctx) {
    int sockfd = (int)ctx->rdi;
    int how = (int)ctx->rsi;
    int ret = shutdown(sockfd, how);
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = 0;
    }
    SHIM_RETURN();
}

void shim_select(GuestContext *ctx) {
    int nfds = (int)ctx->rdi;
    fd_set *readfds = (fd_set *)guest_to_host(ctx, ctx->rsi);
    fd_set *writefds = (fd_set *)guest_to_host(ctx, ctx->rdx);
    fd_set *exceptfds = (fd_set *)guest_to_host(ctx, ctx->rcx);
    struct timeval *timeout = (struct timeval *)guest_to_host(ctx, ctx->r8);
    int ret = nps5_kselect(nfds, readfds, writefds, exceptfds, timeout);
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = (uint64_t)ret;
    }
    SHIM_RETURN();
}

void shim_inet_pton(GuestContext *ctx) {
    int af = (int)ctx->rdi;
    const char *src = (const char *)guest_to_host(ctx, ctx->rsi);
    void *dst = guest_to_host(ctx, ctx->rdx);
    int ret = inet_pton(af, src, dst);
    if (ret <= 0) {
        set_guest_errno(ctx, errno);
    }
    ctx->rax = (uint64_t)ret;
    SHIM_RETURN();
}

void shim_usleep(GuestContext *ctx) {
    useconds_t usec = (useconds_t)ctx->rdi;
    int ret = nps5_sleep_ns((uint64_t)usec * 1000ull, NULL);
    if (ret < 0) {
        set_guest_errno(ctx, errno);
        ctx->rax = (uint64_t)-1;
    } else {
        ctx->rax = 0;
    }
    SHIM_RETURN();
}

void shim_getpid(GuestContext *ctx) {
    ctx->rax = (uint64_t)getpid();
    SHIM_RETURN();
}

void shim_pthread_exit(GuestContext *ctx) {
    (void)ctx;
    pthread_exit(NULL);
}

void shim_pthread_rename_np(GuestContext *ctx) {
    // pthread_rename_np(pthread_t thread, const char *name)
    // On macOS: pthread_setname_np(const char *name) for self, or nothing if external.
    const char *name = (const char *)guest_to_host(ctx, ctx->rsi);
    if (name) {
        pthread_setname_np(name);
    }
    ctx->rax = 0;
    SHIM_RETURN();
}

void shim_pthread_create_name_np(GuestContext *ctx) {
    // pthread_create_name_np(thread, attr, start_routine, arg, name)
    // Maps directly to shim_pthread_create which sets up context and spawns thread
    shim_pthread_create(ctx);
}

