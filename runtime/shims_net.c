/* BSD sockets and name lookup. sockaddr, fd_set and socklen_t have the same
 * layout for i386 and the host; struct timeval (8 vs 16 bytes), hostent and
 * ifaddrs are converted. Used by LAN/online multiplayer and by game hosting
 * (also for single-player sessions). */
#include "shim.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

void rt_set_errno(int e);
#define ERRNO_RET(expr) do { long r_ = (long)(expr); if (r_ < 0) rt_set_errno(errno); RET(r_); } while (0)

static void *opt(uint32_t g) { return g ? PTR(g) : NULL; }
static socklen_t *lenp(uint32_t g) { return g ? (socklen_t *)PTR(g) : NULL; }

SHIM(_socket) { ERRNO_RET(socket((int)ARG(0), (int)ARG(1), (int)ARG(2))); }
SHIM(_bind$UNIX2003) { ERRNO_RET(bind((int)ARG(0), opt(ARG(1)), (socklen_t)ARG(2))); }
SHIM(_connect$UNIX2003) { ERRNO_RET(connect((int)ARG(0), opt(ARG(1)), (socklen_t)ARG(2))); }
SHIM(_listen$UNIX2003) { ERRNO_RET(listen((int)ARG(0), (int)ARG(1))); }
SHIM(_accept$UNIX2003) { ERRNO_RET(accept((int)ARG(0), opt(ARG(1)), lenp(ARG(2)))); }
SHIM(_shutdown) { ERRNO_RET(shutdown((int)ARG(0), (int)ARG(1))); }
SHIM(_getsockname$UNIX2003) { ERRNO_RET(getsockname((int)ARG(0), opt(ARG(1)), lenp(ARG(2)))); }
SHIM(_send$UNIX2003) { ERRNO_RET(send((int)ARG(0), opt(ARG(1)), ARG(2), (int)ARG(3))); }
SHIM(_recv$UNIX2003) { ERRNO_RET(recv((int)ARG(0), opt(ARG(1)), ARG(2), (int)ARG(3))); }
SHIM(_sendto$UNIX2003) {
    ERRNO_RET(sendto((int)ARG(0), opt(ARG(1)), ARG(2), (int)ARG(3), opt(ARG(4)), (socklen_t)ARG(5)));
}
SHIM(_recvfrom) { ERRNO_RET(recvfrom((int)ARG(0), opt(ARG(1)), ARG(2), (int)ARG(3), opt(ARG(4)), lenp(ARG(5)))); }
SHIM(_recvfrom$UNIX2003) {
    ERRNO_RET(recvfrom((int)ARG(0), opt(ARG(1)), ARG(2), (int)ARG(3), opt(ARG(4)), lenp(ARG(5))));
}

/* Socket options: timeouts are struct timeval, which differs in size. */
SHIM(_setsockopt) {
    int level = (int)ARG(1), name = (int)ARG(2);
    if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO) && ARG(3)) {
        struct timeval tv = {(int32_t)R32_(g_mem, ARG(3)), (int32_t)R32_(g_mem, ARG(3) + 4)};
        ERRNO_RET(setsockopt((int)ARG(0), level, name, &tv, sizeof tv));
        return;
    }
    ERRNO_RET(setsockopt((int)ARG(0), level, name, opt(ARG(3)), (socklen_t)ARG(4)));
}
SHIM(_getsockopt) {
    int level = (int)ARG(1), name = (int)ARG(2);
    if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO) && ARG(3)) {
        struct timeval tv;
        socklen_t n = sizeof tv;
        int r = getsockopt((int)ARG(0), level, name, &tv, &n);
        if (r < 0) { rt_set_errno(errno); RET(r); return; }
        W32_(g_mem, ARG(3), (uint32_t)tv.tv_sec);
        W32_(g_mem, ARG(3) + 4, (uint32_t)tv.tv_usec);
        if (ARG(4)) W32_(g_mem, ARG(4), 8);
        RET(0);
        return;
    }
    ERRNO_RET(getsockopt((int)ARG(0), level, name, opt(ARG(3)), lenp(ARG(4))));
}

/* select(nfds, read, write, except, struct timeval *): fd_set layouts match. */
static void do_select(CPU *cpu) {
    struct timeval tv, *tp = NULL;
    if (ARG(4)) {
        tv.tv_sec = (int32_t)R32_(g_mem, ARG(4));
        tv.tv_usec = (int32_t)R32_(g_mem, ARG(4) + 4);
        tp = &tv;
    }
    int r = select((int)ARG(0), opt(ARG(1)), opt(ARG(2)), opt(ARG(3)), tp);
    if (r < 0) rt_set_errno(errno);
    if (tp) {
        W32_(g_mem, ARG(4), (uint32_t)tv.tv_sec);
        W32_(g_mem, ARG(4) + 4, (uint32_t)tv.tv_usec);
    }
    RET(r);
}
SHIM(_select$DARWIN_EXTSN) { do_select(cpu); }
SHIM(_select$UNIX2003) { do_select(cpu); }

