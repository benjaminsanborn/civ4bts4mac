/* POSIX shims beyond plain libc: directories, process info, time, signals,
 * condition variables, misc math. Mostly needed by the guest CPython. */
#include "shim.h"

#include <dirent.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <langinfo.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

void rt_set_errno(int e);
#define ERRNO_RET(expr) do { long r_ = (long)(expr); if (r_ < 0) rt_set_errno(errno); RET(r_); } while (0)

/* ---- stdio helpers shared with shims_stdio.c ---- */
extern const ShimDef shims_stdio[];
static GuestFn stdio_fn(const char *name) {
    for (const ShimDef *d = shims_stdio; d->name; d++)
        if (!strcmp(d->name, name)) return d->fn;
    rt_fatal("no stdio shim %s", name);
}
#define FORWARD(alias, real) SHIM(alias) { static GuestFn f; if (!f) f = stdio_fn(real); f(cpu); }
FORWARD(_fopen$DARWIN_EXTSN, "_fopen$UNIX2003")
FORWARD(_fdopen$DARWIN_EXTSN, "_fdopen$UNIX2003")
FORWARD(_fputs, "_fputs$UNIX2003")
FORWARD(_fwrite, "_fwrite$UNIX2003")
SHIM(_chmod$UNIX2003) { int r = chmod(CPTR(ARG(0)), (mode_t)ARG(1)); if (r) rt_set_errno(errno); RET(r); }

FILE *rt_host_file(uint32_t g);

/* Darwin's getc() macro decrements FILE._r and calls __srget when it goes
 * negative. Guest FILEs keep _r at 0, so every getc lands here. */
SHIM(___srget) {
    W32_(g_mem, ARG(0) + 4, 0);
    RET(fgetc(rt_host_file(ARG(0))));
}
SHIM(_fgets) {
    char *r = fgets(PTR(ARG(0)), (int)ARG(1), rt_host_file(ARG(2)));
    RET(r ? ARG(0) : 0);
}
SHIM(_clearerr) { clearerr(rt_host_file(ARG(0))); }
SHIM(_flockfile) {}
SHIM(_funlockfile) {}
SHIM(_fseeko) { RET(fseeko(rt_host_file(ARG(0)), (off_t)(int64_t)ARG64(1), (int)ARG(3))); }
SHIM(_ftello) { RET64(ftello(rt_host_file(ARG(0)))); }
SHIM(_putchar) { RET(putchar((int)ARG(0))); }
SHIM(_perror) { perror(CPTR(ARG(0))); }
SHIM(_tmpfile) { rt_fatal("tmpfile not supported"); }
SHIM(_vfprintf) {
    /* Route through the guest-aware formatter via vsnprintf, then write. */
    static GuestFn vsn;
    if (!vsn) vsn = stdio_fn("_vsnprintf");
    uint32_t buf = gmalloc(8192);
    /* Build a fake frame for vsnprintf(buf, 8192, fmt, va) below the current one. */
    uint32_t esp = cpu->r[4], f = ARG(0), fmt = ARG(1), va = ARG(2);
    cpu->r[4] = esp - 20;
    W32_(g_mem, cpu->r[4] + 4, buf);
    W32_(g_mem, cpu->r[4] + 8, 8192);
    W32_(g_mem, cpu->r[4] + 12, fmt);
    W32_(g_mem, cpu->r[4] + 16, va);
    vsn(cpu);
    cpu->r[4] = esp;
    uint32_t n = cpu->r[0];
    fwrite(PTR(buf), 1, n < 8191 ? n : 8191, rt_host_file(f));
    gfree(buf);
    RET(n);
}

