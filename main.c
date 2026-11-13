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

#define STACK_START (0x100000000000UL)
#define STACK_END   (0x108000000000UL)
#define STACK_SIZE  (8UL * 1024 * 1024)

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

enum slot_state { DETACHED, JOINING };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    pthread_t thread;
    void *stack;
    enum slot_state state;
} pending[(STACK_END - STACK_START) / STACK_SIZE];
static size_t npending;

static ptrdiff_t find_locked(pthread_t t) {
    for (size_t i = 0; i < npending; ++i) {
        if (pthread_equal(pending[i].thread, t)) {
            return (ptrdiff_t)i;
        }
    }

    return -1;
}

static void add_locked(pthread_t t, void *stack, enum slot_state state) {
    pending[npending].thread = t;
    pending[npending].stack = stack;
    pending[npending].state = state;
    npending++;
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
            sa_release_user_stack(pending[i].stack);
            remove_locked(i);
        } else {
            ++i;
        }
    }
    pthread_mutex_unlock(&lock);
}

// Here we keep track of both the original stack that the protected thread set as the signal
// handler's stack and effective stack that the signal handler of the protected thread will use
// as its stack
static __thread stack_t user_ss = {.ss_flags = SS_DISABLE};
static __thread void *alt_slot;
static pthread_key_t alt_key;

static pthread_t main_thread;

static void after_fork_child(void) {
    npending = 0;
    pthread_mutex_init(&lock, NULL);

    uintptr_t here = (uintptr_t)&here;
    void *cur = (void *)(here & ~(STACK_SIZE - 1));
    if (here < STACK_START || here >= STACK_END) {
        return;
    }

    sa_reset_after_fork(cur, alt_slot);
    if (alt_slot && alt_slot != cur && !sa_refresh_stack(alt_slot)) {
        stack_t off = {.ss_flags = SS_DISABLE};
        real_sigaltstack(&off, NULL);
        sa_release_user_stack(alt_slot);
        alt_slot = NULL;
        user_ss = (stack_t){.ss_flags = SS_DISABLE};
    }
}

static void release_alt_slot(void *slot) {
    sa_release_user_stack(slot);
}

