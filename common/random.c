/*
 * SPDX-License-Identifier: BSD-3-Clause-UC
 *
 * Copyright (c) 2026 H. Peter Anvin <hpa@zytor.com>
 */

#include "config.h"
#include "tftpsubs.h"

#ifndef HAVE_RANDOM
static inline long random(void)
{
    return rand();
}
static inline void srandom(unsigned int seed)
{
    return srand(seed);
}
#endif

static inline uint32_t mulx(uint32_t a, uint32_t b)
{
    uint64_t c = (uint64_t)a * b;
    return (uint32_t)(c >> 32) ^ (uint32_t)c;
}

/*
 * Arbitrary prime numbers in [2^31,2^32) with decent bit balance
 * 0x915d4bcd = 2438810573 = 0b10010001010111010100101111001101
 * 0xa1789f93 = 2709036947 = 0b10100001011110001001111110010011
 * 0xb4cb6f11 = 3033231121 = 0x10110100110010110110111100010001
 */
#define PRIME1 0x915d4bcd
#define PRIME2 0xa1789f93
#define PRIME3 0xb4cb6f11

uint32_t (*random_u32)(void);

static uint32_t random_from_tick(void)
{
#ifdef HAVE_CLOCK_GETTIME
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return mulx(ts.tv_sec, PRIME1) + mulx(ts.tv_nsec, PRIME2);
#else
    return mulx(clock_us() * PRIME1, PRIME2);
#endif
}

static void seed_random(void)
{
    srandom(random_from_tick() ^ mulx(getpid(), PRIME3));
}

#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))

struct cpuid {
    uint32_t eax, ecx, edx, ebx;
};

#define X86_EFLAGS_ID		(1U << 21)
#define X86_CPUID_RDRAND	(1U << 30) /* In cpuid(1,0).ecx */

static inline bool have_cpuid(void)
{
#ifdef __x86_64__
    return true;
#else
    uint32_t x, y;
    asm("pushf; "
        "pushf; "
        "pop %0; "
        "xor %2,%0; "
        "push %0; "
        "popf; "
        "pushf; "
        "pop %1; "
        "popf"
        : "=r" (x), "=r" (y)
        : "g" (X86_EFLAGS_ID));
    return !!((x^y) & (X86_EFLAGS_ID));
#endif
}

static inline struct cpuid cpuid(uint32_t leaf, uint32_t subleaf)
{
    struct cpuid res;

#if defined(__i386__) && defined(__PIC__)
    /* %ebx might be a reserved register for i386 in PIC mode */
    asm(".ifnc '%3','%%ebx'; xchg %%ebx,%3; .endif; "
        "cpuid; "
        ".ifnc '%3','%%ebx'; xchg %%ebx,%3; .endif"
        : "=a" (res.eax), "=c" (res.ecx), "=d" (res.edx), "=r" (res.ebx)
        : "a" (leaf), "c" (subleaf));
#else
    asm("cpuid"
        : "=a" (res.eax), "=c" (res.ecx), "=d" (res.edx), "=b" (res.ebx)
        : "a" (leaf), "c" (subleaf));
#endif

    return res;
}

NOINLINE_FUNC static uint32_t x86_random_u32_tsc(void)
{
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a" (lo), "=d" (hi));
    return (mulx(lo, PRIME1) + mulx(hi, PRIME2)) ^ random();
}

NOINLINE_FUNC static uint32_t x86_random_u32_rdrand(void)
{
    uint32_t v;
    bool ok;
    int cnt;

    for (cnt = 0; cnt < 16; cnt++) {
#ifdef __GCC_ASM_FLAG_OUTPUTS__
        asm volatile("rdrand %0" : "=r" (v), "=@ccnc" (ok) : : "cc");
#else
        asm volatile("rdrand %0; setnc %1" : "=r" (v), "=q" (ok) : : "cc");
#endif
        if (likely(ok))
            return v;
    }

    /* rdrand failed despite multiple tries; assume broken */
    random_u32 = x86_random_u32_tsc;
    return x86_random_u32_tsc();
}

static inline void arch_random_init(void)
{
    if (have_cpuid() && cpuid(0, 0).eax >= 1) {
        struct cpuid leaf1 = cpuid(1, 0);

        if (leaf1.ecx & X86_CPUID_RDRAND)
            random_u32 = x86_random_u32_rdrand;
    }
}

#else

static inline void arch_random_init(void)
{
}

#endif /* GNUC x86 */

static FILE *urandom_fh;

static uint32_t random_u32_tick(void)
{
    return random_from_tick() ^ random();
}

NOINLINE_FUNC static uint32_t random_u32_urandom(void)
{
    uint32_t v = PRIME3;

    if (fread(&v, sizeof v, 1, urandom_fh) == 1)
        return v;

    /* Reading /dev/urandom failed, but might have gotten something... */
    return v + random_u32_tick();
}

static void close_urandom(void)
{
    fclose(urandom_fh);
}

void random_init(void)
{
    seed_random();

    /* Try to open /dev/urandom if it exists */
    urandom_fh = fopen("/dev/urandom", "r");

    if (urandom_fh) {
        setvbuf(urandom_fh, NULL, _IONBF, 0); /* Disable buffering */
        atexit(close_urandom);
        random_u32 = random_u32_urandom;
    } else {
        random_u32 = random_u32_tick;
    }

    arch_random_init();
}
