#pragma once

#include <hardware.h>

#define P_SPC__HYPERTHREADING_FACTOR 2

#if !defined(SPC__THREAD_COUNT) || ((SPC__THREAD_COUNT+0) == 0)
    #if defined(SPC__CORE_COUNT) && ((SPC__CORE_COUNT+0) > 0)
        #define P_SPC__THREAD_COUNT SPC__CORE_COUNT
    #else
        #define P_SPC__THREAD_COUNT 4
    #endif
#else
#define P_SPC__THREAD_COUNT SPC__THREAD_COUNT
#endif

#if !defined(SPC__CORE_COUNT) || ((SPC__CORE_COUNT+0) == 0)
#define P_SPC__CORE_COUNT 4
#else
    #if (SPC__CORE_COUNT * P_SPC__HYPERTHREADING_FACTOR) < P_SPC__THREAD_COUNT
        #define P_SPC__CORE_COUNT (SPC__CORE_COUNT * P_SPC__HYPERTHREADING_FACTOR)
    #else
        #define P_SPC__CORE_COUNT P_SPC__THREAD_COUNT
    #endif
#endif

#if !defined(SPC__LEVEL1_DCACHE_SIZE) || ((SPC__LEVEL1_DCACHE_SIZE+0) == 0)
#define P_SPC__LEVEL1_DCACHE_SIZE 32000
#else
#define P_SPC__LEVEL1_DCACHE_SIZE SPC__LEVEL1_DCACHE_SIZE
#endif

#if !defined(SPC__LEVEL2_CACHE_SIZE) || ((SPC__LEVEL2_CACHE_SIZE+0) == 0)
#define P_SPC__LEVEL2_CACHE_SIZE P_SPC__LEVEL1_DCACHE_SIZE
#else
#define P_SPC__LEVEL2_CACHE_SIZE SPC__LEVEL2_CACHE_SIZE
#endif

#if !defined(SPC__LEVEL3_CACHE_SIZE) || ((SPC__LEVEL3_CACHE_SIZE+0) == 0)
#define P_SPC__LEVEL3_CACHE_SIZE P_SPC__LEVEL2_CACHE_SIZE
#else
#define P_SPC__LEVEL3_CACHE_SIZE SPC__LEVEL3_CACHE_SIZE
#endif

#if !defined(SPC__LEVEL4_CACHE_SIZE) || ((SPC__LEVEL4_CACHE_SIZE+0) == 0)
#define P_SPC__LEVEL4_CACHE_SIZE P_SPC__LEVEL3_CACHE_SIZE
#else
#define P_SPC__LEVEL4_CACHE_SIZE SPC__LEVEL4_CACHE_SIZE
#endif

#define P_SPC__LAST_LEVEL_CACHE_SIZE P_SPC__LEVEL4_CACHE_SIZE

#ifdef SPC__CPU_NAME // we will never define this locally
#ifdef SPC__AARCH64 // arm64
#define P_SPC__SLEEP 700000
#elif defined(SPC__PPC64LE) // ppc64le
#define P_SPC__SLEEP 1200000
#elif defined(SPC__X86_64) // x86_64
    #ifdef SPC__SUPPORTS_AVX2 // amd
    #define P_SPC__SLEEP 700000
    #else // intel
    #define P_SPC__SLEEP 1000000
    #endif
#endif
#else
#define P_SPC__SLEEP 0
#endif