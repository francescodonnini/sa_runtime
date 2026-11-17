#include "libsa.h"
#include "uapi.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/syscall.h>

#include <asm/prctl.h>

typedef int (*real_pthread_clockjoin_np_t)(pthread_t, void **, clockid_t, const struct timespec *);
typedef int (*real_pthread_create_t)(pthread_t *, const pthread_attr_t *, void *(*)(void *),
                                     void *);
typedef int (*real_pthread_detach_t)(pthread_t);
typedef int (*real_pthread_join_t)(pthread_t, void **);
typedef int (*real_pthread_timedjoin_np_t)(pthread_t, void **, const struct timespec *);
typedef int (*real_pthread_tryjoin_np_t)(pthread_t, void **);
typedef int (*real_pthread_getattr_np_t)(pthread_t, pthread_attr_t *);
typedef int (*real_sigaltstack_t)(const stack_t *, stack_t *);

static real_pthread_clockjoin_np_t real_pthread_clockjoin_np;
static real_pthread_create_t real_pthread_create;
static real_pthread_detach_t real_pthread_detach;
static real_pthread_getattr_np_t real_pthread_getattr_np;
static real_pthread_join_t real_pthread_join;
static real_pthread_timedjoin_np_t real_pthread_timedjoin_np;
static real_pthread_tryjoin_np_t real_pthread_tryjoin_np;
static real_sigaltstack_t real_sigaltstack;

/// Le richieste di uno stack, mmap(MAP_STACK) senza indirizzo (per esempio le fibre di PHP),
/// vengono servite dal dispositivo. Lo stack è la mappatura restituita: la munmap del
/// programma lo libera senza che ci sia bisogno di intercettarla.
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    if ((flags & MAP_STACK) && (flags & MAP_ANONYMOUS) && !(flags & MAP_FIXED) && !addr && len &&
        sa_ready()) {
        void *p = sa_alloc_user_stack(len, prot);
        if (p) {
            return p;
        }
        // dispositivo non disponibile: lo stack resta fuori dalla regione, e il codice
        // strumentato che ci gira si fermerà al primo prologo
    }
    return sa_sys_mmap(addr, len, prot, flags, fd, off);
}

void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    return mmap(addr, len, prot, flags, fd, off);
}

enum slot_state { DETACHED, JOINING };

struct pending_entry {
    pthread_t thread;
    void *stack;
    size_t size;
    enum slot_state state;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct pending_entry *pending;
static size_t npending;
static size_t cap;

static ptrdiff_t find_locked(pthread_t t) {
    for (size_t i = 0; i < npending; ++i) {
        if (pthread_equal(pending[i].thread, t)) {
            return (ptrdiff_t)i;
        }
    }

    return -1;
}

static bool add_locked(pthread_t t, void *stack, size_t size, enum slot_state state) {
    if (npending == cap) {
        size_t ncap = cap ? cap * 2 : 64;
        struct pending_entry *n = realloc(pending, ncap * sizeof(*n));
        if (!n) {
            return false;
        }
        pending = n;
        cap = ncap;
    }

    pending[npending++] = (struct pending_entry){t, stack, size, state};
    return true;
}

static bool is_marked_detached(pthread_t t) {
    pthread_mutex_lock(&lock);
    ptrdiff_t i = find_locked(t);
    bool detached = i >= 0 && pending[i].state == DETACHED;
    pthread_mutex_unlock(&lock);
    return detached;
}

static void remove_locked(size_t i) {
    pending[i] = pending[--npending];
}

static void reap_detached(void) {
    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < npending;) {
        if (pending[i].state == DETACHED && !real_pthread_tryjoin_np(pending[i].thread, NULL)) {
            sa_release_user_stack(pending[i].stack, pending[i].size);
            remove_locked(i);
        } else {
            ++i;
        }
    }
    pthread_mutex_unlock(&lock);
}

// Lo stack alternativo che il programma crede di aver registrato (user_ss) e quello servito
// dal dispositivo che il kernel usa davvero (alt_slot, alt_size)
static __thread stack_t user_ss = {.ss_flags = SS_DISABLE};
static __thread void *alt_slot;
static __thread size_t alt_size;
static pthread_key_t alt_key;
static pthread_t main_thread;

static void after_fork_child(void) {
    npending = 0;
    pthread_mutex_init(&lock, NULL);
    if (!alt_slot) {
        return;
    }

    void *ns = sa_alloc_user_stack(alt_size, PROT_READ | PROT_WRITE);
    if (!ns) {
        return;
    }

    stack_t kss = {.ss_sp = ns, .ss_size = alt_size, .ss_flags = 0};
    if (real_sigaltstack(&kss, NULL) != 0) {
        // es. fork chiamata da un gestore che gira sullo stack alternativo: si tiene il vecchio
        sa_release_user_stack(ns, alt_size);
        return;
    }

    sa_release_user_stack(alt_slot, alt_size);
    alt_slot = ns;
    pthread_setspecific(alt_key, alt_slot);
}

