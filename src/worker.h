#ifndef WORKER_H
#define WORKER_H

#include "types.h"

void enqueue_job(gateway_job_t *job);
void workers_start(void);
void workers_stop(void);
void job_free(gateway_job_t *job);

#endif