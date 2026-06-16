#include <ctype.h>
#include <errno.h>
#include <sys/reent.h>
#include <time.h>

static struct _reent ra_reent;
struct _reent *_impure_ptr = &ra_reent;

const char _ctype_[257] = {
    0,
    _C, _C, _C, _C, _C, _C, _C, _C, _C, _C | _S, _C | _S, _C | _S, _C | _S, _C | _S, _C, _C,
    _C, _C, _C, _C, _C, _C, _C, _C, _C, _C, _C, _C, _C, _C, _C, _C,
    _S | _B,
    _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P,
    _N, _N, _N, _N, _N, _N, _N, _N, _N, _N,
    _P, _P, _P, _P, _P, _P, _P,
    _U | _X, _U | _X, _U | _X, _U | _X, _U | _X, _U | _X,
    _U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _U,
    _P, _P, _P, _P, _P, _P,
    _L | _X, _L | _X, _L | _X, _L | _X, _L | _X, _L | _X,
    _L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _L,
    _P, _P, _P, _P, _C
};

char *getenv(const char *name) {
    (void)name;
    return NULL;
}

int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    (void)clk_id;
    if (!tp) {
        errno = EINVAL;
        return -1;
    }
    tp->tv_sec = 0;
    tp->tv_nsec = 0;
    return 0;
}
