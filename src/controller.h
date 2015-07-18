/* preemp.h - custom PI controller for outputter sample rate */

#ifndef JACKPIFM_CONTROLLER_H
#define JACKPIFM_CONTROLLER_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jackpifm_controller_t jackpifm_controller_t;

/* jackpifm_controller_new: create new sample rate controller */
jackpifm_controller_t *jackpifm_controller_new(
  double static_resample_factor,
  size_t target_delay,
  size_t smooth_size,
  int catch_factor,
  int catch_factor2,
  double pclamp,
  double controlquant,
  double max_resample_factor,
  double min_resample_factor
) __attribute__((malloc));

/* jackpifm_controller_clear: clean the most temporary state variables to start a new control cycle */
void jackpifm_controller_clear(jackpifm_controller_t *ctr);

/* jackpifm_controller_process: process a new delay measure and recalculate the coefficient */
double jackpifm_controller_process(jackpifm_controller_t *ctr, size_t delay);

/* jackpifm_controller_free: deallocate a controller object */
void jackpifm_controller_free(jackpifm_controller_t *ctr);

#ifdef __cplusplus
}
#endif

#endif /* JACKPIFM_CONTROLLER_H */
