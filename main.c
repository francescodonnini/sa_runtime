#include "libsa.h"
#include "uapi.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <pthread.h>
#include <signal.h>
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

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    pthread_t thread;
    void *stack;
} pending[(STACK_END - STACK_START) / STACK_SIZE];
static size_t npending;

static bool marked_locked(pthread_t t) {
    for (size_t i = 0; i < npending; ++i) {
        if (pending[i].thread == t) {
            return true;
        }
    }
    return false;
}

static bool is_marked_detached(pthread_t t) {
    pthread_mutex_lock(&lock);
    bool marked = marked_locked(t);
    pthread_mutex_unlock(&lock);
    return marked;
}

static bool mark_detached(pthread_t t, void *stack) {
    pthread_mutex_lock(&lock);
    bool already_detached = marked_locked(t);
    if (!already_detached) {
        pending[npending].thread = t;
        pending[npending].stack = stack;
        npending++;
    }
    pthread_mutex_unlock(&lock);
    return !already_detached;
}

static void reap_detached(void) {
    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < npending;) {
        if (!pthread_tryjoin_np(pending[i].thread, NULL)) {
            sa_release_user_stack(pending[i].stack);
            pending[i] = pending[--npending];
        } else {
            ++i;
        }
    }
    pthread_mutex_unlock(&lock);
}

typedef int (*real_pthread_create_t)(pthread_t *, const pthread_attr_t *, void *(*)(void *),
                                     void *);
typedef int (*real_pthread_detach_t)(pthread_t);
typedef int (*real_pthread_join_t)(pthread_t, void **);
typedef int (*real_sigaltstack_t)(const stack_t *, stack_t *);
typedef int (*real_pthread_getattr_np_t)(pthread_t, pthread_attr_t *);

static real_pthread_create_t real_pthread_create;
static real_pthread_detach_t real_pthread_detach;
static real_pthread_join_t real_pthread_join;
static real_sigaltstack_t real_sigaltstack;
static real_pthread_getattr_np_t real_pthread_getattr_np;

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

    void *sym = dlsym(RTLD_NEXT, "pthread_create");
    memcpy((void *)&real_pthread_create, (void *)&sym, sizeof(sym));
    sym = dlsym(RTLD_NEXT, "pthread_detach");
    memcpy((void *)&real_pthread_detach, (void *)&sym, sizeof(sym));
    sym = dlsym(RTLD_NEXT, "pthread_join");
    memcpy((void *)&real_pthread_join, (void *)&sym, sizeof(sym));
    sym = dlsym(RTLD_NEXT, "sigaltstack");
    memcpy((void *)&real_sigaltstack, (void *)&sym, sizeof(sym));
    pthread_key_create(&alt_key, release_alt_slot);
    main_thread = pthread_self();
    sym = dlsym(RTLD_NEXT, "pthread_getattr_np");
    memcpy((void *)&real_pthread_getattr_np, (void *)&sym, sizeof(sym));
    // lo stack viene spostato tramite una procedura assembly per il momento
    // quindi serve un modo per risalire all'indirizzo
    pthread_atfork(NULL, NULL, after_fork_child);
}

static inline void *stack_of(pthread_t t) {
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

    if (pthread_attr_setstack(&attr, stack_base, STACK_SIZE) ||
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE)) {
        sa_release_user_stack(stack_base);
        err = EINVAL;
        goto out;
    }

    err = real_pthread_create(thread, &attr, start_routine, arg);
    if (err) {
        sa_release_user_stack(stack_base);
    } else if (detach_state == PTHREAD_CREATE_DETACHED) {
        mark_detached(*thread, stack_base);
    }

out:
    if (!user_attr) {
        pthread_attr_destroy(&attr);
    }

    return err;
}

int pthread_detach(pthread_t t) {
    void *base = stack_of(t);
    if (pthread_equal(t, main_thread) || !base) {
        return real_pthread_detach(t);
    }
    return mark_detached(t, base) ? 0 : EINVAL;
}

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

int pthread_join(pthread_t t, void **retval) {
    if (is_marked_detached(t)) {
        return EINVAL;
    }
    void *base = stack_of(t);
    int err = real_pthread_join(t, retval);
    if (!err && base) {
        sa_release_user_stack(base);
    }

    return err;
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
