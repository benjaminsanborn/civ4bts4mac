/* File system shims: BSD calls (including ones the game looks up dynamically
 * through CFBundleGetFunctionPointerForName) and the Carbon File Manager.
 *
 * A guest FSRef (80 opaque bytes) holds {FSREF_TAG, index}; the index selects
 * an absolute host path. */
#include "cf.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void rt_set_errno(int e);

#define FSREF_TAG 0x46535246u /* 'FSRF' */

/* Classic Mac OS error codes the game checks for. */
#define noErr 0
#define fnfErr (-43)
#define eofErr (-39)
#define paramErr (-50)
#define ioErr (-36)
#define dupFNErr (-48)
#define errFSNoMoreItems (-1417)

static pthread_mutex_t g_fs_lock = PTHREAD_MUTEX_INITIALIZER;
static char **g_paths;
static uint32_t g_npaths, g_cap;

static uint32_t path_id(const char *p) {
    pthread_mutex_lock(&g_fs_lock);
    for (uint32_t i = 0; i < g_npaths; i++) {
        if (!strcmp(g_paths[i], p)) {
            pthread_mutex_unlock(&g_fs_lock);
            return i;
        }
    }
    if (g_npaths == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 256;
        g_paths = realloc(g_paths, g_cap * sizeof *g_paths);
    }
    g_paths[g_npaths] = strdup(p);
    uint32_t r = g_npaths++;
    pthread_mutex_unlock(&g_fs_lock);
    return r;
}

static void fsref_set(uint32_t g, const char *path) {
    memset(PTR(g), 0, 80);
    W32_(g_mem, g, FSREF_TAG);
    W32_(g_mem, g + 4, path_id(path));
}

static const char *fsref_path(uint32_t g) {
    if (R32_(g_mem, g) != FSREF_TAG) rt_fatal("bad guest FSRef %08x", g);
    return g_paths[R32_(g_mem, g + 4)];
}

/* ---- BSD (old 32-bit struct stat for the plain `stat` symbol) ---- */
static void stat32_to_guest(const struct stat *s, uint32_t g) {
    memset(PTR(g), 0, 96);
    W32_(g_mem, g + 0, (uint32_t)s->st_dev);
    W32_(g_mem, g + 4, (uint32_t)s->st_ino);
    W16_(g_mem, g + 8, s->st_mode);
    W16_(g_mem, g + 10, s->st_nlink);
    W32_(g_mem, g + 12, s->st_uid);
    W32_(g_mem, g + 16, s->st_gid);
    W32_(g_mem, g + 20, (uint32_t)s->st_rdev);
    W32_(g_mem, g + 24, (uint32_t)s->st_atimespec.tv_sec);
    W32_(g_mem, g + 28, (uint32_t)s->st_atimespec.tv_nsec);
    W32_(g_mem, g + 32, (uint32_t)s->st_mtimespec.tv_sec);
    W32_(g_mem, g + 36, (uint32_t)s->st_mtimespec.tv_nsec);
    W32_(g_mem, g + 40, (uint32_t)s->st_ctimespec.tv_sec);
    W32_(g_mem, g + 44, (uint32_t)s->st_ctimespec.tv_nsec);
    W64_(g_mem, g + 48, (uint64_t)s->st_size);
    W64_(g_mem, g + 56, (uint64_t)s->st_blocks);
    W32_(g_mem, g + 64, (uint32_t)s->st_blksize);
    W32_(g_mem, g + 68, s->st_flags);
    W32_(g_mem, g + 72, s->st_gen);
}

#define ERRNO_RET(expr) do { int r_ = (expr); if (r_ < 0) rt_set_errno(errno); RET(r_); } while (0)
SHIM(_access) { ERRNO_RET(access(CPTR(ARG(0)), (int)ARG(1))); }
SHIM(_chdir) { ERRNO_RET(chdir(CPTR(ARG(0)))); }
SHIM(_chmod) { ERRNO_RET(chmod(CPTR(ARG(0)), (mode_t)ARG(1))); }
SHIM(_mkdir) { ERRNO_RET(mkdir(CPTR(ARG(0)), (mode_t)ARG(1))); }
SHIM(_open) { ERRNO_RET(open(CPTR(ARG(0)), (int)ARG(1), (int)ARG(2))); }
SHIM(_remove) { ERRNO_RET(remove(CPTR(ARG(0)))); }
SHIM(_rename) { ERRNO_RET(rename(CPTR(ARG(0)), CPTR(ARG(1)))); }
SHIM(_unlink) { ERRNO_RET(unlink(CPTR(ARG(0)))); }
SHIM(_stat) {
    struct stat s;
    int r = stat(CPTR(ARG(0)), &s);
    if (r) rt_set_errno(errno); else stat32_to_guest(&s, ARG(1));
    RET(r);
}
/* Plain `fopen` (looked up dynamically) behaves like fopen$UNIX2003. */
extern const ShimDef shims_stdio[];
SHIM(_fopen) {
    for (const ShimDef *d = shims_stdio; d->name; d++)
        if (!strcmp(d->name, "_fopen$UNIX2003")) { d->fn(cpu); return; }
}

