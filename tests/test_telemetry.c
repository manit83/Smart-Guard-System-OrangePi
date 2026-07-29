#include "telemetry.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(void) {
    telemetry_sampler_t sampler;
    telemetry_sample_t first;
    telemetry_sample_t second;
    const struct timespec delay = {0, 250000000L};

    telemetry_sampler_init(&sampler);
    if (telemetry_read(&sampler, &first) != 0) {
        fprintf(stderr, "Could not read telemetry from /proc and /sys.\n");
        return EXIT_FAILURE;
    }

    nanosleep(&delay, NULL);
    if (telemetry_read(&sampler, &second) != 0) {
        fprintf(stderr, "Could not read the second telemetry sample.\n");
        return EXIT_FAILURE;
    }

    printf("timestamp=%s\n", second.timestamp);
    printf(
        "temperature_c=%s%.1f\n",
        second.temperature_valid ? "" : "unavailable/",
        second.cpu_temperature_c);
    printf(
        "memory_available_mb=%s%.1f\n",
        second.memory_valid ? "" : "unavailable/",
        second.memory_available_mb);
    printf(
        "cpu_percent=%s%.1f\n",
        second.cpu_valid ? "" : "unavailable/",
        second.cpu_usage_percent);
    printf("uptime_seconds=%.0f\n", second.uptime_seconds);

    if (!second.memory_valid || !second.cpu_valid) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
