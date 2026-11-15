#include "uapi.h"

#include <elf.h>
#include <fcntl.h>

#include <sys/mman.h>

#include <asm/unistd.h>

#define STDERR (2)
#define PG     4096UL
#define DN(x)  ((x) & ~(PG - 1))
#define UP(x)  DN((x) + PG - 1)
#define BAD(r) ((unsigned long)(r) >= -4095UL)

/* Range del vecchio stack, letto da entry.S per la munmap */
__attribute__((visibility("hidden"))) unsigned long pt_old_start;
__attribute__((visibility("hidden"))) unsigned long pt_old_len;

static inline long sc(long n, long a, long b, long c, long d, long e, long f) {
    register long r10 asm("r10") = d;
    register long r8 asm("r8") = e;
    register long r9 asm("r9") = f;
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return ret;
}

#define sc1(nr, a1)             sc(nr, a1, 0, 0, 0, 0, 0)
#define sc2(nr, a1, a2)         sc(nr, a1, a2, 0, 0, 0, 0)
#define sc3(nr, a1, a2, a3)     sc(nr, a1, a2, a3, 0, 0, 0)
#define sc4(nr, a1, a2, a3, a4) sc(nr, a1, a2, a3, a4, 0, 0)
#define sc6                     sc

static void die(const char *m, long len) {
    sc3(__NR_write, STDERR, (long)m, len);
    sc1(__NR_exit_group, 127);
    __builtin_unreachable();
}
#define DIE(s) die(s "\n", sizeof(s))

static unsigned long load_ldso(unsigned long *entry) {
    static const char path[] = "/lib64/ld-linux-x86-64.so.2";
    Elf64_Ehdr eh;
    Elf64_Phdr ph[16];

    long fd = sc2(__NR_open, (long)path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        DIE("tramp: open ld.so");
    }
    if (sc4(__NR_pread64, fd, (long)&eh, sizeof eh, 0) != sizeof eh || eh.e_type != ET_DYN ||
        eh.e_phnum > 16) {
        DIE("tramp: bad ld.so");
    }
    long phsz = eh.e_phnum * sizeof(Elf64_Phdr);
    if (sc4(__NR_pread64, fd, (long)ph, phsz, eh.e_phoff) != phsz) {
        DIE("tramp: phdr");
    }

    unsigned long lo = -1UL;
    unsigned long hi = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            if (ph[i].p_vaddr < lo) {
                lo = ph[i].p_vaddr;
            }
            if (ph[i].p_vaddr + ph[i].p_memsz > hi) {
                hi = ph[i].p_vaddr + ph[i].p_memsz;
            }
        }
    }
    lo = DN(lo);
    hi = UP(hi);
    long res = sc6(__NR_mmap, 0, hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (BAD(res)) {
        DIE("tramp: reserve");
    }
    unsigned long base = res - lo;

    for (int i = 0; i < eh.e_phnum; i++) {
        Elf64_Phdr *p = &ph[i];
        if (p->p_type != PT_LOAD) {
            continue;
        }
        int prot = (p->p_flags & PF_R ? PROT_READ : 0) | (p->p_flags & PF_W ? PROT_WRITE : 0) |
                   (p->p_flags & PF_X ? PROT_EXEC : 0);
        unsigned long start = base + DN(p->p_vaddr);
        unsigned long fend = base + p->p_vaddr + p->p_filesz;
        unsigned long mend = base + p->p_vaddr + p->p_memsz;

        if (BAD(sc6(__NR_mmap, start, UP(fend) - start, prot, MAP_PRIVATE | MAP_FIXED, fd,
                    DN(p->p_offset)))) {
            DIE("tramp: map segment");
        }

        if (p->p_memsz > p->p_filesz) { /* .bss */
            for (volatile char *c = (char *)fend; c < (char *)UP(fend); c++) {
                *c = 0; /* coda dell'ultima pagina */
            }
            if (mend > UP(fend) && BAD(sc6(__NR_mmap, UP(fend), UP(mend) - UP(fend), prot,
                                           MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0))) {
                DIE("tramp: map bss");
            }
        }
    }
    sc1(__NR_close, fd);
    *entry = base + eh.e_entry;
    return base;
}

static void set_at_base(unsigned long *sp, unsigned long base) {
    unsigned long *p = sp + 1 + sp[0] + 1; /* envp */
    while (*p) {
        p++;
    }
    for (p++; p[0] != AT_NULL; p += 2) { /* auxv */
        if (p[0] == AT_BASE) {
            p[1] = base;
        }
    }
}

#define STACK_SIZE      0x800000UL
#define ADR_HINT        0x100000000000UL
#define max_ulong(a, b) ((a) > (b) ? (a) : (b))

