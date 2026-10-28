#include "libsa.h"

#include "uapi.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include <sys/mman.h>

#define RECLAIM_CAPACITY (64)

extern int shadow_fd;

static uint64_t stack_start;
static uint64_t stack_end;
static uint64_t stack_size;

// Bitmap used to hold free stack addresses (slots)
// Initially all the bits are zero and the first slot available is given by the counter next.
static _Atomic uint64_t *free_bits;

static atomic_ulong next;

long sa_alloc_shadow_stack(void *stack_base, bool detached) {
    struct ioctl_prepare_params req = {.stack_base = (unsigned long)stack_base,
                                       .detached = (__u8)detached};
    long err = ioctl(shadow_fd, IOCTL_SHADOW_PREPARE, &req);
    if (err < 0) {
        perror("ioctl");
        return -errno;
    }

    return req.error;
}

static inline size_t used_words(void) {
    uintptr_t n = atomic_load(&next);
    if (n > stack_end) {
        n = stack_end;
    }

    return (((n - stack_start) / stack_size) + 63) / 64;
}

void *freelist_pop(void) {
    size_t limit = used_words();
    for (size_t i = 0; i < limit; ++i) {
        uint64_t w = atomic_load_explicit(&free_bits[i], memory_order_relaxed);
        while (w) {
            uint64_t bits = __builtin_ctzll(w);
            uint64_t mask = 1ULL << bits;
            uint64_t old = atomic_fetch_and_explicit(&free_bits[i], ~mask, memory_order_acquire);
            if (old & mask) {
                return (void *)(stack_start + (((i * 64) + bits) * stack_size));
            }
            w = old & ~mask;
        }
    }
    return NULL;
}

void freelist_push(void *stack) {
    size_t index = ((uint64_t)stack - stack_start) / stack_size;
    atomic_fetch_or_explicit(&free_bits[index / 64], 1ULL << (index & 63), memory_order_release);
}

static inline void *bump_alloc(void) {
    unsigned long base = atomic_fetch_add(&next, stack_size);
    if (base >= stack_end) {
        return NULL;
    }

    return (void *)base;
}

void sa_release_user_stack(void *stack) {
    if (mmap(stack, stack_size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
             -1, 0) == MAP_FAILED) {
        // slot is lost because it won't be inserted into the bitmap
        return;
    }
    freelist_push(stack);
}

static void reclaim_dead(void) {
    uint64_t stacks[RECLAIM_CAPACITY];
    struct ioctl_reclaim_dead_params p = {.bases = (uintptr_t)stacks, .cap = RECLAIM_CAPACITY};
    if (!ioctl(shadow_fd, IOCTL_SHADOW_RECLAIM_DEAD, &p) && p.error == 0) {
        for (unsigned i = 0; i < p.count; ++i) {
            sa_release_user_stack((void *)stacks[i]);
        }
    }
}

static void *make_usable(void *stack) {
    if (mprotect(stack, stack_size, PROT_READ | PROT_WRITE)) {
        freelist_push(stack); /* ancora PROT_NONE: l'invariante regge */
        return NULL;
    }
    return stack;
}

void *sa_alloc_user_stack(void) {
    void *s = freelist_pop();
    if (!s) {
        reclaim_dead();
        s = freelist_pop();
    }

    if (!s) {
        s = bump_alloc();
    }
    return s ? make_usable(s) : NULL;
}

long sa_claim(unsigned long stack_base) {
    struct ioctl_stack_params req = {.stack_base = stack_base};
    if (ioctl(shadow_fd, IOCTL_SHADOW_ALLOC, &req)) {
        perror("ioctl");
        return -errno;
    }

    return req.error;
}

long sa_init(unsigned long start, unsigned long end, unsigned long size) {
    if (!start || end <= start || !size || (end - start) % size) {
        return -EINVAL;
    }

    if (mmap((void *)start, end - start, PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1,
             0) == MAP_FAILED) {
        perror("mmap failed");
        return -errno;
    }

    size_t words = (((end - start) / size) + 63) / 64;
    void *bits = mmap(NULL, words * sizeof(uint64_t), PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bits == MAP_FAILED) {
        return -errno;
    }

    free_bits = bits;
    stack_start = start;
    stack_end = end;
    stack_size = size;
    atomic_store(&next, start);
    return 0;
}

long sa_free_kernel_stack(void *stack_base) {
    struct ioctl_stack_params req = {.stack_base = (unsigned long)stack_base};
    long err = ioctl(shadow_fd, IOCTL_SHADOW_UNPREPARE, &req);
    if (err) {
        perror("ioctl");
        return -errno;
    }

    return req.error;
}

long sa_mark_detached(void *stack_base) {
    struct ioctl_stack_params req = {.stack_base = (uintptr_t)stack_base};
    if (ioctl(shadow_fd, IOCTL_SHADOW_DETACH, &req) < 0) {
        return -errno;
    }
    return req.error;
}

long sa_reclaim(void *stack_base) {
    struct ioctl_stack_params req = {.stack_base = (unsigned long)stack_base};
    long err = ioctl(shadow_fd, IOCTL_SHADOW_RECLAIM, &req);
    if (err) {
        perror("ioctl");
        return -errno;
    }

    return req.error;
}

void sa_reset_after_fork(void *current_stack) {
    size_t used = (atomic_load(&next) - stack_start) / stack_size;
    if (used > (stack_end - stack_start) / stack_size) {
        used = (stack_end - stack_start) / stack_size;
    }

    for (size_t w = 0; w < (used + 63) / 64; ++w) {
        atomic_store(&free_bits[w], 0);
    }
    for (size_t i = 0; i < used; ++i) {
        void *s = (void *)(stack_start + (i * stack_size));
        if (s != current_stack) {
            sa_release_user_stack(s);
        }
    }
}
