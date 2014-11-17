/* stereo.h - stereo-modulate two signals at 152kHz */

#ifndef JACKPIFM_STEREO_H
#define JACKPIFM_STEREO_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jackpifm_stereo_t jackpifm_stereo_t;

/* jackpifm_stereo_new: create new stereo filter object */
jackpifm_stereo_t *jackpifm_stereo_new() __attribute__((malloc));

/* jackpifm_stereo_process: process left and right samples and write result to data
 *                          all signals are at 152kHz and all buffers have same size */
void jackpifm_stereo_process(jackpifm_stereo_t *filter, jackpifm_sample_t *data, const jackpifm_sample_t *left, const jackpifm_sample_t *right, size_t size);

/* jackpifm_stereo_free: deallocate a stereo filter object */
void jackpifm_stereo_free(jackpifm_stereo_t *filter);

#ifdef __cplusplus
}
#endif

#endif /* JACKPIFM_STEREO_H */