/* ---- strings / misc libc ---- */
SHIM(_strpbrk) { RET(GADDR(strpbrk(CPTR(ARG(0)), CPTR(ARG(1))))); }
SHIM(___strncpy_chk) { strncpy(PTR(ARG(0)), CPTR(ARG(1)), ARG(2)); RET(ARG(0)); }
SHIM(_strerror$UNIX2003) {
    static uint32_t buf;
    if (!buf) buf = gmalloc(256);
    strlcpy(PTR(buf), strerror((int)ARG(0)), 256);
    RET(buf);
}
SHIM(_memset_pattern16) {
    uint8_t *d = PTR(ARG(0));
    const uint8_t *p = PTR(ARG(1));
    for (uint32_t i = 0; i < ARG(2); i++) d[i] = p[i & 15];
}
SHIM(_putenv$UNIX2003) { RET(putenv(strdup(CPTR(ARG(0))))); }
SHIM(_unsetenv$UNIX2003) { RET(unsetenv(CPTR(ARG(0)))); }
SHIM(_localeconv) {
    /* struct lconv (i386): 10 char* then 8 chars ... ; "C" locale values. */
    static uint32_t lc;
    if (!lc) {
        lc = gcalloc(1, 64);
        uint32_t dot = gstrdup("."), empty = gstrdup("");
        W32_(g_mem, lc, dot);
        for (int i = 1; i < 10; i++) W32_(g_mem, lc + 4 * i, empty);
        memset(PTR(lc + 40), 127, 8); /* CHAR_MAX: not available */
    }
    RET(lc);
}
SHIM(_nl_langinfo) {
    static uint32_t s;
    if (!s) s = gstrdup("UTF-8");
    RET(s);
}

/* ---- math ---- */
SHIM(_sqrt) { RETF(sqrt(ARGF64(0))); }
SHIM(_fabs) { RETF(fabs(ARGF64(0))); }
SHIM(_log) { RETF(log(ARGF64(0))); }
SHIM(_log10) { RETF(log10(ARGF64(0))); }
SHIM(_log1p) { RETF(log1p(ARGF64(0))); }
SHIM(_asin) { RETF(asin(ARGF64(0))); }
SHIM(_sinh) { RETF(sinh(ARGF64(0))); }
SHIM(_cosh) { RETF(cosh(ARGF64(0))); }
SHIM(_tanh) { RETF(tanh(ARGF64(0))); }
SHIM(_asinh) { RETF(asinh(ARGF64(0))); }
SHIM(_acosh) { RETF(acosh(ARGF64(0))); }
SHIM(_atanh) { RETF(atanh(ARGF64(0))); }
SHIM(_hypot) { RETF(hypot(ARGF64(0), ARGF64(2))); }
SHIM(_copysign) { RETF(copysign(ARGF64(0), ARGF64(2))); }
SHIM(_ldexp) { RETF(ldexp(ARGF64(0), (int)ARG(2))); }
SHIM(_frexp) { int e; double m = frexp(ARGF64(0), &e); W32_(g_mem, ARG(2), (uint32_t)e); RETF(m); }

/* ---- files and directories ---- */
extern void rt_stat64_to_guest(const struct stat *s, uint32_t g);
SHIM(_lstat$INODE64) {
    struct stat s;
    int r = lstat(CPTR(ARG(0)), &s);
    if (r) rt_set_errno(errno); else rt_stat64_to_guest(&s, ARG(1));
    RET(r);
}
SHIM(_getcwd) {
    if (!getcwd(PTR(ARG(0)), ARG(1))) { rt_set_errno(errno); RET(0); return; }
    RET(ARG(0));
}
SHIM(_readlink) { ERRNO_RET(readlink(CPTR(ARG(0)), PTR(ARG(1)), ARG(2))); }
SHIM(_realpath$DARWIN_EXTSN) {
    char buf[PATH_MAX];
    if (!realpath(CPTR(ARG(0)), buf)) { rt_set_errno(errno); RET(0); return; }
    uint32_t out = ARG(1) ? ARG(1) : gmalloc(PATH_MAX);
    strlcpy(PTR(out), buf, PATH_MAX);
    RET(out);
}
SHIM(_rmdir) { ERRNO_RET(rmdir(CPTR(ARG(0)))); }
SHIM(_isatty) { RET(isatty((int)ARG(0))); }
SHIM(_dup) { ERRNO_RET(dup((int)ARG(0))); }
SHIM(_dup2) { ERRNO_RET(dup2((int)ARG(0), (int)ARG(1))); }
SHIM(_fcntl$UNIX2003) { ERRNO_RET(fcntl((int)ARG(0), (int)ARG(1), (int)ARG(2))); }
SHIM(_umask) { RET(umask((mode_t)ARG(0))); }
SHIM(_utime) { RET(0); }
SHIM(_fsync$UNIX2003) { RET(fsync((int)ARG(0))); }

