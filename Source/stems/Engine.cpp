// The generic (portable) network engine and the CPU feature check used to
// pick an engine at run time. Compiled with the target's baseline flags.

#include "dsp.hpp"
#include "model.hpp"

#include <cstddef>
#include <string>

#define FLAYR_STEMS_ENGINE_NS generic
#define FLAYR_STEMS_ENGINE_NAME "generic"
#include "stems/EngineImpl.inc"

#if defined(_MSC_VER) && !defined(__clang__) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#include <intrin.h>
#define FLAYR_STEMS_X86_MSVC 1
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#define FLAYR_STEMS_X86_GNU 1
#endif

namespace chop::stems::detail {

const EngineOps& genericEngine()
{
    return generic::ops();
}

bool cpuSupportsAvx2Fma()
{
#if defined(FLAYR_STEMS_X86_MSVC) || defined(FLAYR_STEMS_X86_GNU)
    unsigned int r1[4] = {0, 0, 0, 0}; // eax ebx ecx edx of leaf 1
    unsigned int r7[4] = {0, 0, 0, 0}; // leaf 7, subleaf 0
    unsigned int maxLeaf = 0;
#if defined(FLAYR_STEMS_X86_MSVC)
    int info[4];
    __cpuid(info, 0);
    maxLeaf = (unsigned int)info[0];
    if (maxLeaf < 7)
        return false;
    __cpuid(info, 1);
    for (int i = 0; i < 4; ++i)
        r1[i] = (unsigned int)info[i];
    __cpuidex(info, 7, 0);
    for (int i = 0; i < 4; ++i)
        r7[i] = (unsigned int)info[i];
#else
    maxLeaf = __get_cpuid_max(0, nullptr);
    if (maxLeaf < 7)
        return false;
    __cpuid(1, r1[0], r1[1], r1[2], r1[3]);
    __cpuid_count(7, 0, r7[0], r7[1], r7[2], r7[3]);
#endif
    const bool fma = (r1[2] >> 12) & 1u;
    const bool osxsave = (r1[2] >> 27) & 1u;
    const bool avx = (r1[2] >> 28) & 1u;
    const bool avx2 = (r7[1] >> 5) & 1u;
    if (!(fma && osxsave && avx && avx2))
        return false;
    // The OS must save and restore the XMM and YMM registers (XCR0 bits 1, 2).
#if defined(FLAYR_STEMS_X86_MSVC)
    const unsigned long long xcr0 = _xgetbv(0);
#else
    unsigned int lo = 0, hi = 0;
    __asm__ __volatile__(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0)); // xgetbv
    const unsigned long long xcr0 = ((unsigned long long)hi << 32) | lo;
#endif
    return (xcr0 & 0x6) == 0x6;
#else
    return false;
#endif
}

} // namespace chop::stems::detail
