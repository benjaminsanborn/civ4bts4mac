/* File system shims: BSD calls (including ones the game looks up dynamically
 * through CFBundleGetFunctionPointerForName) and the Carbon File Manager.
 *
 * A guest FSRef (80 opaque bytes) holds {FSREF_TAG, index}; the index selects
 * an absolute host path. */
#include "cf.h"
#include <CoreText/CoreText.h>
#include "handles.h"

#include <dirent.h>
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

static void remember_dirid(uint32_t id, const char *path);
static const char *dirid_path(uint32_t id);

static void fsref_set(uint32_t g, const char *path) {
    memset(PTR(g), 0, 80);
    W32_(g_mem, g, FSREF_TAG);
    W32_(g_mem, g + 4, path_id(path));
}

const char *rt_fsref_path(uint32_t g);
static const char *fsref_path(uint32_t g) { return rt_fsref_path(g); }
const char *rt_fsref_path(uint32_t g) {
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
    /* kOnAppropriateDisk (-32767) and 0 mean "wherever it lives": the system for
     * frameworks, the user's home for per-user data. */
    int user = domain == kUserDomain || ((domain == -32767 || domain == 0) && type != 'fram');
    const char *root = user ? home : (domain == kSystemDomain || type == 'fram' ? "/System" : "");
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
    /* (vRefNum, folderType, create, short *foundVRefNum, long *foundDirID) */
    char buf[1024];
    const char *p = folder_path((int16_t)ARG(0), ARG(1), buf, sizeof buf);
    struct stat s;
    if (p && ARG(2)) mkdir(p, 0755);
    if (!p || stat(p, &s)) { RET(fnfErr); return; }
    remember_dirid((uint32_t)s.st_ino, p);
    if (ARG(3)) W16_(g_mem, ARG(3), (uint32_t)(int16_t)-100);
    if (ARG(4)) W32_(g_mem, ARG(4), (uint32_t)s.st_ino);
    RET(noErr);
}

