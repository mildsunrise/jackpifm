/* resamp.h - simple resampling filter */

#ifndef JACKPIFM_RESAMP_H
#define JACKPIFM_RESAMP_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jackpifm_resamp_t jackpifm_resamp_t;

/* jackpifm_resamp_new: create new resamp filter object */
jackpifm_resamp_t *jackpifm_resamp_new(float ratio, size_t quality, size_t squality) __attribute__((malloc));

/* jackpifm_resamp_process: process samples using a filter object */
size_t jackpifm_resamp_process(jackpifm_resamp_t *filter, jackpifm_sample_t *out, const jackpifm_sample_t *data, size_t size);

/* jackpifm_resamp_free: deallocate a resamp filter object */
void jackpifm_resamp_free(jackpifm_resamp_t *filter);

#ifdef __cplusplus
}
#endif

#endif /* JACKPIFM_RESAMP_H */
