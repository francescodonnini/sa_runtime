/* SPDX-License-Identifier: GPL-2.0 */
#ifndef UAPI_SHDWSTCK_H
#define UAPI_SHDWSTCK_H
#include <linux/ioctl.h>
#include <linux/types.h>

#define IOCTL_SHADOW_MAGIC 0xca

enum {
    IOCTL_SHADOW_ALLOC_NO = 0x70,
    IOCTL_SHADOW_FORK_NO,
    IOCTL_SHADOW_DETACH_NO,
    IOCTL_SHADOW_PIVOT_NO,
    IOCTL_SHADOW_PREPARE_NO,
    IOCTL_SHADOW_RECLAIM_NO,
    IOCTL_SHADOW_RECLAIM_DEAD_NO,
    IOCTL_SHADOW_UNPREPARE_NO,
    IOCTL_SHADOW_MAX_NR
};

struct ioctl_stack_params {
    __s64 error;
    __u64 stack_base;
};

struct ioctl_pivot_params {
    __s64 error;
    __u64 new_stack_address;
    __u64 rsp;
    __u64 delta;
};

struct ioctl_prepare_params {
    __s64 error;
    __u64 stack_base;
    __u32 detached;
    __u32 __pad;
};

struct ioctl_reclaim_dead_params {
    __s64 error;
    __u64 bases;
    __u32 cap;
    __u32 count;
};

#define IOCTL_SHADOW_ALLOC                                                                         \
    _IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_ALLOC_NO, struct ioctl_stack_params)
#define IOCTL_SHADOW_DETACH                                                                        \
    _IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_DETACH_NO, struct ioctl_stack_params)
#define IOCTL_SHADOW_FORK _IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_FORK_NO, struct ioctl_stack_params)
#define IOCTL_SHADOW_PIVOT                                                                         \
    _IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_PIVOT_NO, struct ioctl_pivot_params)
#define IOCTL_SHADOW_PREPARE                                                                       \
    _IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_PREPARE_NO, struct ioctl_prepare_params)
#define IOCTL_SHADOW_RECLAIM                                                                       \
    _IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_RECLAIM_NO, struct ioctl_stack_params)
#define IOCTL_SHADOW_RECLAIM_DEAD                                                                  \
    _IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_RECLAIM_DEAD_NO, struct ioctl_reclaim_dead_params)
#define IOCTL_SHADOW_UNPREPARE                                                                     \
    _IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_UNPREPARE_NO, struct ioctl_stack_params)

#endif
