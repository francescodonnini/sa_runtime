#ifndef SS_RUNTIME_H
#define SS_RUNTIME_H
#include <fcntl.h>
#include <stdbool.h>

#include <sys/ioctl.h>

void *sa_alloc_user_stack(size_t len, int prot);

long sa_init(void);

void sa_main_stack(void **base, size_t *size);

bool sa_owns(const void *p);

bool sa_ready(void);

void sa_release_user_stack(void *base, size_t len);

int sa_stack_prot(void);

void *sa_sys_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);

#endif