/* ---- FSRef <-> paths, URLs ---- */
SHIM(_FSRefMakePath) {
    strlcpy(PTR(ARG(1)), fsref_path(ARG(0)), ARG(2));
    RET(noErr);
}
SHIM(_FSPathMakeRef) {
    struct stat s;
    const char *p = CPTR(ARG(0));
    if (stat(p, &s)) {
        if (getenv("CIV4_DEBUG_FILES")) fprintf(stderr, "[files] FSPathMakeRef miss: %s\n", p);
        RET(fnfErr);
        return;
    }
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

/* Classic File Manager directory IDs are inode numbers; remember their paths. */
static struct { uint32_t id; char *path; } *g_dirids;
static uint32_t g_ndirids;
static void remember_dirid(uint32_t id, const char *path) {
    for (uint32_t i = 0; i < g_ndirids; i++)
        if (g_dirids[i].id == id) return;
    g_dirids = realloc(g_dirids, sizeof *g_dirids * (g_ndirids + 1));
    g_dirids[g_ndirids].id = id;
    g_dirids[g_ndirids++].path = strdup(path);
}
static const char *dirid_path(uint32_t id) {
    if (id == 2) return "/";
    for (uint32_t i = 0; i < g_ndirids; i++)
        if (g_dirids[i].id == id) return g_dirids[i].path;
    return NULL;
}
static uint32_t g_cur_dirid; /* HSetVol */

/* FSCatalogInfo (Carbon, 2-byte packed), 144 bytes. */
static void put_utc(uint32_t g, time_t t) {
    uint64_t secs = (uint64_t)t + 2082844800u; /* since 1904 */
    W16_(g_mem, g, (uint32_t)(secs >> 32));
    W32_(g_mem, g + 2, (uint32_t)secs);
    W16_(g_mem, g + 6, 0);
}
static int catalog_info(const char *path, uint32_t info) {
    struct stat s;
    if (stat(path, &s)) return fnfErr;
    if (!info) return noErr;
    memset(PTR(info), 0, 144);
    W16_(g_mem, info + 0, S_ISDIR(s.st_mode) ? 0x10 : 0);  /* kFSNodeIsDirectoryMask */
    W16_(g_mem, info + 2, (uint32_t)(int16_t)-100);        /* volume refnum */
    char parent[PATH_MAX];
    strlcpy(parent, path, sizeof parent);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) *slash = 0; else strcpy(parent, "/");
    struct stat ps;
    uint32_t parent_id = stat(parent, &ps) ? 2 : (uint32_t)ps.st_ino;
    remember_dirid((uint32_t)s.st_ino, path);
    remember_dirid(parent_id, parent);
    W32_(g_mem, info + 4, parent_id);
    W32_(g_mem, info + 8, (uint32_t)s.st_ino);
    put_utc(info + 16, s.st_birthtimespec.tv_sec);
    put_utc(info + 24, s.st_mtimespec.tv_sec);
    put_utc(info + 32, s.st_ctimespec.tv_sec);
    put_utc(info + 40, s.st_atimespec.tv_sec);
    W32_(g_mem, info + 56, s.st_uid);
    W32_(g_mem, info + 60, s.st_gid);
    W16_(g_mem, info + 66, s.st_mode);
    W64_(g_mem, info + 104, (uint64_t)s.st_size);
    W64_(g_mem, info + 112, (uint64_t)s.st_blocks * 512);
    return noErr;
}
static void hfs_name(uint32_t g, const char *path) {
    /* HFSUniStr255 {UInt16 length; UniChar unicode[255]} */
    const char *base = strrchr(path, '/');
    base = base && base[1] ? base + 1 : path;
    CFStringRef cs = CFStringCreateWithCString(NULL, base, kCFStringEncodingUTF8);
    CFIndex n = CFStringGetLength(cs);
    if (n > 255) n = 255;
    CFStringGetCharacters(cs, CFRangeMake(0, n), PTR(g + 2));
    W16_(g_mem, g, (uint32_t)n);
    CFRelease(cs);
}
SHIM(_FSGetCatalogInfo) {
    /* (ref, whichInfo, catalogInfo*, outName*, fsSpec*, parentRef*) */
    const char *p = fsref_path(ARG(0));
    int r = catalog_info(p, ARG(2));
    if (r) { RET(r); return; }
    if (ARG(3)) hfs_name(ARG(3), p);
    if (ARG(4)) memset(PTR(ARG(4)), 0, 70);
    if (ARG(5)) {
        char parent[PATH_MAX];
        strlcpy(parent, p, sizeof parent);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) *slash = 0; else strcpy(parent, "/");
        fsref_set(ARG(5), parent);
    }
    RET(noErr);
}
SHIM(_GetProcessBundleLocation) {
    /* (psn, FSRef*): the game's .app */
    const char *app = getenv("CIV4_APP");
    if (!app) app = "build/game/Beyond the Sword/Civilization IV Beyond the Sword.app";
    char real[PATH_MAX];
    fsref_set(ARG(1), realpath(app, real) ? real : app);
    RET(noErr);
}

