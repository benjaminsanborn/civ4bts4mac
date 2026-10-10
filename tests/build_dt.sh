#!/bin/sh
# Build the differential-test library: recompiled code + core runtime, with
# weak stand-ins for host overrides (tests never reach them).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
cat > build/dt_stubs.c <<'STUBS'
/* Runtime debugging hooks referenced by rt.c that live in the full runtime. */
#include <stdint.h>
__attribute__((weak)) void rt_heap_check(void) {}
__attribute__((weak)) void rt_redzone_sweep(void) {}
__attribute__((weak)) void rt_hw_watch_at_call(uint64_t call) { (void)call; }
__attribute__((weak)) void plat_pump_busy(void) {}
__attribute__((weak)) void rt_report_watch(void *cpu, int i, uint32_t o, uint32_t n, uint32_t l) {
    (void)cpu; (void)i; (void)o; (void)n; (void)l;
}
STUBS
grep -ho '^OVERRIDE([A-Za-z0-9_$]*)' runtime/*.c | sed 's/OVERRIDE(\(.*\))/__attribute__((weak)) void ov_\1(void *cpu) { (void)cpu; }/' >> build/dt_stubs.c
clang -shared -O2 -flto=thin -w -Iruntime runtime/rt.c tests/dt_native.c build/dt_stubs.c -Wl,-all_load ${LIBGAME:-build/libgame.a} -lc++ -o ${OUT:-build/libdt.dylib}
echo "built ${OUT:-build/libdt.dylib}"
