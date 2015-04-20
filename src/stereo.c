#include "stereo.h"

#include <math.h>

#define PI 3.14159265358979323846

struct jackpifm_stereo_t {
  int state;
  double sin[16];
};

jackpifm_stereo_t *jackpifm_stereo_new() {
  jackpifm_stereo_t *filter = jackpifm_malloc(sizeof(jackpifm_stereo_t));
  filter->state = 0;
  for (size_t i = 0; i < 16; i++)
    filter->sin[i] = sin(i * 2*PI/8);
  return filter;
}

void jackpifm_stereo_process(jackpifm_stereo_t *filter, jackpifm_sample_t *data, const jackpifm_sample_t *left, const jackpifm_sample_t *right, size_t size) {
  int state = filter->state;
  double *sin = filter->sin;

  for (size_t i = 0; i < size; i++) {
    jackpifm_sample_t med = (left[i]+right[i]) + (left[i]-right[i])*sin[state*2];
    data[i] = 0.9 * med/2  +  0.1 * sin[state];
    state = (state+1) % 8;
  }

  filter->state = state;
}

void jackpifm_stereo_free(jackpifm_stereo_t *filter) {
  if (!filter) return;
  free(filter);
}