SHIM(_HSetVol) { g_cur_dirid = ARG(2); RET(noErr); }
SHIM(_HGetVol) {
    if (ARG(1)) W16_(g_mem, ARG(1), (uint32_t)(int16_t)-100);
    if (ARG(2)) W32_(g_mem, ARG(2), g_cur_dirid);
    RET(noErr);
}
SHIM(_PBMakeFSRefSync) {
    /* FSRefParam: ioResult 16, ioNamePtr 18, ioVRefNum 22, ioDirID 48, newRef 60 */
    uint32_t pb = ARG(0);
    uint32_t dirid = R32_(g_mem, pb + 48), name = R32_(g_mem, pb + 18);
    char cwd[PATH_MAX];
    const char *dir = dirid ? dirid_path(dirid) : (g_cur_dirid ? dirid_path(g_cur_dirid) : getcwd(cwd, sizeof cwd));
    if (!dir) { W16_(g_mem, pb + 16, (uint32_t)(int16_t)fnfErr); RET(fnfErr); return; }
    char path[PATH_MAX];
    strlcpy(path, dir, sizeof path);
    if (name && R8_(g_mem, name)) {
        uint32_t n = R8_(g_mem, name);
        size_t len = strlen(path);
        if (len && path[len - 1] != '/') strlcat(path, "/", sizeof path);
        len = strlen(path);
        memcpy(path + len, PTR(name + 1), n);
        path[len + n] = 0;
    }
    struct stat st;
    if (stat(path, &st)) { W16_(g_mem, pb + 16, (uint32_t)(int16_t)fnfErr); RET(fnfErr); return; }
    fsref_set(R32_(g_mem, pb + 60), path);
    W16_(g_mem, pb + 16, 0);
    RET(noErr);
}

/* ---- forks: refnums index host fds ---- */
#define MAX_FORKS 256
static int g_fork_fd[MAX_FORKS];

static int fork_fd(uint32_t ref) { return ref > 0 && ref < MAX_FORKS ? g_fork_fd[ref] : -1; }
static int new_fork(int fd) {
    for (int i = 1; i < MAX_FORKS; i++)
        if (!g_fork_fd[i]) { g_fork_fd[i] = fd; return i; }
    close(fd);
    return -1;
}
static off_t seek_mode(int fd, uint32_t mode, int64_t off) {
    switch (mode & 3) {
    case 1: return lseek(fd, off, SEEK_SET);
    case 2: return lseek(fd, off, SEEK_END);
    case 3: return lseek(fd, off, SEEK_CUR);
    default: return lseek(fd, 0, SEEK_CUR);
    }
}

SHIM(_FSOpenFork) {
    /* (ref, forkNameLength, forkName, SInt8 permissions, FSIORefNum *out); data fork only */
    int perm = (int8_t)ARG(3);
    int flags = (perm == 1 /* fsRdPerm */) ? O_RDONLY : (perm == 2 ? O_WRONLY : O_RDWR);
    int fd = open(fsref_path(ARG(0)), flags);
    if (fd < 0 && flags != O_RDONLY) fd = open(fsref_path(ARG(0)), O_RDONLY);
    if (fd < 0) { RET(fnfErr); return; }
    int r = new_fork(fd);
    if (r < 0) { RET(-42 /* tmfoErr */); return; }
    W16_(g_mem, ARG(4), (uint32_t)r);
    RET(noErr);
}
static void rw_fork(CPU *cpu, int write) {
    /* (refnum, posMode, SInt64 offset, count, buffer, ByteCount *actual) */
    int fd = fork_fd(ARG(0));
    if (fd < 0) { RET(-51 /* rfNumErr */); return; }
    seek_mode(fd, ARG(1), (int64_t)ARG64(2));
    ssize_t n = write ? pwrite(fd, PTR(ARG(5)), ARG(4), lseek(fd, 0, SEEK_CUR)) : read(fd, PTR(ARG(5)), ARG(4));
    if (n < 0) n = 0;
    if (write) lseek(fd, n, SEEK_CUR);
    if (ARG(6)) W32_(g_mem, ARG(6), (uint32_t)n);
    RET((uint32_t)n < ARG(4) && !write ? eofErr : noErr);
}
SHIM(_FSReadFork) { rw_fork(cpu, 0); }
SHIM(_FSWriteFork) { rw_fork(cpu, 1); }
SHIM(_FSGetForkPosition) {
    int fd = fork_fd(ARG(0));
    if (fd < 0) { RET(-51); return; }
    W64_(g_mem, ARG(1), (uint64_t)lseek(fd, 0, SEEK_CUR));
    RET(noErr);
}
SHIM(_FSSetForkPosition) {
    int fd = fork_fd(ARG(0));
    if (fd < 0) { RET(-51); return; }
    RET(seek_mode(fd, ARG(1), (int64_t)ARG64(2)) < 0 ? -40 /* posErr */ : noErr);
}
SHIM(_FSGetForkSize) {
    int fd = fork_fd(ARG(0));
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { RET(-51); return; }
    W64_(g_mem, ARG(1), (uint64_t)st.st_size);
    RET(noErr);
}
SHIM(_FSSetForkSize) {
    int fd = fork_fd(ARG(0));
    if (fd < 0) { RET(-51); return; }
    off_t cur = lseek(fd, 0, SEEK_CUR);
    off_t end = seek_mode(fd, ARG(1), (int64_t)ARG64(2));
    RET(ftruncate(fd, end) ? ioErr : noErr);
    lseek(fd, cur < end ? cur : end, SEEK_SET);
}
SHIM(_FSCloseFork) {
    int fd = fork_fd(ARG(0));
    if (fd >= 0) { close(fd); g_fork_fd[ARG(0)] = 0; }
    RET(noErr);
}
SHIM(_FSClose) {
    int fd = fork_fd(ARG(0));
    if (fd >= 0) { close(fd); g_fork_fd[ARG(0)] = 0; }
    RET(noErr);
}
SHIM(_FSFlushFork) { RET(noErr); }
SHIM(_FSGetDataForkName) { W16_(g_mem, ARG(0), 0); RET(noErr); }

