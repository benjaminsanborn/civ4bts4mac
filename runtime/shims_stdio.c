/* stdio shims. A guest FILE* is the guest address of an 88-byte dummy FILE
 * struct (Darwin i386 sizeof(FILE)); the host FILE* lives in a side table. */
#include "shim.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>

void rt_set_errno(int e);

#define MAX_FILES 1024
#define GUEST_FILE_SIZE 88
static struct { uint32_t g; FILE *h; } g_files[MAX_FILES];

FILE *rt_host_file(uint32_t g);
#define hf rt_host_file
FILE *rt_host_file(uint32_t g) {
    for (int i = 0; i < MAX_FILES; i++)
        if (g_files[i].g == g) return g_files[i].h;
    rt_fatal("unknown guest FILE* %08x", g);
}

static uint32_t wrap(FILE *h) {
    if (!h) return 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!g_files[i].g) {
            g_files[i].g = gcalloc(1, GUEST_FILE_SIZE);
            g_files[i].h = h;
            return g_files[i].g;
        }
    }
    rt_fatal("too many open files");
}

static void unwrap(uint32_t g) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (g_files[i].g == g) {
            gfree(g);
            g_files[i].g = 0;
            g_files[i].h = NULL;
        }
    }
}

extern char **environ;
void rt_stdio_init(void) {
    uint32_t a;
    if ((a = rt_hostdata("___stdinp"))) W32_(g_mem, a, wrap(stdin));
    if ((a = rt_hostdata("___stdoutp"))) W32_(g_mem, a, wrap(stdout));
    if ((a = rt_hostdata("___stderrp"))) W32_(g_mem, a, wrap(stderr));
    /* Guest `environ` (Python's os.environ is built from it; BUG reads HOME).
     * The game's crt1 defines its own copy, which its start code would fill;
     * guest modules link against that one. Fill both. */
    uint32_t game_environ = rt_lookup_symbol("_environ");
    if ((a = rt_hostdata("_environ"))) {
        uint32_t n = 0;
        while (environ[n]) n++;
        uint32_t arr = gmalloc(4 * (n + 1));
        for (uint32_t i = 0; i < n; i++) W32_(g_mem, arr + 4 * i, gstrdup(environ[i]));
        W32_(g_mem, arr + 4 * n, 0);
        W32_(g_mem, a, arr);
        if (game_environ) W32_(g_mem, game_environ, arr);
    }
}

/* Guest paths are used as-is (relative to the working directory). */
SHIM(_fopen$UNIX2003) { FILE *f = fopen(CPTR(ARG(0)), CPTR(ARG(1))); if (!f) rt_set_errno(errno); RET(wrap(f)); }
SHIM(_fdopen$UNIX2003) { RET(wrap(fdopen((int)ARG(0), CPTR(ARG(1))))); }
SHIM(_fclose) {
    /* The game's mmioClose closes the same FILE twice; old libc shrugged that off. */
    for (int i = 0; i < MAX_FILES; i++) {
        if (g_files[i].g == ARG(0)) {
            FILE *f = g_files[i].h;
            unwrap(ARG(0));
            RET(fclose(f));
            return;
        }
    }
    RET(0xffffffffu);
}
SHIM(_fread) { RET(fread(PTR(ARG(0)), ARG(1), ARG(2), hf(ARG(3)))); }
SHIM(_fwrite$UNIX2003) { RET(fwrite(PTR(ARG(0)), ARG(1), ARG(2), hf(ARG(3)))); }
SHIM(_fseek) { RET(fseek(hf(ARG(0)), (long)(int32_t)ARG(1), (int)ARG(2))); }
SHIM(_ftell) { RET((uint32_t)ftell(hf(ARG(0)))); }
SHIM(_rewind) { rewind(hf(ARG(0))); }
SHIM(_fgetpos) { fpos_t p; int r = fgetpos(hf(ARG(0)), &p); W64_(g_mem, ARG(1), (uint64_t)p); RET(r); }
SHIM(_fflush) { RET(fflush(ARG(0) ? hf(ARG(0)) : NULL)); }
SHIM(_feof) { RET(feof(hf(ARG(0)))); }
SHIM(_ferror) { RET(ferror(hf(ARG(0)))); }
SHIM(_fileno) { RET(fileno(hf(ARG(0)))); }
SHIM(_setvbuf) { RET(0); }
SHIM(_fgetc) { RET(fgetc(hf(ARG(0)))); }
SHIM(_getc) { RET(getc(hf(ARG(0)))); }
SHIM(_ungetc) { RET(ungetc((int)ARG(0), hf(ARG(1)))); }
SHIM(_fputc) { RET(fputc((int)ARG(0), hf(ARG(1)))); }
SHIM(_putc) { RET(putc((int)ARG(0), hf(ARG(1)))); }
SHIM(_fputs$UNIX2003) { RET(fputs(CPTR(ARG(0)), hf(ARG(1)))); }
SHIM(_puts) { RET(puts(CPTR(ARG(0)))); }
SHIM(_getwc) { RET(getwc(hf(ARG(0)))); }
SHIM(_ungetwc) { RET(ungetwc((wint_t)ARG(0), hf(ARG(1)))); }
SHIM(_putwc) { RET(putwc((wchar_t)ARG(0), hf(ARG(1)))); }

