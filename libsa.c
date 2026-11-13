#include "libsa.h"

#include "uapi.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/mman.h>

static uint64_t stack_start;
static uint64_t stack_end;
static uint64_t stack_size;

// Bitmap used to hold free stack addresses (slots)
// Initially all the bits are zero and the first slot available is given by the counter next.
static _Atomic uint64_t *free_bits;

static atomic_ulong next;

static int shadow_open(void) {
    return open("/dev/shdwstck", O_RDWR | O_CLOEXEC);
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
    uintptr_t s = (uintptr_t)stack;
    // un indirizzo fuori dall'area, o non allineato a uno slot, è un errore del runtime:
    // freelist_push scriverebbe fuori dalla bitmap, o renderebbe libero lo slot sbagliato
    if (s < stack_start || s >= stack_end || (s - stack_start) % stack_size) {
        (void)fprintf(stderr, "shadowstack: release of invalid slot %p\n", stack);
        abort();
    }
    if (mmap(stack, stack_size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
             -1, 0) == MAP_FAILED) {
        // slot is lost because it won't be inserted into the bitmap
        return;
    }
    freelist_push(stack);
}

static void *make_usable(void *stack) {
    int fd = shadow_open();
    void *p = fd < 0
                  ? MAP_FAILED
                  : mmap(stack, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, 0);
    if (fd >= 0) {
        close(fd);
    }
    if (p == MAP_FAILED) {
        sa_release_user_stack(stack);
        return NULL;
    }
    return stack;
}

void *sa_alloc_user_stack(void) {
    void *s = freelist_pop();
    if (!s) {
        s = bump_alloc();
    }
    return s ? make_usable(s) : NULL;
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

bool sa_ready(void) {
    return free_bits != NULL;
}

void sa_reset_after_fork(void *current_stack, void *keep) {
    size_t used = (atomic_load(&next) - stack_start) / stack_size;
    if (used > (stack_end - stack_start) / stack_size) {
        used = (stack_end - stack_start) / stack_size;
    }

    for (size_t w = 0; w < (used + 63) / 64; ++w) {
        atomic_store(&free_bits[w], 0);
    }
    for (size_t i = 0; i < used; ++i) {
        void *s = (void *)(stack_start + (i * stack_size));
        bool free = atomic_load(&free_bits[i / 64]) & (1ULL << (i & 63));
        if (!free && s != current_stack && s != keep) {
            sa_release_user_stack(s);
        }
    }
}

bool sa_refresh_stack(void *stack) {
    int fd = shadow_open();
    if (fd < 0) {
        return false;
    }
    void *p = mmap(stack, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, 0);
    close(fd);
    return p != MAP_FAILED;
}