/* ---- name lookup ---- */
/* struct hostent (i386): {char *h_name; char **h_aliases; int h_addrtype; int h_length; char **h_addr_list} */
SHIM(_gethostbyname) {
    static uint32_t he, name, aliases, list, addrs;
    struct hostent *h = gethostbyname(CPTR(ARG(0)));
    if (!h) { RET(0); return; }
    if (!he) {
        he = gcalloc(1, 20);
        aliases = gcalloc(1, 4);
        list = gcalloc(9, 4);
        addrs = gcalloc(8, 16);
    }
    if (name) gfree(name);
    name = gstrdup(h->h_name ? h->h_name : "");
    int n = 0;
    for (; h->h_addr_list[n] && n < 8 && h->h_length <= 16; n++) {
        memcpy(PTR(addrs + 16 * n), h->h_addr_list[n], (size_t)h->h_length);
        W32_(g_mem, list + 4 * n, addrs + 16 * n);
    }
    W32_(g_mem, list + 4 * n, 0);
    W32_(g_mem, he, name);
    W32_(g_mem, he + 4, aliases);
    W32_(g_mem, he + 8, (uint32_t)h->h_addrtype);
    W32_(g_mem, he + 12, (uint32_t)h->h_length);
    W32_(g_mem, he + 16, list);
    RET(he);
}
SHIM(_inet_ntoa) {
    static uint32_t buf;
    if (!buf) buf = gcalloc(1, 32);
    struct in_addr a = {ARG(0)};
    strlcpy(PTR(buf), inet_ntoa(a), 32);
    RET(buf);
}

/* struct ifaddrs (i386): {next, name, flags, addr, netmask, dstaddr, data}; one guest block per list. */
static uint32_t copy_sa(const struct sockaddr *sa) {
    if (!sa) return 0;
    uint32_t n = sa->sa_len ? sa->sa_len : sizeof(struct sockaddr), g = gcalloc(1, n);
    memcpy(PTR(g), sa, n);
    return g;
}
SHIM(_getifaddrs) {
    struct ifaddrs *list;
    if (getifaddrs(&list)) { rt_set_errno(errno); RET(-1); return; }
    uint32_t head = 0, prev = 0;
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        uint32_t g = gcalloc(1, 28);
        W32_(g_mem, g + 4, gstrdup(i->ifa_name));
        W32_(g_mem, g + 8, i->ifa_flags);
        W32_(g_mem, g + 12, copy_sa(i->ifa_addr));
        W32_(g_mem, g + 16, copy_sa(i->ifa_netmask));
        W32_(g_mem, g + 20, copy_sa(i->ifa_dstaddr));
        if (prev) W32_(g_mem, prev, g); else head = g;
        prev = g;
    }
    freeifaddrs(list);
    W32_(g_mem, ARG(0), head);
    RET(0);
}
SHIM(_freeifaddrs) {
    for (uint32_t g = ARG(0), next; g; g = next) {
        next = R32_(g_mem, g);
        for (int k = 1; k <= 5; k++)
            if (k != 2 && R32_(g_mem, g + 4 * k)) gfree(R32_(g_mem, g + 4 * k));
        gfree(g);
    }
}

const ShimDef shims_net[] = {
    SHIM_ENTRY(_socket), SHIM_ENTRY(_bind$UNIX2003), SHIM_ENTRY(_connect$UNIX2003), SHIM_ENTRY(_listen$UNIX2003),
    SHIM_ENTRY(_accept$UNIX2003), SHIM_ENTRY(_shutdown), SHIM_ENTRY(_getsockname$UNIX2003),
    SHIM_ENTRY(_send$UNIX2003), SHIM_ENTRY(_recv$UNIX2003), SHIM_ENTRY(_sendto$UNIX2003), SHIM_ENTRY(_recvfrom),
    SHIM_ENTRY(_recvfrom$UNIX2003), SHIM_ENTRY(_setsockopt), SHIM_ENTRY(_getsockopt),
    SHIM_ENTRY(_select$DARWIN_EXTSN), SHIM_ENTRY(_select$UNIX2003), SHIM_ENTRY(_gethostbyname),
    SHIM_ENTRY(_inet_ntoa), SHIM_ENTRY(_getifaddrs), SHIM_ENTRY(_freeifaddrs),
    {0, 0},
};
