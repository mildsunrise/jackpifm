/* rds.h - encodes a chunk of RDS data into a 152kHz signal */

#ifndef JACKPIFM_RDS_H
#define JACKPIFM_RDS_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jackpifm_rds_t jackpifm_rds_t;

/* jackpifm_rds_new: create new RDS filter object */
jackpifm_rds_t *jackpifm_rds_new(const uint8_t *rds_data, size_t rds_size) __attribute__((malloc));

/* jackpifm_rds_process: process samples at 152kHz using the filter */
void jackpifm_rds_process(jackpifm_rds_t *filter, jackpifm_sample_t *data, size_t size);

/* jackpifm_rds_free: deallocate an RDS filter object */
void jackpifm_rds_free(jackpifm_rds_t *filter);

#ifdef __cplusplus
}
#endif

#endif /* JACKPIFM_RDS_H */