/* DIR* handles: guest gets a small cookie; struct dirent is the 64-bit-inode
 * layout: {u64 ino; u64 seekoff; u16 reclen; u16 namlen; u8 type; char name[1024]}. */
#define MAX_DIRS 64
static DIR *g_dirs[MAX_DIRS];
static uint32_t g_dirent[MAX_DIRS];
SHIM(_opendir$INODE64$UNIX2003) {
    DIR *d = opendir(CPTR(ARG(0)));
    if (!d) { rt_set_errno(errno); RET(0); return; }
    for (int i = 0; i < MAX_DIRS; i++) {
        if (!g_dirs[i]) {
            g_dirs[i] = d;
            if (!g_dirent[i]) g_dirent[i] = gcalloc(1, 1048);
            RET(0x100 + i);
            return;
        }
    }
    closedir(d);
    rt_set_errno(EMFILE);
    RET(0);
}
SHIM(_readdir$INODE64) {
    int i = (int)ARG(0) - 0x100;
    struct dirent *e = readdir(g_dirs[i]);
    if (!e) { RET(0); return; }
    uint32_t g = g_dirent[i];
    W64_(g_mem, g, e->d_ino);
    W64_(g_mem, g + 8, e->d_seekoff);
    W16_(g_mem, g + 16, e->d_reclen);
    W16_(g_mem, g + 18, e->d_namlen);
    W8_(g_mem, g + 20, e->d_type);
    memcpy(PTR(g + 21), e->d_name, e->d_namlen + 1);
    RET(g);
}
SHIM(_closedir$UNIX2003) {
    int i = (int)ARG(0) - 0x100;
    closedir(g_dirs[i]);
    g_dirs[i] = NULL;
    RET(0);
}

/* ---- process / identity ---- */
SHIM(_inet_addr) { RET(inet_addr(CPTR(ARG(0)))); } /* network byte order, same on both sides */
SHIM(_getpid) { RET(getpid()); }
SHIM(_getppid) { RET(getppid()); }
SHIM(_getuid) { RET(getuid()); }
SHIM(_geteuid) { RET(geteuid()); }
SHIM(_getgid) { RET(getgid()); }
SHIM(_getegid) { RET(getegid()); }
SHIM(_getpgrp) { RET(getpgrp()); }
SHIM(_getlogin) { static uint32_t s; if (!s) s = gstrdup(getlogin() ? getlogin() : "player"); RET(s); }
SHIM(_sysconf) {
    long v = sysconf((int)ARG(0));
    RET(v > INT32_MAX ? INT32_MAX : v);
}
SHIM(_uname) {
    /* struct utsname: 5 x char[256] */
    struct utsname u;
    uname(&u);
    strlcpy(PTR(ARG(0)), u.sysname, 256);
    strlcpy(PTR(ARG(0) + 256), u.nodename, 256);
    strlcpy(PTR(ARG(0) + 512), u.release, 256);
    strlcpy(PTR(ARG(0) + 768), u.version, 256);
    strlcpy(PTR(ARG(0) + 1024), "i386", 256);
    RET(0);
}
SHIM(__NSGetExecutablePath) {
    const char *p = "/civ4/Civilization IV Beyond the Sword";
    uint32_t cap = R32_(g_mem, ARG(1));
    if (cap < strlen(p) + 1) { W32_(g_mem, ARG(1), (uint32_t)strlen(p) + 1); RET(-1); return; }
    strlcpy(PTR(ARG(0)), p, cap);
    RET(0);
}
SHIM(_system$UNIX2003) { RET(-1); }
SHIM(_sigaction) {
    /* Guest signal handlers are not supported; report no previous handler. */
    if (ARG(2)) memset(PTR(ARG(2)), 0, 12);
    RET(0);
}
SHIM(_siginterrupt) { RET(0); }
SHIM(_alarm) { RET(0); }
SHIM(_kill$UNIX2003) { RET(-1); }

