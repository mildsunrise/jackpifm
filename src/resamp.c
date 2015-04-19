#include "resamp.h"

#include <math.h>

#define PI 3.14159265358979323846

struct jackpifm_resamp_t {
  /* Static parameters */
  float ratio;
  size_t quality;
  size_t squality;

  /* Lookup tables */
  jackpifm_sample_t **sinc_lut;
  
  /* Variables */
  jackpifm_sample_t *sample_data;
  float free_time;
};

jackpifm_resamp_t *jackpifm_resamp_new(float ratio, size_t quality, size_t squality) {
  jackpifm_resamp_t *filter = jackpifm_malloc(sizeof(jackpifm_resamp_t));
  filter->ratio = ratio;
  filter->quality = quality;
  filter->squality = squality;
  filter->sample_data = jackpifm_calloc(quality, sizeof(jackpifm_sample_t));
  filter->free_time = 1;

  filter->sinc_lut = jackpifm_calloc(squality, sizeof(jackpifm_sample_t *));
  for (size_t lut_num = 0; lut_num < squality; lut_num++) {
    filter->sinc_lut[lut_num] = jackpifm_calloc(quality, sizeof(jackpifm_sample_t));
    for (size_t sample_num = 0; sample_num < quality; sample_num++) {
      float x = (quality-1)/2.0 + lut_num/(float)squality - sample_num;
      filter->sinc_lut[lut_num][sample_num] = (x == 0) ? 1 : sinf(x)/x;
    }
  }

  return filter;
}

size_t jackpifm_resamp_process(jackpifm_resamp_t *filter, jackpifm_sample_t *out, const jackpifm_sample_t *data, size_t size) {
  size_t quality = filter->quality, squality = filter->squality;
  jackpifm_sample_t *sample_data = filter->sample_data;
  float free_time = filter->free_time, ratio = filter->ratio;
  size_t o = 0;

  for (size_t i = 0; i < size; i++) {
    /* Shift old samples to the left */
    for (size_t s = 0; s < quality; s++)
      sample_data[s] = sample_data[s+1];

    /* Insert sample at the end */
    sample_data[quality-1] = data[i];
    free_time -= 1;

    /* Output resampled samples */
    while (free_time < 1) {
      float out_sample = 0;
      jackpifm_sample_t *lut = filter->sinc_lut[(int)(free_time*squality)];
      for (size_t s = 0; s < quality; s++)
        out_sample += sample_data[s] * lut[s];
      out[o++] = out_sample;
      free_time += ratio;
    }
  }

  filter->free_time = free_time;
  return o;
}

void jackpifm_resamp_free(jackpifm_resamp_t *filter) {
  if (!filter) return;
  for (size_t lut_num = 0; lut_num < filter->squality; lut_num++)
    free(filter->sinc_lut[lut_num]);
  free(filter->sinc_lut);
  free(filter->sample_data);
  free(filter);
}