static void release_alt_slot(void *slot) {
    sa_release_user_stack(slot, alt_size);
}

static __attribute__((constructor)) void main_thread_init(void) {
    if (sa_init()) {
        exit(EXIT_FAILURE);
    }

    void *sym = dlsym(RTLD_NEXT, "pthread_clockjoin_np");
    memcpy((void *)&real_pthread_clockjoin_np, (void *)&sym, sizeof(sym));
    sym = dlsym(RTLD_NEXT, "pthread_create");
    memcpy((void *)&real_pthread_create, (void *)&sym, sizeof(sym));
    sym = dlsym(RTLD_NEXT, "pthread_detach");
    memcpy((void *)&real_pthread_detach, (void *)&sym, sizeof(sym));
    sym = dlsym(RTLD_NEXT, "pthread_getattr_np");
    memcpy((void *)&real_pthread_getattr_np, (void *)&sym, sizeof(sym));
    sym = dlsym(RTLD_NEXT, "pthread_join");
    memcpy((void *)&real_pthread_join, (void *)&sym, sizeof(sym));
    sym = dlsym(RTLD_NEXT, "pthread_timedjoin_np");
    memcpy((void *)&real_pthread_timedjoin_np, (void *)&sym, sizeof(sym));
    sym = dlsym(RTLD_NEXT, "pthread_tryjoin_np");
    memcpy((void *)&real_pthread_tryjoin_np, (void *)&sym, sizeof(sym));
    sym = dlsym(RTLD_NEXT, "sigaltstack");
    memcpy((void *)&real_sigaltstack, (void *)&sym, sizeof(sym));
    pthread_key_create(&alt_key, release_alt_slot);
    main_thread = pthread_self();
    pthread_atfork(NULL, NULL, after_fork_child);
}

/// Lo stack del thread @t, solo se non è il main thread ed è servito dal dispositivo
static bool stack_of(pthread_t t, void **base, size_t *size) {
    if (pthread_equal(t, main_thread)) {
        return false;
    }

    pthread_attr_t attr;
    if (pthread_getattr_np(t, &attr) != 0) {
        return false;
    }

    pthread_attr_getstack(&attr, base, size);
    pthread_attr_destroy(&attr);
    return sa_owns(*base);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *user_attr,
                   void *(*start_routine)(void *), void *arg) {
    pthread_attr_t attr;
    int detach_state = PTHREAD_CREATE_JOINABLE;
    if (user_attr) {
        pthread_attr_getdetachstate(user_attr, &detach_state);
        memcpy(&attr, user_attr, sizeof(attr));
    } else {
        pthread_attr_init(&attr);
    }

    reap_detached();
    size_t size;
    pthread_attr_getstacksize(&attr, &size);
    int err = EAGAIN;
    void *stack_base = sa_alloc_user_stack(size, sa_stack_prot());
    if (!stack_base) {
        goto out;
    }

    err = pthread_attr_setstack(&attr, stack_base, size);
    if (!err) {
        err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    }
    if (err) {
        sa_release_user_stack(stack_base, size);
        goto out;
    }

    bool detached = detach_state == PTHREAD_CREATE_DETACHED;
    if (detached) {
        pthread_mutex_lock(&lock);
    }

    err = real_pthread_create(thread, &attr, start_routine, arg);
    if (err) {
        sa_release_user_stack(stack_base, size);
    } else if (detached) {
        (void)add_locked(*thread, stack_base, size, DETACHED);
    }

    if (detached) {
        pthread_mutex_unlock(&lock);
    }

out:
    if (!user_attr) {
        pthread_attr_destroy(&attr);
    }

    return err;
}

/// Un thread gestito dal dispositivo non può essere effettivamente marcato
/// come detached ma ci si limita a segnare nell'array che il programma l'ha
/// segnato come tale. Se invece il thread non è direttamente gestito dal dispositivo
/// si chiama la vera pthread_detach
int pthread_detach(pthread_t t) {
    void *base;
    size_t size;
    if (!stack_of(t, &base, &size)) {
        return real_pthread_detach(t);
    }

    pthread_mutex_lock(&lock);
    int err = 0;
    ptrdiff_t i = find_locked(t);
    if (i < 0) {
        (void)add_locked(t, base, size, DETACHED);
    } else if (pending[i].state == DETACHED) {
        err = EINVAL; // già detached
    }

    pthread_mutex_unlock(&lock);
    return err;
}