/* ---- creating, deleting, renaming, naming ---- */
static void unicode_name(uint32_t chars, uint32_t n, char *out, size_t cap) {
    CFStringRef cs = CFStringCreateWithCharacters(NULL, PTR(chars), (CFIndex)n);
    CFStringGetCString(cs, out, (CFIndex)cap, kCFStringEncodingUTF8);
    CFRelease(cs);
}
static void join(char *out, size_t cap, const char *dir, const char *name) {
    snprintf(out, cap, "%s%s%s", dir, (dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/", name);
}
SHIM(_FSMakeFSRefUnicode) {
    /* (parentRef, nameLength, name, textEncodingHint, newRef) */
    char name[1024], path[PATH_MAX];
    unicode_name(ARG(2), ARG(1), name, sizeof name);
    join(path, sizeof path, fsref_path(ARG(0)), name);
    struct stat st;
    if (stat(path, &st)) { RET(fnfErr); return; }
    fsref_set(ARG(4), path);
    RET(noErr);
}
SHIM(_FSCreateFileUnicode) {
    /* (parentRef, nameLength, name, whichInfo, catalogInfo, newRef, newSpec) */
    char name[1024], path[PATH_MAX];
    unicode_name(ARG(2), ARG(1), name, sizeof name);
    join(path, sizeof path, fsref_path(ARG(0)), name);
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) { RET(errno == EEXIST ? dupFNErr : ioErr); return; }
    close(fd);
    if (ARG(5)) fsref_set(ARG(5), path);
    RET(noErr);
}
SHIM(_FSDeleteObject) {
    const char *p = fsref_path(ARG(0));
    RET((remove(p) == 0) ? noErr : fnfErr);
}
SHIM(_FSRenameUnicode) {
    /* (ref, nameLength, name, textEncodingHint, newRef) */
    char name[1024], dir[PATH_MAX], path[PATH_MAX];
    unicode_name(ARG(2), ARG(1), name, sizeof name);
    strlcpy(dir, fsref_path(ARG(0)), sizeof dir);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    join(path, sizeof path, dir, name);
    if (rename(fsref_path(ARG(0)), path)) { RET(ioErr); return; }
    if (ARG(4)) fsref_set(ARG(4), path);
    RET(noErr);
}
SHIM(_FSSetCatalogInfo) { RET(noErr); }

/* ---- directory iteration ---- */
typedef struct {
    char *dir;
    char **names;
    int n, pos;
} Iter;
SHIM(_FSOpenIterator) {
    /* (container, flags, FSIterator *out) */
    const char *dir = fsref_path(ARG(0));
    DIR *d = opendir(dir);
    if (!d) { RET(fnfErr); return; }
    Iter *it = calloc(1, sizeof *it);
    it->dir = strdup(dir);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        it->names = realloc(it->names, sizeof *it->names * (size_t)(it->n + 1));
        it->names[it->n++] = strdup(e->d_name);
    }
    closedir(d);
    W32_(g_mem, ARG(2), gh_new(0x49544552u /* 'ITER' */, it));
    RET(noErr);
}
SHIM(_FSGetCatalogInfoBulk) {
    /* (iterator, maxObjects, ItemCount *actual, Boolean *changed, whichInfo, catalogInfos, refs, specs, names) */
    Iter *it = gh_get(ARG(0), 0x49544552u);
    if (!it) { RET(paramErr); return; }
    uint32_t k = 0;
    while (k < ARG(1) && it->pos < it->n) {
        char path[PATH_MAX];
        join(path, sizeof path, it->dir, it->names[it->pos++]);
        if (ARG(5)) catalog_info(path, ARG(5) + 144 * k);
        if (ARG(6)) fsref_set(ARG(6) + 80 * k, path);
        if (ARG(8)) hfs_name(ARG(8) + 512 * k, path);
        k++;
    }
    if (ARG(2)) W32_(g_mem, ARG(2), k);
    if (ARG(3)) W8_(g_mem, ARG(3), 0);
    RET(it->pos >= it->n && k == 0 ? errFSNoMoreItems : noErr);
}
SHIM(_FSCloseIterator) {
    Iter *it = gh_get(ARG(0), 0x49544552u);
    if (it) {
        for (int i = 0; i < it->n; i++) free(it->names[i]);
        free(it->names);
        free(it->dir);
        free(it);
        gh_free(ARG(0));
    }
    RET(noErr);
}