/* ---- POSIX fds ---- */
SHIM(_read$UNIX2003) { ssize_t r = read((int)ARG(0), PTR(ARG(1)), ARG(2)); if (r < 0) rt_set_errno(errno); RET(r); }
SHIM(_write$UNIX2003) { ssize_t r = write((int)ARG(0), PTR(ARG(1)), ARG(2)); if (r < 0) rt_set_errno(errno); RET(r); }
SHIM(_close$UNIX2003) { RET(close((int)ARG(0))); }
SHIM(_lseek) { RET64(lseek((int)ARG(0), (off_t)(int64_t)ARG64(1), (int)ARG(3))); }
SHIM(_ioctl) { RET(0xffffffffu); }
SHIM(_poll$UNIX2003) { RET(0); }
SHIM(_writev$UNIX2003) {
    /* struct iovec on i386: {void *base; size_t len} = 8 bytes. */
    uint32_t iov = ARG(1), n = ARG(2);
    ssize_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        ssize_t r = write((int)ARG(0), PTR(R32_(g_mem, iov + 8 * i)), R32_(g_mem, iov + 8 * i + 4));
        if (r < 0) { rt_set_errno(errno); RET(-1); return; }
        total += r;
    }
    RET(total);
}

/* struct stat (64-bit inode variant) for Darwin i386, 108 bytes. */
void rt_stat64_to_guest(const struct stat *s, uint32_t g) {
    memset(PTR(g), 0, 108);
    W32_(g_mem, g + 0, (uint32_t)s->st_dev);
    W16_(g_mem, g + 4, s->st_mode);
    W16_(g_mem, g + 6, s->st_nlink);
    W64_(g_mem, g + 8, s->st_ino);
    W32_(g_mem, g + 16, s->st_uid);
    W32_(g_mem, g + 20, s->st_gid);
    W32_(g_mem, g + 24, (uint32_t)s->st_rdev);
    W32_(g_mem, g + 28, (uint32_t)s->st_atimespec.tv_sec);
    W32_(g_mem, g + 32, (uint32_t)s->st_atimespec.tv_nsec);
    W32_(g_mem, g + 36, (uint32_t)s->st_mtimespec.tv_sec);
    W32_(g_mem, g + 40, (uint32_t)s->st_mtimespec.tv_nsec);
    W32_(g_mem, g + 44, (uint32_t)s->st_ctimespec.tv_sec);
    W32_(g_mem, g + 48, (uint32_t)s->st_ctimespec.tv_nsec);
    W32_(g_mem, g + 52, (uint32_t)s->st_birthtimespec.tv_sec);
    W32_(g_mem, g + 56, (uint32_t)s->st_birthtimespec.tv_nsec);
    W64_(g_mem, g + 60, (uint64_t)s->st_size);
    W64_(g_mem, g + 68, (uint64_t)s->st_blocks);
    W32_(g_mem, g + 76, (uint32_t)s->st_blksize);
    W32_(g_mem, g + 80, s->st_flags);
    W32_(g_mem, g + 84, s->st_gen);
}
SHIM(_stat$INODE64) {
    struct stat s;
    int r = stat(CPTR(ARG(0)), &s);
    if (r) rt_set_errno(errno); else rt_stat64_to_guest(&s, ARG(1));
    RET(r);
}
SHIM(_fstat$INODE64) {
    struct stat s;
    int r = fstat((int)ARG(0), &s);
    if (r) rt_set_errno(errno); else rt_stat64_to_guest(&s, ARG(1));
    RET(r);
}

