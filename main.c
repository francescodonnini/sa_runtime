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

struct trampoline_args {
    void *(*fn)(void *);
    void *fn_arg;
    void *stack_base;
    size_t stack_size;
};

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

static void release_alt_slot(void *slot) {
    sa_release_user_stack(slot);
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

static void after_fork_child(void) {
    uintptr_t here = (uintptr_t)&here;
    void *cur = (void *)(here & ~(STACK_SIZE - 1));

    if (here < STACK_START || here >= STACK_END) {
        return;
    }

    sa_reset_after_fork(cur, alt_slot);
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

static void *thread_trampoline(void *argp) {
    struct trampoline_args t;
    memcpy(&t, argp, sizeof(t));

    long err = sa_claim((unsigned long)t.stack_base);
    if (err) {
        (void)fprintf(stderr, "shadowstack: claim failed %ld\n", err);
        abort();
    }

    return t.fn(t.fn_arg);
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

int pthread_detach(pthread_t t) {
    void *base = stack_of(t);
    int err = real_pthread_detach(t);
    if (!err && base) {
        sa_mark_detached(base);
    }

    return err;
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

    int err = 0;
    void *stack_base = sa_alloc_user_stack();
    if (!stack_base) {
        err = EAGAIN;
        goto attr_destroy;
    }

    struct trampoline_args *targs = stack_base;
    void *prev_stack = NULL;
    size_t prev_size;
    if (!pthread_attr_getstack(&attr, &prev_stack, &prev_size) && prev_stack) {
        (void)fprintf(stderr, "WARNING: user has already set an existing stack %p:%lu\n",
                      prev_stack, prev_size);
    }

    if (pthread_attr_setstack(&attr, stack_base, STACK_SIZE)) {
        err = EINVAL;
        goto stack_destroy;
    }

    if (sa_alloc_shadow_stack(stack_base, detach_state == PTHREAD_CREATE_DETACHED)) {
        err = EAGAIN;
        goto stack_destroy;
    }

    targs->fn = start_routine;
    targs->fn_arg = arg;
    targs->stack_base = stack_base;
    targs->stack_size = STACK_SIZE;
    err = real_pthread_create(thread, &attr, thread_trampoline, targs);
    if (!err) {
        if (!user_attr) {
            pthread_attr_destroy(&attr);
        }

        return 0;
    }

    sa_free_kernel_stack(stack_base);
stack_destroy:
    sa_release_user_stack(stack_base);
attr_destroy:
    if (!user_attr) {
        pthread_attr_destroy(&attr);
    }

    return err;
}

static void reclaim_after_join(void *stack_base) {
    long err = sa_reclaim(stack_base);
    if (err) {
        return;
    }

    sa_release_user_stack(stack_base);
}

int pthread_join(pthread_t t, void **retval) {
    void *base = stack_of(t);
    int err = real_pthread_join(t, retval);
    if (!err && base) {
        reclaim_after_join(base);
    }

    return err;
}

int pthread_getattr_np(pthread_t t, pthread_attr_t *attr) {
    int err = real_pthread_getattr_np(t, attr);
    if (err) {
        return err;
    }
    void *base;
    size_t size;
    pthread_attr_getstack(attr, &base, &size);
    if ((uintptr_t)base >= STACK_START && (uintptr_t)base < STACK_END) {
        return 0;
    }
    if (t == main_thread) {
        pthread_attr_setstack(attr, (void *)STACK_START, STACK_SIZE);
    }
    return 0;
}
