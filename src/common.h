/* common.h - common definitions and constants */

#ifndef JACKPIFM_COMMON_H
#define JACKPIFM_COMMON_H

#define _POSIX_C_SOURCE 199309L

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <jack/jack.h>

#ifdef __cplusplus
extern "C" {
#endif


#define JACKPIFM_VERSION "3.0.0"
#define JACKPIFM_VERSION_MAJOR 3
#define JACKPIFM_VERSION_MINOR 0
#define JACKPIFM_VERSION_REVISION 0


typedef jack_default_audio_sample_t jackpifm_sample_t;

/* malloc / realloc / calloc wrappers */
#define JACKPIFM_ALLOC_WRAPPER(sig, call)                                     \
  static inline void *jackpifm_##sig __attribute__ ((malloc));                \
  static inline void *jackpifm_##sig {                                        \
    void *ret = call;                                                         \
    if (!ret) {                                                               \
      fprintf(stderr, "Allocation failed.\n");                                \
      abort();                                                                \
    }                                                                         \
    return ret;                                                               \
  }

JACKPIFM_ALLOC_WRAPPER(malloc(size_t size), malloc(size));
JACKPIFM_ALLOC_WRAPPER(calloc(size_t nmemb, size_t size), calloc(nmemb, size));
JACKPIFM_ALLOC_WRAPPER(realloc(void *ptr, size_t size), realloc(ptr, size));


#ifdef __cplusplus
}
#endif

#endif /* JACKPIFM_COMMON_H */