/* ---- Folder Manager ---- */
#define kSystemDomain (-32766)
#define kLocalDomain (-32765)
#define kUserDomain (-32763)

static const char *folder_path(int16_t domain, uint32_t type, char *buf, size_t n) {
    const char *home = getenv("HOME");
    int user = domain == kUserDomain || domain == -32767 /* kOnAppropriateDisk */ || domain == 0;
    const char *root = user ? home : (domain == kSystemDomain ? "/System" : "");
    switch (type) {
    case 'fram': snprintf(buf, n, "%s/Library/Frameworks", root); break;
    case 'pref': snprintf(buf, n, "%s/Library/Preferences", root); break;
    case 'asup': snprintf(buf, n, "%s/Library/Application Support", root); break;
    case 'cach': snprintf(buf, n, "%s/Library/Caches", root); break;
    case 'docs': snprintf(buf, n, "%s/Documents", home); break;
    case 'desk': snprintf(buf, n, "%s/Desktop", home); break;
    case 'temp': case 'tmp ': snprintf(buf, n, "%s", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp"); break;
    case 'cusr': snprintf(buf, n, "%s", home); break;
    default: return NULL;
    }
    return buf;
}

SHIM(_FSFindFolder) {
    char buf[1024];
    int16_t domain = (int16_t)ARG(0);
    uint32_t type = ARG(1);
    const char *p = folder_path(domain, type, buf, sizeof buf);
    if (!p) {
        rt_log("FSFindFolder: unknown folder type '%c%c%c%c' domain %d", type >> 24, type >> 16 & 255,
               type >> 8 & 255, type & 255, domain);
        RET(fnfErr);
        return;
    }
    if (ARG(2)) mkdir(p, 0755);
    fsref_set(ARG(3), p);
    RET(noErr);
}
SHIM(_FindFolder) {
    /* (vRefNum, folderType, create, short *foundVRefNum, long *foundDirID): FSSpec-era API. */
    rt_fatal("FindFolder (FSSpec-era) not supported");
}

/* ---- FSRef <-> paths, URLs ---- */
SHIM(_FSRefMakePath) {
    strlcpy(PTR(ARG(1)), fsref_path(ARG(0)), ARG(2));
    RET(noErr);
}
SHIM(_FSPathMakeRef) {
    struct stat s;
    const char *p = CPTR(ARG(0));
    if (stat(p, &s)) { RET(fnfErr); return; }
    char real[PATH_MAX];
    fsref_set(ARG(1), realpath(p, real) ? real : p);
    if (ARG(2)) W8_(g_mem, ARG(2), S_ISDIR(s.st_mode));
    RET(noErr);
}
SHIM(_CFURLCreateFromFSRef) {
    const char *p = fsref_path(ARG(1));
    struct stat s;
    int dir = !stat(p, &s) && S_ISDIR(s.st_mode);
    RETCF_OWNED(CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)p, (CFIndex)strlen(p), dir));
}
SHIM(_CFURLGetFSRef) {
    char p[PATH_MAX];
    struct stat s;
    if (!CFURLGetFileSystemRepresentation(CFI(0), true, (UInt8 *)p, sizeof p) || stat(p, &s)) { RET(0); return; }
    fsref_set(ARG(1), p);
    RET(1);
}
SHIM(_FSCompareFSRefs) { RET(strcmp(fsref_path(ARG(0)), fsref_path(ARG(1))) ? -1 /* diffRefNumErr */ : noErr); }
SHIM(_FSIsAliasFile) {
    W8_(g_mem, ARG(1), 0);
    if (ARG(2)) W8_(g_mem, ARG(2), 0);
    RET(noErr);
}
SHIM(_FSResolveAliasFile) {
    struct stat s;
    stat(fsref_path(ARG(0)), &s);
    if (ARG(2)) W8_(g_mem, ARG(2), S_ISDIR(s.st_mode));
    if (ARG(3)) W8_(g_mem, ARG(3), 0);
    RET(noErr);
}

const ShimDef shims_files[] = {
    SHIM_ENTRY(_access), SHIM_ENTRY(_chdir), SHIM_ENTRY(_chmod), SHIM_ENTRY(_mkdir), SHIM_ENTRY(_open),
    SHIM_ENTRY(_remove), SHIM_ENTRY(_rename), SHIM_ENTRY(_unlink), SHIM_ENTRY(_stat), SHIM_ENTRY(_fopen),
    SHIM_ENTRY(_FSFindFolder), SHIM_ENTRY(_FindFolder), SHIM_ENTRY(_FSRefMakePath), SHIM_ENTRY(_FSPathMakeRef),
    SHIM_ENTRY(_CFURLCreateFromFSRef), SHIM_ENTRY(_CFURLGetFSRef), SHIM_ENTRY(_FSCompareFSRefs),
    SHIM_ENTRY(_FSIsAliasFile), SHIM_ENTRY(_FSResolveAliasFile),
    {0, 0},
};
