#pragma once
#include "tes_protocol/tes_types.h"
#include <esp_err.h>
#include <stdint.h>

esp_err_t log_svc_init      (void);
uint8_t   log_svc_get_history(charge_session_t *buf, uint8_t max);  // newest-first
void      task_log           (void *arg);