static __attribute__((constructor)) void main_thread_init(void) {
    // Checks that the device is available. Future operations open and close the file descriptor
    // for each ioctl call (libsa.c): a descriptor opened at the beginning might be closed
    // by programs that inheriths their descriptors from their parent (e.g. subprocess and
    // multiprocessing from cpython) or might be assigned to another file. Some programs might
    // set RLIMIT_NOFILE to a lower number than the one we were setting the shadow stack device's
    // descriptor.
    int fd = open("/dev/shdwstck", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("cannot open shadowstack device");
        exit(EXIT_FAILURE);
    }
    close(fd);

    // WARNING: for now I'm reserving STACK_START + STACK_SIZE for the main thread stack, this might
    // be change in the future
    if (sa_init(STACK_START + STACK_SIZE, STACK_END, STACK_SIZE)) {
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
    // lo stack viene spostato tramite una procedura assembly per il momento
    // quindi serve un modo per risalire all'indirizzo
    pthread_atfork(NULL, NULL, after_fork_child);
}

/// Ritorna lo stack associato al thread identificato da pthread_t t solamente
/// se t non è il main thread ed è un thread direttamente gestito da questa libreria
/// cioè lo stack cade nella regionde degli indirizzi gestita dal dispositivo
static inline void *stack_of(pthread_t t) {
    if (pthread_equal(t, main_thread)) {
        return NULL;
    }

    pthread_attr_t attr;
    if (pthread_getattr_np(t, &attr) != 0) {
        return NULL;
    }

    void *base;
    size_t size;
    pthread_attr_getstack(&attr, &base, &size);
    pthread_attr_destroy(&attr);
    if ((uintptr_t)base < STACK_START || (uintptr_t)base >= STACK_END) {
        return NULL;
    }

    return base;
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

    int err = EAGAIN;
    reap_detached();
    void *stack_base = sa_alloc_user_stack();
    if (!stack_base) {
        goto out;
    }

    err = pthread_attr_setstack(&attr, stack_base, STACK_SIZE);
    if (err) {
        sa_release_user_stack(stack_base);
        goto out;
    }

    err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    if (err) {
        sa_release_user_stack(stack_base);
        goto out;
    }

    bool detached = detach_state == PTHREAD_CREATE_DETACHED;
    if (detached) {
        pthread_mutex_lock(&lock);
    }

    err = real_pthread_create(thread, &attr, start_routine, arg);
    if (err) {
        sa_release_user_stack(stack_base);
    } else if (detached) {
        add_locked(*thread, stack_base, DETACHED);
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
    void *base = stack_of(t);
    if (!base) {
        return real_pthread_detach(t);
    }

    pthread_mutex_lock(&lock);
    int err = 0;
    ptrdiff_t i = find_locked(t);
    if (i < 0) {
        add_locked(t, base, DETACHED);
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
        if ((uintptr_t)base < STACK_START || (uintptr_t)base >= STACK_END) {
            pthread_attr_setstack(attr, (void *)STACK_START, STACK_SIZE);
        }
    }
    return 0;
}

typedef int (*join_fn)(pthread_t, void **, void *);

/// Se il thread è detached oppure qualcun altro lo sta gia "joinando", allora il join
/// fallisce. La procedura inserisce il thread nell'array pending in modo che reap_detached
/// concorrentemente con join non causino race.
static int join_reserved(pthread_t t, void **retval, join_fn join, void *ctx) {
    void *base = stack_of(t);
    if (!base) {
        return join(t, retval, ctx); // non è un thread del runtime
    }

    pthread_mutex_lock(&lock);
    ptrdiff_t i = find_locked(t);
    if (i >= 0) {
        // detached, oppure un altro join in corso: glibc restituisce EINVAL in entrambi i casi
        pthread_mutex_unlock(&lock);
        return EINVAL;
    }

    add_locked(t, NULL, JOINING);
    pthread_mutex_unlock(&lock);
    int err = join(t, retval, ctx);
    pthread_mutex_lock(&lock);
    i = find_locked(t);
    if (i >= 0) {
        remove_locked((size_t)i);
    }

    pthread_mutex_unlock(&lock);
    if (!err) {
        sa_release_user_stack(base);
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
    return join_reserved(t, retval, do_join, NULL);
}

int pthread_tryjoin_np(pthread_t t, void **retval) {
    return join_reserved(t, retval, do_tryjoin, NULL);
}

int pthread_timedjoin_np(pthread_t t, void **retval, const struct timespec *abstime) {
    return join_reserved(t, retval, do_timedjoin, (void *)abstime);
}

int pthread_clockjoin_np(pthread_t t, void **retval, clockid_t clock,
                         const struct timespec *abstime) {
    struct clock_args a = {clock, abstime};
    return join_reserved(t, retval, do_clockjoin, &a);
}

int sigaltstack(const stack_t *ss, stack_t *old_ss) {
    stack_t kss;
    stack_t cur;
    void *slot = NULL;
    const stack_t *arg = ss;

    if (ss && !(ss->ss_flags & SS_DISABLE)) {
        slot = sa_alloc_user_stack();
        if (!slot) {
            errno = ENOMEM;
            return -1;
        }
        kss = *ss;
        if (kss.ss_size > STACK_SIZE) {
            kss.ss_size = STACK_SIZE;
        }
        kss.ss_sp = (char *)slot + STACK_SIZE - kss.ss_size;
        arg = &kss;
    }

    if (real_sigaltstack(arg, &cur) != 0) {
        int e = errno;
        if (slot) {
            sa_release_user_stack(slot);
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
            sa_release_user_stack(alt_slot);
        }
        alt_slot = slot;
        user_ss = *ss;
        pthread_setspecific(alt_key, alt_slot);
    }
    return 0;
}