/// Tutti i thread sono joinable, quindi è fondamentale mascherare questa caratteristica
/// al programma; se quest'ultimo ha creato un thread detached non può leggere che quel
/// thread è joinable.
int pthread_getattr_np(pthread_t t, pthread_attr_t *attr) {
    int err = real_pthread_getattr_np(t, attr);
    if (err) {
        return err;
    }

    if (is_marked_detached(t)) {
        pthread_attr_setdetachstate(attr, PTHREAD_CREATE_DETACHED);
    }

    if (pthread_equal(t, main_thread)) {
        void *base;
        size_t size;
        pthread_attr_getstack(attr, &base, &size);
        if (!sa_owns(base)) {
            sa_main_stack(&base, &size);
            pthread_attr_setstack(attr, base, size);
        }
    }

    return 0;
}

typedef int (*join_fn)(pthread_t, void **, void *);

/// Se il thread è detached oppure qualcun altro lo sta gia "joinando", allora il join
/// fallisce. La procedura inserisce il thread nell'array pending in modo che reap_detached
/// concorrentemente con join non causino race.
static int join_reserved(pthread_t t, void **retval, join_fn join, void *ctx, int busy_err) {
    void *base;
    size_t size;
    if (!stack_of(t, &base, &size)) {
        return join(t, retval, ctx); // non è un thread del runtime
    }

    pthread_mutex_lock(&lock);
    if (find_locked(t) >= 0) {
        // detached, oppure un altro join in corso: glibc restituisce EINVAL in entrambi i casi
        pthread_mutex_unlock(&lock);
        return busy_err;
    }

    if (!add_locked(t, NULL, 0, JOINING)) {
        pthread_mutex_unlock(&lock);
        return EAGAIN;
    }

    pthread_mutex_unlock(&lock);
    int err = join(t, retval, ctx);
    pthread_mutex_lock(&lock);
    ptrdiff_t i = find_locked(t);
    if (i >= 0) {
        remove_locked((size_t)i);
    }

    pthread_mutex_unlock(&lock);
    if (!err) {
        sa_release_user_stack(base, size);
    }

    return err;
}

static int do_join(pthread_t t, void **r, void *ctx) {
    (void)ctx;
    return real_pthread_join(t, r);
}

static int do_tryjoin(pthread_t t, void **r, void *ctx) {
    (void)ctx;
    return real_pthread_tryjoin_np(t, r);
}

static int do_timedjoin(pthread_t t, void **r, void *ctx) {
    return real_pthread_timedjoin_np(t, r, ctx);
}

struct clock_args {
    clockid_t clock;
    const struct timespec *abstime;
};

static int do_clockjoin(pthread_t t, void **r, void *ctx) {
    struct clock_args *a = ctx;
    return real_pthread_clockjoin_np(t, r, a->clock, a->abstime);
}

int pthread_join(pthread_t t, void **retval) {
    return join_reserved(t, retval, do_join, NULL, EINVAL);
}

int pthread_tryjoin_np(pthread_t t, void **retval) {
    return join_reserved(t, retval, do_tryjoin, NULL, EBUSY);
}

int pthread_timedjoin_np(pthread_t t, void **retval, const struct timespec *abstime) {
    return join_reserved(t, retval, do_timedjoin, (void *)abstime, EINVAL);
}

int pthread_clockjoin_np(pthread_t t, void **retval, clockid_t clock,
                         const struct timespec *abstime) {
    struct clock_args a = {clock, abstime};
    return join_reserved(t, retval, do_clockjoin, &a, EINVAL);
}

int sigaltstack(const stack_t *ss, stack_t *old_ss) {
    stack_t kss;
    stack_t cur;
    void *slot = NULL;
    size_t size = 0;
    const stack_t *arg = ss;

    if (ss && !(ss->ss_flags & SS_DISABLE)) {
        size = (ss->ss_size + 4095) & ~(size_t)4095;
        slot = sa_alloc_user_stack(size, sa_stack_prot());
        if (!slot) {
            errno = ENOMEM;
            return -1;
        }

        kss = *ss;
        kss.ss_sp = slot;
        kss.ss_size = size;
        arg = &kss;
    }

    if (real_sigaltstack(arg, &cur) != 0) {
        int e = errno;
        if (slot) {
            sa_release_user_stack(slot, size);
        }
        errno = e;
        return -1;
    }

    if (old_ss) {
        *old_ss = cur;
        if (alt_slot && !(cur.ss_flags & SS_DISABLE)) {
            old_ss->ss_sp = user_ss.ss_sp;
            old_ss->ss_size = user_ss.ss_size;
        }
    }

    if (ss) {
        if (alt_slot) {
            sa_release_user_stack(alt_slot, alt_size);
        }

        alt_slot = slot;
        alt_size = size;
        user_ss = *ss;
        pthread_setspecific(alt_key, alt_slot);
    }

    return 0;
}
