#ifndef SMART_GUARD_TELEMETRY_H
#define SMART_GUARD_TELEMETRY_H

#include <stdbool.h>
#include <stdint.h>

#define TELEMETRY_TIMESTAMP_SIZE 32

typedef struct {
    uint64_t previous_total;
    uint64_t previous_idle;
    double previous_cpu_percent;
    bool cpu_initialized;
} telemetry_sampler_t;

typedef struct {
    double cpu_temperature_c;
    double memory_available_mb;
    double cpu_usage_percent;
    double uptime_seconds;
    bool temperature_valid;
    bool memory_valid;
    bool cpu_valid;
    char timestamp[TELEMETRY_TIMESTAMP_SIZE];
} telemetry_sample_t;

void telemetry_sampler_init(telemetry_sampler_t *sampler);
int telemetry_read(telemetry_sampler_t *sampler, telemetry_sample_t *sample);

#endif