/* ---- FSSpec (vRefNum 0, parID 2, Str63 name 6; 70 bytes) ---- */
static int fsspec_path(uint32_t spec, char *out, size_t cap) {
    const char *dir = dirid_path(R32_(g_mem, spec + 2));
    if (!dir) return -1;
    char name[256];
    uint32_t n = R8_(g_mem, spec + 6);
    memcpy(name, PTR(spec + 7), n);
    name[n] = 0;
    if (n) join(out, cap, dir, name); else strlcpy(out, dir, cap);
    return 0;
}
SHIM(_FSMakeFSSpec) {
    /* (vRefNum, dirID, ConstStr255Param name, FSSpec *out) */
    uint32_t name = ARG(2), spec = ARG(3);
    memset(PTR(spec), 0, 70);
    W16_(g_mem, spec, (uint32_t)(int16_t)-100);
    W32_(g_mem, spec + 2, ARG(1) ? ARG(1) : g_cur_dirid);
    uint32_t n = name ? R8_(g_mem, name) : 0;
    if (n > 63) n = 63;
    W8_(g_mem, spec + 6, n);
    if (n) memcpy(PTR(spec + 7), PTR(name + 1), n);
    char path[PATH_MAX];
    struct stat st;
    RET(fsspec_path(spec, path, sizeof path) || stat(path, &st) ? fnfErr : noErr);
}
SHIM(_FSpDelete) {
    char path[PATH_MAX];
    RET(fsspec_path(ARG(0), path, sizeof path) || remove(path) ? fnfErr : noErr);
}
SHIM(_FSpOpenDF) {
    /* (spec, SInt8 permission, short *refNum) */
    char path[PATH_MAX];
    int fd = fsspec_path(ARG(0), path, sizeof path) ? -1 : open(path, (int8_t)ARG(1) == 1 ? O_RDONLY : O_RDWR);
    if (fd < 0) { RET(fnfErr); return; }
    W16_(g_mem, ARG(2), (uint32_t)new_fork(fd));
    RET(noErr);
}
SHIM(_FSpGetFInfo) { memset(PTR(ARG(1)), 0, 16); RET(noErr); }
SHIM(_FSpSetFLock) { RET(noErr); }
SHIM(_FSpRstFLock) { RET(noErr); }