/* ---- printf engine ----
 * Formats with arguments read from guest memory starting at guest address
 * `va` (an i386 va_list is just a pointer into the stack). Output is wide
 * (wchar_t) so one engine serves both families; narrow callers convert. */
#include "format.h"

static void wput(WOut *o, wchar_t c) {
    if (o->len + 1 >= o->cap) {
        o->cap = o->cap ? o->cap * 2 : 256;
        o->buf = realloc(o->buf, o->cap * sizeof(wchar_t));
    }
    o->buf[o->len++] = c;
}

static void wputs_n(WOut *o, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) wput(o, (wchar_t)(unsigned char)s[i]);
}

/* Format characters come from a guest string (narrow or wide) or a host
 * wchar_t array (CF formats); %@ is delegated to the CF layer. */
static const wchar_t *g_host_fmt;
void (*rt_format_object)(WOut *o, uint32_t obj);

static wchar_t fmt_char(uint32_t fmt, int wide, size_t k) {
    if (wide == 2) return g_host_fmt[k];
    return wide ? (wchar_t)R32_(g_mem, fmt + 4 * k) : (wchar_t)R8_(g_mem, fmt + k);
}

static size_t gformat(WOut *o, uint32_t fmt, int wide, uint32_t va) {
    size_t i = 0;
#define FCH(k) fmt_char(fmt, wide, (k))
    for (;;) {
        wchar_t c = FCH(i);
        if (!c) break;
        if (c != '%') { wput(o, c); i++; continue; }
        /* Collect the conversion spec into a narrow host format string. */
        char spec[64];
        size_t sn = 0;
        spec[sn++] = '%';
        i++;
        while (strchr("-+ #0'", (int)FCH(i)) && FCH(i)) spec[sn++] = (char)FCH(i++);
        if (FCH(i) == '*') { sn += snprintf(spec + sn, sizeof spec - sn, "%d", (int)R32_(g_mem, va)); va += 4; i++; }
        while (FCH(i) >= '0' && FCH(i) <= '9') spec[sn++] = (char)FCH(i++);
        if (FCH(i) == '.') {
            spec[sn++] = (char)FCH(i++);
            if (FCH(i) == '*') { sn += snprintf(spec + sn, sizeof spec - sn, "%d", (int)R32_(g_mem, va)); va += 4; i++; }
            while (FCH(i) >= '0' && FCH(i) <= '9') spec[sn++] = (char)FCH(i++);
        }
        int lng = 0, half = 0, ldbl = 0; /* lng: 1 = l, 2 = ll/q */
        for (;;) {
            wchar_t m = FCH(i);
            if (m == 'l') { lng++; i++; }
            else if (m == 'q') { lng = 2; i++; }
            else if (m == 'h') { half++; i++; }
            else if (m == 'L') { ldbl = 1; i++; }
            else if (m == 'z' || m == 'j' || m == 't') { i++; }
            else break;
        }
        spec[sn] = 0;
        wchar_t conv = FCH(i++);
        char tmp[512];
        int n = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            if (conv == 'c' && lng) { /* %lc: a wide character */
                wput(o, (wchar_t)R32_(g_mem, va));
                va += 4;
                break;
            }
            if (lng >= 2) {
                spec[sn++] = 'l'; spec[sn++] = 'l'; spec[sn++] = (char)conv; spec[sn] = 0;
                n = snprintf(tmp, sizeof tmp, spec, (long long)R64_(g_mem, va));
                va += 8;
            } else {
                uint32_t v = R32_(g_mem, va);
                va += 4;
                if (half == 1) v = (conv == 'd' || conv == 'i') ? (uint32_t)(int32_t)(int16_t)v : (v & 0xffff);
                if (half >= 2) v = (conv == 'd' || conv == 'i') ? (uint32_t)(int32_t)(int8_t)v : (v & 0xff);
                spec[sn++] = (char)conv; spec[sn] = 0;
                n = snprintf(tmp, sizeof tmp, spec, v);
            }
            wputs_n(o, tmp, (size_t)n);
            break;
        case 'p':
            spec[sn++] = 'x'; spec[sn] = 0;
            wputs_n(o, "0x", 2);
            n = snprintf(tmp, sizeof tmp, spec, R32_(g_mem, va));
            va += 4;
            wputs_n(o, tmp, (size_t)n);
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d;
            if (ldbl) { d = rt_rf80(g_mem, va); va += 16; }
            else { d = RF64_(g_mem, va); va += 8; }
            spec[sn++] = (char)conv; spec[sn] = 0;
            n = snprintf(tmp, sizeof tmp, spec, d);
            wputs_n(o, tmp, (size_t)(n < (int)sizeof tmp ? n : (int)sizeof tmp - 1));
            break;
        }
        case 's': case 'S': {
            uint32_t s = R32_(g_mem, va);
            va += 4;
            int is_wide = conv == 'S' || lng;
            /* Width/precision for strings: apply via a host format on a narrow copy. */
            int prec = -1, width = 0, left = strchr(spec, '-') != NULL;
            const char *dot = strchr(spec, '.');
            if (dot) prec = atoi(dot + 1);
            for (const char *p = spec + 1; *p && *p != '.'; p++)
                if (*p >= '1' && *p <= '9') { width = atoi(p); break; }
            size_t start = o->len;
            if (!s) { wputs_n(o, "(null)", 6); break; }
            for (size_t k = 0; prec < 0 || (int)k < prec; k++) {
                wchar_t ch = is_wide ? (wchar_t)R32_(g_mem, s + 4 * k) : (wchar_t)R8_(g_mem, s + k);
                if (!ch) break;
                wput(o, ch);
            }
            size_t got = o->len - start;
            if ((int)got < width) {
                size_t pad = (size_t)width - got;
                for (size_t k = 0; k < pad; k++) wput(o, ' ');
                if (!left) {
                    memmove(o->buf + start + pad, o->buf + start, got * sizeof(wchar_t));
                    for (size_t k = 0; k < pad; k++) o->buf[start + k] = ' ';
                }
            }
            break;
        }
        case 'n':
            W32_(g_mem, R32_(g_mem, va), (uint32_t)o->len);
            va += 4;
            break;
        case '%':
            wput(o, '%');
            break;
        case '@':
            if (!rt_format_object) rt_fatal("printf: %%@ outside CF");
            rt_format_object(o, R32_(g_mem, va));
            va += 4;
            break;
        default:
            /* Malformed spec (the game has "options=%6 ..."): print it literally, like libc. */
            wputs_n(o, spec, sn);
            if (conv) wput(o, conv);
            else i--;
            break;
        }
    }