/* ---- time ---- */
SHIM(_gettimeofday) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (ARG(0)) { W32_(g_mem, ARG(0), (uint32_t)tv.tv_sec); W32_(g_mem, ARG(0) + 4, (uint32_t)tv.tv_usec); }
    RET(0);
}
SHIM(_tzset) { tzset(); }
SHIM(_times) {
    struct tms t;
    clock_t r = times(&t);
    W32_(g_mem, ARG(0), (uint32_t)t.tms_utime);
    W32_(g_mem, ARG(0) + 4, (uint32_t)t.tms_stime);
    W32_(g_mem, ARG(0) + 8, (uint32_t)t.tms_cutime);
    W32_(g_mem, ARG(0) + 12, (uint32_t)t.tms_cstime);
    RET(r);
}
/* struct tm conversions are in shims_libc.c */
extern void rt_tm_from_guest(struct tm *t, uint32_t g);
extern uint32_t rt_tm_to_guest(const struct tm *t, uint32_t g);
SHIM(_gmtime) {
    static uint32_t gtm;
    if (!gtm) gtm = gcalloc(1, 44);
    time_t tt = (time_t)(int32_t)R32_(g_mem, ARG(0));
    struct tm t;
    gmtime_r(&tt, &t);
    RET(rt_tm_to_guest(&t, gtm));
}
SHIM(_mktime$UNIX2003) {
    struct tm t;
    rt_tm_from_guest(&t, ARG(0));
    time_t r = mktime(&t);
    rt_tm_to_guest(&t, ARG(0));
    RET((uint32_t)r);
}
SHIM(_ctime) {
    static uint32_t buf;
    if (!buf) buf = gcalloc(1, 32);
    time_t tt = (time_t)(int32_t)R32_(g_mem, ARG(0));
    ctime_r(&tt, PTR(buf));
    RET(buf);
}
SHIM(_select$DARWIN_EXTSN) {
    /* Only used as a sleep (no fds) by the guest. */
    if (ARG(4)) usleep(R32_(g_mem, ARG(4)) * 1000000u + R32_(g_mem, ARG(4) + 4));
    RET(0);
}

