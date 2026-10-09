/* Guest-aware printf engine shared by stdio and CoreFoundation shims. */
#ifndef CIV4_FORMAT_H
#define CIV4_FORMAT_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef struct {
    wchar_t *buf;
    size_t len, cap;
} WOut;

/* Format `fmt` (host wide string) reading arguments from guest va_list `va`. */
wchar_t *rt_format_host(const wchar_t *fmt, uint32_t va, size_t *len);
void rt_wout_put(WOut *o, wchar_t c);
/* Set by the CF layer: appends the description of a guest CF object (%@). */
extern void (*rt_format_object)(WOut *o, uint32_t obj);

#endif
