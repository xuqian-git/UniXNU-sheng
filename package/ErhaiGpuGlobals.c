/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The one NativeAdreno board global its GPU context reads (NativeAdrenoGpuContext.cpp) that erhai's archive has no NativeAdreno
 * service to define (the pool ones are NativeAdrenoMemory.cpp's): no write-combined BO pool. The pool size stays 0, so mode 9
 * maps only the five kernel buffers. */
#include <stdint.h>
volatile uint32_t native_adreno_wc_enabled;