/* ---- pthread conditions, attributes, misc ---- */
#define COND_LIVE 0x434f4e44u /* 'COND': opaque holds a host pointer */
static pthread_mutex_t g_cond_init = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t *cond_of(uint32_t g) {
    pthread_cond_t *c;
    if (__atomic_load_n((uint32_t *)PTR(g), __ATOMIC_ACQUIRE) == COND_LIVE) {
        memcpy(&c, PTR(g + 4), sizeof c);
        return c;
    }
    pthread_mutex_lock(&g_cond_init);
    if (R32_(g_mem, g) == COND_LIVE) {
        memcpy(&c, PTR(g + 4), sizeof c);
    } else {
        c = malloc(sizeof *c);
        pthread_cond_init(c, NULL);
        memcpy(PTR(g + 4), &c, sizeof c);
        __atomic_store_n((uint32_t *)PTR(g), COND_LIVE, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_cond_init);
    return c;
}
pthread_mutex_t *rt_host_mutex(uint32_t g);
SHIM(_pthread_cond_init$UNIX2003) { W32_(g_mem, ARG(0), 0); cond_of(ARG(0)); RET(0); }
SHIM(_pthread_cond_destroy) { RET(0); }
SHIM(_pthread_cond_signal) { RET(pthread_cond_signal(cond_of(ARG(0)))); }
SHIM(_pthread_cond_broadcast) { RET(pthread_cond_broadcast(cond_of(ARG(0)))); }
SHIM(_pthread_cond_wait$UNIX2003) { RET(pthread_cond_wait(cond_of(ARG(0)), rt_host_mutex(ARG(1)))); }
SHIM(_pthread_attr_init) { memset(PTR(ARG(0)), 0, 40); RET(0); }
SHIM(_pthread_attr_destroy) { RET(0); }
SHIM(_pthread_attr_setscope) { RET(0); }
SHIM(_pthread_attr_setstacksize) { RET(0); }
SHIM(_pthread_detach) { RET(0); }
SHIM(_pthread_self) { RET((uint32_t)(uintptr_t)pthread_self()); }

const ShimDef shims_posix[] = {
    SHIM_ENTRY(_fopen$DARWIN_EXTSN), SHIM_ENTRY(_fdopen$DARWIN_EXTSN), SHIM_ENTRY(_fputs), SHIM_ENTRY(_fwrite),
    SHIM_ENTRY(_chmod$UNIX2003), SHIM_ENTRY(___srget), SHIM_ENTRY(_fgets), SHIM_ENTRY(_clearerr),
    SHIM_ENTRY(_flockfile), SHIM_ENTRY(_funlockfile), SHIM_ENTRY(_fseeko), SHIM_ENTRY(_ftello), SHIM_ENTRY(_putchar),
    SHIM_ENTRY(_perror), SHIM_ENTRY(_tmpfile), SHIM_ENTRY(_vfprintf), SHIM_ENTRY(_strpbrk), SHIM_ENTRY(___strncpy_chk),
    SHIM_ENTRY(_strerror$UNIX2003), SHIM_ENTRY(_memset_pattern16), SHIM_ENTRY(_putenv$UNIX2003),
    SHIM_ENTRY(_unsetenv$UNIX2003), SHIM_ENTRY(_localeconv), SHIM_ENTRY(_nl_langinfo), SHIM_ENTRY(_sqrt),
    SHIM_ENTRY(_fabs), SHIM_ENTRY(_log), SHIM_ENTRY(_log10), SHIM_ENTRY(_log1p), SHIM_ENTRY(_asin), SHIM_ENTRY(_sinh),
    SHIM_ENTRY(_cosh), SHIM_ENTRY(_tanh), SHIM_ENTRY(_asinh), SHIM_ENTRY(_acosh), SHIM_ENTRY(_atanh),
    SHIM_ENTRY(_hypot), SHIM_ENTRY(_copysign), SHIM_ENTRY(_ldexp), SHIM_ENTRY(_frexp), SHIM_ENTRY(_lstat$INODE64),
    SHIM_ENTRY(_getcwd), SHIM_ENTRY(_readlink), SHIM_ENTRY(_realpath$DARWIN_EXTSN), SHIM_ENTRY(_rmdir),
    SHIM_ENTRY(_isatty), SHIM_ENTRY(_dup), SHIM_ENTRY(_dup2), SHIM_ENTRY(_fcntl$UNIX2003), SHIM_ENTRY(_umask),
    SHIM_ENTRY(_utime), SHIM_ENTRY(_fsync$UNIX2003), SHIM_ENTRY(_opendir$INODE64$UNIX2003),
    SHIM_ENTRY(_readdir$INODE64), SHIM_ENTRY(_closedir$UNIX2003), SHIM_ENTRY(_inet_addr), SHIM_ENTRY(_getpid), SHIM_ENTRY(_getppid),
    SHIM_ENTRY(_getuid), SHIM_ENTRY(_geteuid), SHIM_ENTRY(_getgid), SHIM_ENTRY(_getegid), SHIM_ENTRY(_getpgrp),
    SHIM_ENTRY(_getlogin), SHIM_ENTRY(_sysconf), SHIM_ENTRY(_uname), SHIM_ENTRY(__NSGetExecutablePath),
    SHIM_ENTRY(_system$UNIX2003), SHIM_ENTRY(_sigaction), SHIM_ENTRY(_siginterrupt), SHIM_ENTRY(_alarm),
    SHIM_ENTRY(_kill$UNIX2003), SHIM_ENTRY(_gettimeofday), SHIM_ENTRY(_tzset), SHIM_ENTRY(_times),
    SHIM_ENTRY(_gmtime), SHIM_ENTRY(_mktime$UNIX2003), SHIM_ENTRY(_ctime), SHIM_ENTRY(_select$DARWIN_EXTSN),
    SHIM_ENTRY(_pthread_cond_init$UNIX2003), SHIM_ENTRY(_pthread_cond_destroy), SHIM_ENTRY(_pthread_cond_signal),
    SHIM_ENTRY(_pthread_cond_broadcast), SHIM_ENTRY(_pthread_cond_wait$UNIX2003), SHIM_ENTRY(_pthread_attr_init),
    SHIM_ENTRY(_pthread_attr_destroy), SHIM_ENTRY(_pthread_attr_setscope), SHIM_ENTRY(_pthread_attr_setstacksize),
    SHIM_ENTRY(_pthread_detach), SHIM_ENTRY(_pthread_self),
    {0, 0},
};
