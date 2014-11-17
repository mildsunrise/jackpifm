#include "preemp.h"

/* This isn't the right filter, but it's close...
 * TODO something with a bilinear transform not being right... */

struct jackpifm_preemp_t {
  jackpifm_sample_t last_sample;
  double fm_constant;
};

jackpifm_preemp_t *jackpifm_preemp_new(double sample_rate) {
  jackpifm_preemp_t *filter = jackpifm_malloc(sizeof(jackpifm_preemp_t));
  filter->last_sample = 0;
  filter->fm_constant = sample_rate * 75.0e-6;  /* 75µs time constant */
  return filter;
}

void jackpifm_preemp_process(jackpifm_preemp_t *filter, jackpifm_sample_t *data, size_t size) {
  jackpifm_sample_t last_sample = filter->last_sample;
  double coeff = 1 - filter->fm_constant;  /* fir of 1 + s tau */

  for (size_t i = 0; i < size; i++) {
    float sample = data[i];
    data[i] += (last_sample - sample) / coeff;
    last_sample = sample;
  }

  filter->last_sample = last_sample;
}

void jackpifm_preemp_free(jackpifm_preemp_t *filter) {
  if (!filter) return;
  free(filter);
}