#undef FCH
    return o->len;
}

/* Format a host wide format string with guest varargs; returns a malloc'd
 * wchar_t buffer (caller frees) and its length. */
wchar_t *rt_format_host(const wchar_t *fmt, uint32_t va, size_t *len) {
    WOut o = {0};
    g_host_fmt = fmt;
    gformat(&o, 0, 2, va);
    g_host_fmt = NULL;
    wput(&o, 0);
    *len = o.len - 1;
    return o.buf;
}

void rt_wout_put(WOut *o, wchar_t c) { wput(o, c); }

/* Narrow output: encode wide chars as single bytes (the game's narrow text is Latin-1/ASCII). */
static uint32_t emit_narrow(WOut *o, uint32_t dst, uint32_t cap) {
    if (dst && cap) {
        uint32_t n = o->len < cap - 1 ? (uint32_t)o->len : cap - 1;
        for (uint32_t k = 0; k < n; k++) W8_(g_mem, dst + k, (uint32_t)o->buf[k]);
        W8_(g_mem, dst + n, 0);
    }
    return (uint32_t)o->len;
}

static uint32_t emit_wide(WOut *o, uint32_t dst, uint32_t cap) {
    if (dst && cap) {
        uint32_t n = o->len < cap - 1 ? (uint32_t)o->len : cap - 1;
        for (uint32_t k = 0; k < n; k++) W32_(g_mem, dst + 4 * k, (uint32_t)o->buf[k]);
        W32_(g_mem, dst + 4 * n, 0);
        if (o->len >= cap) return 0xffffffffu; /* swprintf reports truncation as -1 */
    }
    return (uint32_t)o->len;
}

static void write_file(FILE *f, WOut *o) {
    for (size_t k = 0; k < o->len; k++) fputc((int)o->buf[k], f);
}