static inline unsigned long string_end(const char *s) {
    while (*s) {
        s++;
    }
    return (unsigned long)s + 1;
}

static inline int is_stack_aux(unsigned long t) {
    return t == AT_PLATFORM || t == AT_RANDOM || t == AT_EXECFN;
}

static inline void copy(void *d, const void *s, unsigned long n) {
    asm volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}

/// Indirizzo di memoria più alto (inizio dello stack)
/// +----------------------------------------------------+
/// | Strings: argv & envp (null-terminated char arrays) |
/// | ...                                                |
/// | AT_EXECFN, AT_PLATFORM strings                     |
/// +----------------------------------------------------+
/// | Random bytes                                       |
/// +----------------------------------------------------+
/// | ELF Aux Vector (AT_SYSINFO, AT_PAGESZ, ...)        |
/// | (Key-value pairs ending with AT_NULL)              |
/// +----------------------------------------------------+
/// | NULL delimiter (end of envp)                       |
/// +----------------------------------------------------+
/// | envp[N] (pointers to environment strings)          |
/// | ...                                                |
/// | envp[0] (pointers to first env variable)           | rsp+argc+1
/// +----------------------------------------------------+
/// | NULL delimiter (end of argv)                       | rsp+argc
/// +----------------------------------------------------+
/// | argv[argc-1] (pointer to last argument)            | rsp+argc-1
/// | ...                                                |
/// | argv[0]      (pointer to program's name/path)      | rsp+1
/// +----------------------------------------------------+
/// | argc                                               |<--+
/// +----------------------------------------------------+   |
/// Indirizzo di memoria più basso (rsp punta qui) ----------+
static unsigned long *move_stack(unsigned long *sp) {
    unsigned long argc = sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    char **e = envp;
    while (*e) {
        e++;
    }

    Elf64_auxv_t *auxv = (Elf64_auxv_t *)(e + 1);
    unsigned long top = 0;
    for (char **p = argv; *p; p++) {
        top = max_ulong(top, string_end(*p));
    }

    for (char **p = envp; *p; p++) {
        top = max_ulong(top, string_end(*p));
    }

    for (Elf64_auxv_t *a = auxv; a->a_type != AT_NULL; ++a) {
        if (a->a_type == AT_RANDOM) {
            top = max_ulong(top, a->a_un.a_val + 16);
        } else if (is_stack_aux(a->a_type)) {
            top = max_ulong(top, string_end((const char *)a->a_un.a_val));
        }
    }

    unsigned long len = top - (unsigned long)sp;
    if (len > STACK_SIZE / 2) {
        DIE("shadowstack: initial stack too large");
    }

    long fd = sc2(__NR_open, (long)"/dev/shdwstck", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        DIE("shdwstck: cannot open /dev/shdwstck (is the module loaded?)");
    }

    long area = sc6(__NR_mmap, ADR_HINT, STACK_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, 0);
    if (BAD(area)) {
        DIE("shadowstack: cannot map the main thread stack");
    }

    unsigned long *nsp = (unsigned long *)((area + STACK_SIZE - len) & -16UL);
    copy(nsp, sp, len);
    long delta = (long)nsp - (long)sp;
    char **p = (char **)(nsp + 1);
    for (; *p; p++) {
        *p += delta;
    }

    for (p++; *p; p++) {
        *p += delta;
    }

    for (Elf64_auxv_t *a = (Elf64_auxv_t *)(p + 1); a->a_type != AT_NULL; a++) {
        if (is_stack_aux(a->a_type)) {
            a->a_un.a_val += delta;
        }
    }

    struct ioctl_pivot_params pv = {
        .new_stack_address = (unsigned long)nsp,
        .rsp = (unsigned long)sp,
        .delta = 0,
        .error = 0,
    };
    if (sc3(__NR_ioctl, fd, IOCTL_SHADOW_PIVOT, (long)&pv) < 0) {
        DIE("shdwstck: IOCTL_SHADOW_PIVOT failed");
    }

    if (pv.error) {
        DIE("shdwstck: pivot rejected by the module");
    }

    pt_old_start = pv.old_start;
    pt_old_len = pv.old_len;
    sc(__NR_close, fd, 0, 0, 0, 0, 0);
    return nsp;
}

struct jump {
    unsigned long *sp;
    unsigned long entry;
};

struct jump pt_interpreter_main(unsigned long *sp) {
    unsigned long entry;
    unsigned long *nsp = move_stack(sp);
    unsigned long base = load_ldso(&entry);
    set_at_base(nsp, base);
    return (struct jump){nsp, entry};
}
