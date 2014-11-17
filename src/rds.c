#include "rds.h"

#include <math.h>

#define PI 3.14159265358979323846
#define EXTRACT_BIT(byte, n) ((byte) >> (7-(n))) & 1

struct jackpifm_rds_t {
  jackpifm_sample_t current_sample;
  bool current_bit;
  int state;
  int bit_num;
  double sin[8];

  uint8_t *rds_data;
  size_t rds_size;
};

jackpifm_rds_t *jackpifm_rds_new(const uint8_t *rds_data, size_t rds_size) {
  jackpifm_rds_t *filter = jackpifm_malloc(sizeof(jackpifm_rds_t));
  filter->current_sample = 0;
  filter->current_bit = 0;
  filter->state = 0;
  filter->bit_num = 0;
  for (size_t i = 0; i < 8; i++)
    filter->sin[i] = sin(i * 2*PI*3/8);

  filter->rds_data = jackpifm_malloc(rds_size);
  filter->rds_size = rds_size;
  memcpy(filter->rds_data, rds_data, rds_size);
  return filter;
}

void jackpifm_rds_process(jackpifm_rds_t *filter, jackpifm_sample_t *data, size_t size) {
  int state = filter->state;
  jackpifm_sample_t current_sample = filter->current_sample;
  bool current_bit = filter->current_bit;

  for (size_t i = 0; i < size; i++) {
    if (state == 0) {
      /* get the next bit */
      uint8_t new_byte = filter->rds_data[filter->bit_num / 8];
      bool new_bit = EXTRACT_BIT(new_byte, filter->bit_num % 8);
      filter->bit_num = (filter->bit_num+1) % (filter->rds_size * 8);

      current_bit ^= new_bit;  /* differential encoding */
    }

    bool output_bit = (state < 384/2) ? current_bit : !current_bit;  /* manchester encoding */
    /* very simple IIR filter to hopefully reduce sidebands */
    current_sample = 0.99 * current_sample + 0.01 * (output_bit ? +1 : -1);

    data[i] += 0.05 * current_sample * filter->sin[state%8];
    state = (state+1) % 384;
  }

  filter->state = state;
  filter->current_sample = current_sample;
  filter->current_bit = current_bit;
}

void jackpifm_rds_free(jackpifm_rds_t *filter) {
  if (!filter) return;
  free(filter->rds_data);
  free(filter);
}
