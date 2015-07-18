#include "controller.h"

#include <math.h>

#define PI 3.14159265358979323846

/* Credit: This controller was originally implemenented at alsa_out */

struct jackpifm_controller_t {
  /* Parameters */
  double static_resample_factor;
  size_t target_delay;
  size_t smooth_size;
  int catch_factor;
  int catch_factor2;
  double pclamp;
  double controlquant;
  double max_resample_factor;
  double min_resample_factor;

  /* State */
  double *smooth_offsets;
  double *window_array;
  double offset_integral;
  int offset_differential_index;
  double resample_mean;
};

inline static double hann(double x) {
  return 0.5 * (1.0 - cos(2*PI * x));
}

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
) {
  jackpifm_controller_t *ctr = jackpifm_malloc(sizeof(jackpifm_controller_t));

  ctr->static_resample_factor = static_resample_factor;
  ctr->target_delay = target_delay;
  ctr->smooth_size = smooth_size;
  ctr->catch_factor = catch_factor;
  ctr->catch_factor2 = catch_factor2;
  ctr->pclamp = pclamp;
  ctr->controlquant = controlquant;
  ctr->max_resample_factor = max_resample_factor;
  ctr->min_resample_factor = min_resample_factor;

  ctr->smooth_offsets = jackpifm_calloc(smooth_size, sizeof(double));
  ctr->window_array = jackpifm_calloc(smooth_size, sizeof(double));
  for (size_t i = 0; i < smooth_size; i++)
    ctr->window_array[i] = hann(i / ((double)(smooth_size - 1)));
  ctr->offset_integral = 0;
  ctr->offset_differential_index = 0;
  ctr->resample_mean = static_resample_factor;

  return ctr;
}

void jackpifm_controller_clear(jackpifm_controller_t *ctr) {
  /* Set the resample_rate... we need to adjust the offset integral, to do this.
   * first look at the PI controller, this code is just a special case, which should never execute once
   * everything is swung in. */
  ctr->offset_integral = - (ctr->resample_mean - ctr->static_resample_factor) * ctr->catch_factor * ctr->catch_factor2;

  /* Also clear the array. we are beginning a new control cycle. */
  memset(ctr->smooth_offsets, 0x00, ctr->smooth_size);
}

double jackpifm_controller_process(jackpifm_controller_t *ctr, size_t delay) {
  double offset = (int) delay - (int) ctr->target_delay;

  /* Save offset. */
  ctr->smooth_offsets[(ctr->offset_differential_index++) % ctr->smooth_size] = offset;

  /* Build the mean of the windowed offset array basically for lowpassing. */
  double smooth_offset = 0.0;
  for (size_t i = 0; i < ctr->smooth_size; i++)
    smooth_offset += ctr->smooth_offsets[(i + ctr->offset_differential_index - 1) % ctr->smooth_size] * ctr->window_array[i];
  smooth_offset /= (double) ctr->smooth_size;

  /* this is the integral of the smoothed_offset */
  ctr->offset_integral += smooth_offset;

  /* Clamp offset.
   * the smooth offset still contains unwanted noise
   * which would go straight onto the resample coefficient.
   * it only used in the P component and the I component is used for the fine tuning anyways. */
  if (fabs(smooth_offset) < ctr->pclamp)
    smooth_offset = 0.0;

  /* ok. now this is the PI controller.
   * u(t) = K * ( e(t) + 1/T \int e(t') dt' )
   * K = 1/catch_factor and T = catch_factor2 */
  double resample_factor = ctr->static_resample_factor;
  resample_factor -= smooth_offset / (double)ctr->catch_factor;
  resample_factor -= ctr->offset_integral / (double)ctr->catch_factor / (double)ctr->catch_factor2;

  /* now quantize this value around resample_mean, so that the noise which is in the integral component doesnt hurt. */
  resample_factor = floor((resample_factor - ctr->resample_mean) * ctr->controlquant + 0.5) / ctr->controlquant + ctr->resample_mean;

  /* Clamp a bit. */
  if (resample_factor < ctr->min_resample_factor) resample_factor = ctr->min_resample_factor;
  else if (resample_factor > ctr->max_resample_factor) resample_factor = ctr->max_resample_factor;

  /* Calculate resample_mean so we can init ourselves to saner values. */
  ctr->resample_mean = 0.9999 * ctr->resample_mean + 0.0001 * resa
  return resample_factor;
}

void jackpifm_controller_free(jackpifm_controller_t *ctr) {
  if (!ctr) return;
  free(ctr);
}