#define FORMAT(fmt, wide, va) WOut o_ = {0}; gformat(&o_, (fmt), (wide), (va))
#define DONE() free(o_.buf)

SHIM(_sprintf) { FORMAT(ARG(1), 0, ARGADDR(2)); RET(emit_narrow(&o_, ARG(0), 0x7fffffff)); DONE(); }
SHIM(_snprintf) { FORMAT(ARG(2), 0, ARGADDR(3)); RET(emit_narrow(&o_, ARG(0), ARG(1))); DONE(); }
SHIM(_vsprintf) { FORMAT(ARG(1), 0, ARG(2)); RET(emit_narrow(&o_, ARG(0), 0x7fffffff)); DONE(); }
SHIM(_vsnprintf) {
    FORMAT(ARG(2), 0, ARG(3));
    RET(emit_narrow(&o_, ARG(0), ARG(1)));
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("CIV4_DEBUG_FMT") != NULL;
    if (dbg && strstr(CPTR(ARG(2)), "xml")) {
        fprintf(stderr, "[fmt] '%s' va=%08x args %08x %08x -> '%s'\n", CPTR(ARG(2)), ARG(3), R32_(g_mem, ARG(3)),
                R32_(g_mem, ARG(3) + 4), ARG(0) ? CPTR(ARG(0)) : "");
        for (int k = 0; k < 2; k++) {
            uint32_t a = R32_(g_mem, ARG(3) + 4 * k);
            fprintf(stderr, "[fmt]   arg%d bytes:", k);
            for (int b = 0; b < 12; b++) fprintf(stderr, " %02x", R8_(g_mem, a + b));
            fprintf(stderr, "\n");
        }
    }
    DONE();
}
/* __sprintf_chk(dst, flag, dstlen, fmt, ...) */
SHIM(___sprintf_chk) { FORMAT(ARG(3), 0, ARGADDR(4)); RET(emit_narrow(&o_, ARG(0), ARG(2))); DONE(); }
SHIM(___vsprintf_chk) { FORMAT(ARG(3), 0, ARG(4)); RET(emit_narrow(&o_, ARG(0), ARG(2))); DONE(); }
SHIM(_swprintf) { FORMAT(ARG(2), 1, ARGADDR(3)); RET(emit_wide(&o_, ARG(0), ARG(1))); DONE(); }
SHIM(_vswprintf) { FORMAT(ARG(2), 1, ARG(3)); RET(emit_wide(&o_, ARG(0), ARG(1))); DONE(); }
SHIM(_printf) { FORMAT(ARG(0), 0, ARGADDR(1)); write_file(stdout, &o_); RET(o_.len); DONE(); }
SHIM(_fprintf) { FORMAT(ARG(1), 0, ARGADDR(2)); write_file(hf(ARG(0)), &o_); RET(o_.len); DONE(); }

/* ---- scanf: rewrite the format for host sizes, pass translated pointers ---- */
static int gscan(const char *in, const wchar_t *win, uint32_t fmt, int wide, uint32_t va) {
    char hfmt[1024];
    void *ptrs[32];
    int np = 0;
    size_t hn = 0, i = 0;
#define FCH(k) (wide ? (wchar_t)R32_(g_mem, fmt + 4 * (k)) : (wchar_t)R8_(g_mem, fmt + (k)))
    while (FCH(i) && hn < sizeof hfmt - 8) {
        wchar_t c = FCH(i++);
        hfmt[hn++] = (char)c;
        if (c != '%') continue;
        if (FCH(i) == '%') { hfmt[hn++] = (char)FCH(i++); continue; }
        int suppress = 0;
        if (FCH(i) == '*') { suppress = 1; hfmt[hn++] = (char)FCH(i++); }
        while (FCH(i) >= '0' && FCH(i) <= '9') hfmt[hn++] = (char)FCH(i++);
        int lng = 0;
        while (FCH(i) == 'l' || FCH(i) == 'h' || FCH(i) == 'L' || FCH(i) == 'q') {
            wchar_t m = FCH(i++);
            if (m == 'l') lng++;
            else if (m == 'q') lng = 2;
            else hfmt[hn++] = (char)m;
        }
        wchar_t conv = FCH(i++);
        if (strchr("eEfgGaA", (int)conv)) { if (lng) hfmt[hn++] = 'l'; }       /* double stays double */
        else if (strchr("diouxXn", (int)conv)) { if (lng >= 2) { hfmt[hn++] = 'l'; hfmt[hn++] = 'l'; } }
        else if (strchr("sc[", (int)conv)) { if (lng) hfmt[hn++] = 'l'; }
        else if (conv == 'p') { conv = 'x'; }
        hfmt[hn++] = (char)conv;
        if (conv == '[') {
            if (FCH(i) == ']') hfmt[hn++] = (char)FCH(i++);
            while (FCH(i) && FCH(i) != ']') hfmt[hn++] = (char)FCH(i++);
            if (FCH(i)) hfmt[hn++] = (char)FCH(i++);
        }
        if (!suppress) {
            if (np == 32) rt_fatal("scanf: too many conversions");
            ptrs[np++] = PTR(R32_(g_mem, va));
            va += 4;
        }
    }
    hfmt[hn] = 0;
#undef FCH
    for (int k = np; k < 32; k++) ptrs[k] = NULL;
#define P8(k) ptrs[k], ptrs[k + 1], ptrs[k + 2], ptrs[k + 3], ptrs[k + 4], ptrs[k + 5], ptrs[k + 6], ptrs[k + 7]
    if (win) {
        wchar_t wf[1024];
        mbstowcs(wf, hfmt, 1024);
        return swscanf(win, wf, P8(0), P8(8), P8(16), P8(24));
    }
    return sscanf(in, hfmt, P8(0), P8(8), P8(16), P8(24));
#undef P8
}

