#include "libsa.h"

#include "uapi.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>

// Lontano dai descrittori bassi, che alcuni programmi si aspettano liberi (es. i test di
// PHP); se RLIMIT_NOFILE è troppo basso, resta dove l'ha messo open.
#define DEV_FD_MIN 512

// Descrittore del dispositivo, aperto in sa_init e riusato: una sandbox (es. Landlock in xz)
// blocca le nuove aperture, ma non l'uso di un descrittore già aperto. Il programma però
// può chiuderlo o riassegnarne il numero a un altro file: prima di ogni uso si controlla
// che sia ancora il dispositivo, e in caso contrario lo si riapre.
static atomic_int dev_fd = -1;
static dev_t dev_rdev;
static struct ioctl_layout layout;
static atomic_bool ready;

void *sa_sys_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    return (void *)syscall(SYS_mmap, addr, len, prot, flags, fd, off);
}

static int open_device(void) {
    int fd = open("/dev/shdwstck", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    int high = fcntl(fd, F_DUPFD_CLOEXEC, DEV_FD_MIN);
    if (high >= 0) {
        close(fd);
        fd = high;
    }

    return fd;
}

static bool is_device(int fd) {
    struct stat st;
    return fd >= 0 && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == dev_rdev;
}

static int shadow_fd(void) {
    int fd = atomic_load(&dev_fd);
    if (is_device(fd)) {
        return fd;
    }

    // chiuso o riassegnato dal programma: il vecchio numero non è più nostro, quindi non
    // va chiuso; se ne apre uno nuovo
    int nfd = open_device();
    if (nfd < 0) {
        return -1;
    }

    if (!atomic_compare_exchange_strong(&dev_fd, &fd, nfd)) {
        close(nfd); // un altro thread l'ha già riaperto
        return atomic_load(&dev_fd);
    }

    return nfd;
}

long sa_init(void) {
    int fd = open_device();
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        perror("cannot open shadowstack device");
        return -errno;
    }

    if (ioctl(fd, IOCTL_SHADOW_GET_LAYOUT, &layout) != 0) {
        int e = errno;
        perror("cannot read the shadowstack layout");
        return -e;
    }

    if (!layout.main_start) {
        (void)fprintf(stderr, "shadowstack: no pivot (not started through the interpreter?)\n");
        return -ENOENT;
    }

    dev_rdev = st.st_rdev;
    atomic_store(&dev_fd, fd);
    atomic_store(&ready, true);
    return 0;
}

bool sa_ready(void) {
    return atomic_load(&ready);
}

bool sa_owns(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return a >= layout.region_start && a < layout.region_end;
}

void sa_main_stack(void **base, size_t *size) {
    *base = (void *)layout.main_start;
    *size = layout.main_size;
}

void *sa_alloc_user_stack(size_t len, int prot) {
    int fd = shadow_fd();
    if (fd < 0 || !len) {
        return NULL;
    }

    void *p = sa_sys_mmap(NULL, len, prot, MAP_PRIVATE, fd, 0);
    return p == MAP_FAILED ? NULL : p;
}

void sa_release_user_stack(void *base, size_t len) {
    if (!sa_owns(base)) {
        (void)fprintf(stderr, "shadowstack: release of %p outside the stack region\n", base);
        abort();
    }
    syscall(SYS_munmap, base, len);
}
