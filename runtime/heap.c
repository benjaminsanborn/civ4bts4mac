/* Guest heap: a dlmalloc mspace placed inside the guest arena, so allocations
 * are native-speed but every pointer handed to the guest is a 32-bit guest
 * address. */
#include "shim.h"

#include <string.h>
#include <sys/mman.h>

#define ONLY_MSPACES 1
#define MSPACES 1
#define USE_LOCKS 1
#define HAVE_MMAP 0
#define HAVE_MORECORE 0
#define USE_DL_PREFIX 1
#define MALLOC_ALIGNMENT 16
#define FOOTERS 0
#define DLMALLOC_EXPORT static
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wnull-pointer-arithmetic"
#endif
#include "../third_party/dlmalloc/malloc.c"

#define HEAP_BASE 0x10000000u
#define HEAP_SIZE 0x90000000u /* up to 0xA0000000 */

static mspace g_heap;

int rt_heap_init(void) {
    if (mprotect(g_mem + HEAP_BASE, HEAP_SIZE, PROT_READ | PROT_WRITE)) return -1;
    g_heap = create_mspace_with_base(g_mem + HEAP_BASE, HEAP_SIZE, 1);
    return g_heap ? 0 : -1;
}

uint32_t gmalloc(uint32_t size) { return GADDR(mspace_malloc(g_heap, size ? size : 1)); }
uint32_t gcalloc(uint32_t n, uint32_t size) { return GADDR(mspace_calloc(g_heap, n, size)); }
uint32_t grealloc(uint32_t p, uint32_t size) { return GADDR(mspace_realloc(g_heap, PTR(p), size)); }
uint32_t gmemalign(uint32_t align, uint32_t size) { return GADDR(mspace_memalign(g_heap, align, size)); }
void gfree(uint32_t p) { if (p) mspace_free(g_heap, PTR(p)); }

uint32_t gstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    uint32_t g = gmalloc((uint32_t)n);
    memcpy(PTR(g), s, n);
    return g;
}

size_t rt_heap_usable(uint32_t p) { return p ? mspace_usable_size(PTR(p)) : 0; }