SHIM(_sscanf) { RET(gscan(CPTR(ARG(0)), NULL, ARG(1), 0, ARGADDR(2))); }
SHIM(_swscanf) { RET(gscan(NULL, (const wchar_t *)PTR(ARG(0)), ARG(1), 1, ARGADDR(2))); }
SHIM(_fscanf) {
    /* Read one line and scan it; adequate for the game's config parsing. */
    char line[4096];
    if (!fgets(line, sizeof line, hf(ARG(0)))) { RET(0xffffffffu); return; }
    RET(gscan(line, NULL, ARG(1), 0, ARGADDR(2)));
}

const ShimDef shims_stdio[] = {
    SHIM_ENTRY(_fopen$UNIX2003), SHIM_ENTRY(_fdopen$UNIX2003), SHIM_ENTRY(_fclose), SHIM_ENTRY(_fread),
    SHIM_ENTRY(_fwrite$UNIX2003), SHIM_ENTRY(_fseek), SHIM_ENTRY(_ftell), SHIM_ENTRY(_rewind),
    SHIM_ENTRY(_fgetpos), SHIM_ENTRY(_fflush), SHIM_ENTRY(_feof), SHIM_ENTRY(_ferror), SHIM_ENTRY(_fileno),
    SHIM_ENTRY(_setvbuf), SHIM_ENTRY(_fgetc), SHIM_ENTRY(_getc), SHIM_ENTRY(_ungetc), SHIM_ENTRY(_fputc),
    SHIM_ENTRY(_putc), SHIM_ENTRY(_fputs$UNIX2003), SHIM_ENTRY(_puts), SHIM_ENTRY(_getwc), SHIM_ENTRY(_ungetwc),
    SHIM_ENTRY(_putwc), SHIM_ENTRY(_read$UNIX2003), SHIM_ENTRY(_write$UNIX2003), SHIM_ENTRY(_close$UNIX2003),
    SHIM_ENTRY(_lseek), SHIM_ENTRY(_ioctl), SHIM_ENTRY(_poll$UNIX2003), SHIM_ENTRY(_writev$UNIX2003),
    SHIM_ENTRY(_stat$INODE64), SHIM_ENTRY(_fstat$INODE64), SHIM_ENTRY(_sprintf), SHIM_ENTRY(_snprintf),
    SHIM_ENTRY(_vsprintf), SHIM_ENTRY(_vsnprintf), SHIM_ENTRY(___sprintf_chk), SHIM_ENTRY(___vsprintf_chk),
    SHIM_ENTRY(_swprintf), SHIM_ENTRY(_vswprintf), SHIM_ENTRY(_printf), SHIM_ENTRY(_fprintf), SHIM_ENTRY(_sscanf),
    SHIM_ENTRY(_swscanf), SHIM_ENTRY(_fscanf),
    {0, 0},
};
