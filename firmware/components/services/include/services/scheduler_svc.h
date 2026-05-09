#pragma once
#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>

esp_err_t scheduler_svc_init(void);
void      task_scheduler(void *arg);

// Returns NTP sync status and current local time string ("YYYY-MM-DD HH:MM" or "---").
// Called by network_svc to populate /status JSON.
void scheduler_svc_get_time_info(bool *synced_out, char *buf, size_t buflen);
