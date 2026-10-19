/*
 * memcpy / memmove / mempcpy for the RXDK-360 clang runtime that are safe for GPU memory.
 *
 * Replaces RXDK's runtime/xbox/memcopy_irql.c (libc.a member xbox_memcopy_irql.o, release be16a94).
 * That version only avoids the VMX copy above PASSIVE_LEVEL. On real hardware it still breaks D3D:
 * d3d9.lib copies into physical / write-combined memory (ring buffer, command buffers) through this
 * memcpy, and picolibc's auto-vectorized copy (lvx/vperm/stvx) on that memory makes CreateDevice take
 * ~1 s and leaves the GPU with corrupt commands: the first Present hangs the console. Verified on an
 * RGH console on 17/10/2026 (x360/tests/d3dmin, hardware_test_v6): plain integer copies fix it.
 * Xenia does not reproduce the problem.
 *
 * Rule: use the vector copy only at PASSIVE_LEVEL and only when both buffers are in the title's
 * cached virtual address space (below 0xA0000000). Physical memory (0xA0000000 and up: the 64 KB,
 * 16 MB and 4 KB page windows, where XPhysicalAlloc / D3D allocations live, cached or not) always
 * gets the integer copy.
 *
 * Must be compiled with -mno-altivec -msoft-float so this file itself contains no VMX/FP instruction
 * (see x360/tools/fix_rxdk_ppc64.py, which builds it and swaps it into libc.a).
 */
#include <stddef.h>
#include <stdint.h>

void *__rxdk_memcpy_vector(void *restrict dst, const void *restrict src, size_t n);
void *__rxdk_memmove_vector(void *dst, const void *src, size_t n);
void *__rxdk_mempcpy_vector(void *restrict dst, const void *restrict src, size_t n);

/* Marker so a linked title can be checked for this implementation (see x360/README.md) */
const char __s3air_memcopy_safe_version[] = "s3air memcopy_safe 1";

#define PHYSICAL_WINDOW_START 0xA0000000u

static inline unsigned current_irql(void)
{
    unsigned irql;
    __asm__ volatile("lbz %0, 0x18(13)" : "=r"(irql));
    return irql;
}

static inline int vector_copy_allowed(const void *dst, const void *src)
{
    return current_irql() == 0
        && (uintptr_t)dst < PHYSICAL_WINDOW_START
        && (uintptr_t)src < PHYSICAL_WINDOW_START;
}

/* Integer copies: aligned doublewords (unrolled x4) when both pointers share their 8-byte
   alignment, aligned words when they share 4-byte alignment, bytes otherwise. */
static void copy_forward(unsigned char *d, const unsigned char *s, size_t n)
{
    const uintptr_t misalign = (uintptr_t)d ^ (uintptr_t)s;
    if ((misalign & 7) == 0) {
        while (n && ((uintptr_t)d & 7)) { *d++ = *s++; n--; }
        for (; n >= 32; n -= 32, d += 32, s += 32) {
            const uint64_t a = ((const uint64_t *)s)[0];
            const uint64_t b = ((const uint64_t *)s)[1];
            const uint64_t c = ((const uint64_t *)s)[2];
            const uint64_t e = ((const uint64_t *)s)[3];
            ((uint64_t *)d)[0] = a;
            ((uint64_t *)d)[1] = b;
            ((uint64_t *)d)[2] = c;
            ((uint64_t *)d)[3] = e;
        }
        for (; n >= 8; n -= 8, d += 8, s += 8)
            *(uint64_t *)d = *(const uint64_t *)s;
    } else if ((misalign & 3) == 0) {
        while (n && ((uintptr_t)d & 3)) { *d++ = *s++; n--; }
        for (; n >= 4; n -= 4, d += 4, s += 4)
            *(uint32_t *)d = *(const uint32_t *)s;
    }
    while (n--) *d++ = *s++;
}

static void copy_backward(unsigned char *d, const unsigned char *s, size_t n)
{
    d += n;
    s += n;
    const uintptr_t misalign = (uintptr_t)d ^ (uintptr_t)s;
    if ((misalign & 7) == 0) {
        while (n && ((uintptr_t)d & 7)) { *--d = *--s; n--; }
        for (; n >= 8; n -= 8) {
            d -= 8;
            s -= 8;
            *(uint64_t *)d = *(const uint64_t *)s;
        }
    } else if ((misalign & 3) == 0) {
        while (n && ((uintptr_t)d & 3)) { *--d = *--s; n--; }
        for (; n >= 4; n -= 4) {
            d -= 4;
            s -= 4;
            *(uint32_t *)d = *(const uint32_t *)s;
        }
    }
    while (n--) *--d = *--s;
}

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    if (vector_copy_allowed(dst, src))
        return __rxdk_memcpy_vector(dst, src, n);
    copy_forward((unsigned char *)dst, (const unsigned char *)src, n);
    return dst;
}

void *mempcpy(void *restrict dst, const void *restrict src, size_t n)
{
    if (vector_copy_allowed(dst, src))
        return __rxdk_mempcpy_vector(dst, src, n);
    copy_forward((unsigned char *)dst, (const unsigned char *)src, n);
    return (unsigned char *)dst + n;
}

void *memmove(void *dst, const void *src, size_t n)
{
    if (vector_copy_allowed(dst, src))
        return __rxdk_memmove_vector(dst, src, n);
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s || d >= s + n)
        copy_forward(d, s, n);
    else
        copy_backward(d, s, n);
    return dst;
}
