#include "libsa.h"
#include "uapi.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <pthread.h>
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
#define STACK_END   (0x200000000000UL)
#define STACK_SIZE  (8UL * 1024 * 1024)

struct trampoline_args {
    void *(*fn)(void *);
    void *fn_arg;
    void *stack_base;
    size_t stack_size;
};

typedef pid_t (*real_fork_t)(void);
typedef int (*real_pthread_create_t)(pthread_t *, const pthread_attr_t *, void *(*)(void *),
                                     void *);
typedef int (*real_pthread_detach_t)(pthread_t);
typedef int (*real_pthread_join_t)(pthread_t, void **);

static real_pthread_create_t real_pthread_create;
static real_pthread_detach_t real_pthread_detach;
static real_fork_t real_fork;
static real_pthread_join_t real_pthread_join;

int shadow_fd = -1;

static __attribute__((constructor)) void main_thread_init(void) {
    shadow_fd = open("/dev/shdwstck", O_RDWR | O_CLOEXEC);
    if (shadow_fd < 0) {
        perror("cannot open shadowstack device");
        exit(EXIT_FAILURE);
    }

    // WARNING: for now I'm reserving STACK_START + STACK_SIZE for the main thread stack, this might
    // be change in the future
    if (sa_init(STACK_START + STACK_SIZE, STACK_END, STACK_SIZE)) {
        exit(EXIT_FAILURE);
    }

    void *sym = dlsym(RTLD_NEXT, "fork");
    memcpy((void *)&real_fork, (void *)&sym, sizeof(sym));
    sym = dlsym(RTLD_NEXT, "pthread_create");
    memcpy((void *)&real_pthread_create, (void *)&sym, sizeof(sym));
    sym = dlsym(RTLD_NEXT, "pthread_detach");
    memcpy((void *)&real_pthread_detach, (void *)&sym, sizeof(sym));
    sym = dlsym(RTLD_NEXT, "pthread_join");
    memcpy((void *)&real_pthread_join, (void *)&sym, sizeof(sym));
    // lo stack viene spostato tramite una procedura assembly per il momento
    // quindi serve un modo per risalire all'indirizzo
}

static void *thread_trampoline(void *argp) {
    struct trampoline_args t;
    memcpy(&t, argp, sizeof(t));
    free(argp);

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
    struct trampoline_args *targs;
    targs = malloc(sizeof(*targs));
    if (!targs) {
        err = ENOMEM;
        goto attr_destroy;
    }

    void *stack_base = sa_alloc_user_stack();
    if (!stack_base) {
        err = EAGAIN;
        goto targs_destroy;
    }

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
targs_destroy:
    free(targs);
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

pid_t fork(void) {
    int sy_pipe[2];
    if (pipe2(sy_pipe, O_CLOEXEC) < 0) {
        return -1;
    }

    pid_t pid = real_fork();
    if (!pid) {
        close(sy_pipe[0]);

        struct ioctl_stack_params req = {0};
        if (ioctl(shadow_fd, IOCTL_SHADOW_FORK, &req) < 0 || req.error) {
            _exit(127);
        }

        sa_reset_after_fork((void *)req.stack_base);
        char done = '1';
        (void)!write(sy_pipe[1], &done, 1);
        close(sy_pipe[1]);
    } else if (pid > 0) {
        close(sy_pipe[1]);

        char done;
        (void)!read(sy_pipe[0], &done, 1);
        close(sy_pipe[0]);
    } else {
        close(sy_pipe[0]);
        close(sy_pipe[1]);
    }

    return pid;
}
