/* preemp.h - pre-emphasis filter for FM */

#ifndef JACKPIFM_PREEMP_H
#define JACKPIFM_PREEMP_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jackpifm_preemp_t jackpifm_preemp_t;

/* jackpifm_preemp_new: create new preemp filter object */
jackpifm_preemp_t *jackpifm_preemp_new(double sample_rate) __attribute__((malloc));

/* jackpifm_preemp_process: process samples using a filter object */
void jackpifm_preemp_process(jackpifm_preemp_t *filter, jackpifm_sample_t *data, size_t size);

/* jackpifm_preemp_free: deallocate a preemp filter object */
void jackpifm_preemp_free(jackpifm_preemp_t *filter);

#ifdef __cplusplus
}
#endif

#endif /* JACKPIFM_PREEMP_H */
