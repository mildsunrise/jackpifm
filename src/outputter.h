/* rds.h - encodes a chunk of RDS data into a 152kHz signal */

#ifndef JACKPIFM_OUTPUTTER_H
#define JACKPIFM_OUTPUTTER_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JACKPIFM_BUFFERINSTRUCTIONS 32768

int jackpifm_setup_fm();
void jackpifm_setup_dma(float center_freq);
void jackpifm_unsetup_dma();

void jackpifm_outputter_setup(double sample_rate, size_t period_size);
void jackpifm_outputter_sync();
void jackpifm_outputter_output(const jackpifm_sample_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* JACKPIFM_RDS_H */
