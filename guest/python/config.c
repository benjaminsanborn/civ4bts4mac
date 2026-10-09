/* Builtin module table for the guest CPython (everything is statically linked). */
#include "Python.h"

#define MODULES(X) \
    X(posix) X(errno) X(signal) X(thread) X(gc) X(_sre) X(_codecs) X(zipimport) X(_symtable) \
    X(math) X(time) X(_random) X(operator) X(itertools) X(_struct) X(array) X(binascii) \
    X(cPickle) X(cStringIO) X(_collections) X(_functools) X(_weakref) X(strop) X(datetime) \
    X(_bisect) X(_heapq) X(_md5) X(_sha) X(_sha256) X(_sha512)

#define DECLARE(m) extern void init##m(void);
MODULES(DECLARE)
extern void PyMarshal_Init(void);
extern void initimp(void);

struct _inittab _PyImport_Inittab[] = {
#define ENTRY(m) {#m, init##m},
    MODULES(ENTRY)
    {"marshal", PyMarshal_Init},
    {"imp", initimp},
    {"__main__", NULL},
    {"__builtin__", NULL},
    {"sys", NULL},
    {"exceptions", NULL},
    {0, 0},
};