/* FMActivateFonts(const FSSpec *, FMFilter *, void *refCon, OptionBits): make the
 * game's bundled fonts (Sylfaen) available to CoreText for this process. */
SHIM(_FMActivateFonts) {
    char path[PATH_MAX];
    if (!ARG(0) || fsspec_path(ARG(0), path, sizeof path)) { RET(fnfErr); return; }
    if (getenv("CIV4_DEBUG_FONTS")) rt_log("FMActivateFonts %s", path);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CFErrorRef err = NULL;
    if (!CTFontManagerRegisterFontsForURL(url, kCTFontManagerScopeProcess, &err) && err) CFRelease(err);
    CFRelease(url);
    RET(noErr);
}
SHIM(_GetScriptManagerVariable) { RET(0); } /* smRoman / US for every selector */

const ShimDef shims_files[] = {
    SHIM_ENTRY(_FMActivateFonts), SHIM_ENTRY(_GetScriptManagerVariable),
    SHIM_ENTRY(_FSOpenFork), SHIM_ENTRY(_FSReadFork), SHIM_ENTRY(_FSWriteFork), SHIM_ENTRY(_FSGetForkPosition),
    SHIM_ENTRY(_FSSetForkPosition), SHIM_ENTRY(_FSGetForkSize), SHIM_ENTRY(_FSSetForkSize), SHIM_ENTRY(_FSCloseFork),
    SHIM_ENTRY(_FSClose), SHIM_ENTRY(_FSFlushFork), SHIM_ENTRY(_FSGetDataForkName), SHIM_ENTRY(_FSMakeFSRefUnicode),
    SHIM_ENTRY(_FSCreateFileUnicode), SHIM_ENTRY(_FSDeleteObject), SHIM_ENTRY(_FSRenameUnicode),
    SHIM_ENTRY(_FSSetCatalogInfo), SHIM_ENTRY(_FSOpenIterator), SHIM_ENTRY(_FSGetCatalogInfoBulk),
    SHIM_ENTRY(_FSCloseIterator), SHIM_ENTRY(_FSMakeFSSpec), SHIM_ENTRY(_FSpDelete), SHIM_ENTRY(_FSpOpenDF),
    SHIM_ENTRY(_FSpGetFInfo), SHIM_ENTRY(_FSpSetFLock), SHIM_ENTRY(_FSpRstFLock),
    SHIM_ENTRY(_HSetVol), SHIM_ENTRY(_HGetVol), SHIM_ENTRY(_PBMakeFSRefSync),
    SHIM_ENTRY(_FSGetCatalogInfo), SHIM_ENTRY(_GetProcessBundleLocation),
    SHIM_ENTRY(_access), SHIM_ENTRY(_chdir), SHIM_ENTRY(_chmod), SHIM_ENTRY(_mkdir), SHIM_ENTRY(_open),
    SHIM_ENTRY(_remove), SHIM_ENTRY(_rename), SHIM_ENTRY(_unlink), SHIM_ENTRY(_stat), SHIM_ENTRY(_fopen),
    SHIM_ENTRY(_FSFindFolder), SHIM_ENTRY(_FindFolder), SHIM_ENTRY(_FSRefMakePath), SHIM_ENTRY(_FSPathMakeRef),
    SHIM_ENTRY(_CFURLCreateFromFSRef), SHIM_ENTRY(_CFURLGetFSRef), SHIM_ENTRY(_FSCompareFSRefs),
    SHIM_ENTRY(_FSIsAliasFile), SHIM_ENTRY(_FSResolveAliasFile),
    {0, 0},
};
