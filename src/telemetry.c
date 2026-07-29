#define _POSIX_C_SOURCE 200809L

#include "telemetry.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PATH_BUFFER_SIZE 512
#define TYPE_BUFFER_SIZE 128

static int read_text_file(const char *path, char *buffer, size_t size) {
    FILE *file;

    if (path == NULL || buffer == NULL || size < 2) {
        return -1;
    }

    file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }

    if (fgets(buffer, (int) size, file) == NULL) {
        fclose(file);
        return -1;
    }

    fclose(file);
    buffer[strcspn(buffer, "\r\n")] = '\0';
    return 0;
}

static void lowercase(char *text) {
    unsigned char *cursor = (unsigned char *) text;

    while (*cursor != '\0') {
        *cursor = (unsigned char) tolower(*cursor);
        cursor++;
    }
}

static int read_temperature_value(const char *path, double *temperature_c) {
    char buffer[64];
    char *end = NULL;
    double value;

    if (read_text_file(path, buffer, sizeof(buffer)) != 0) {
        return -1;
    }

    errno = 0;
    value = strtod(buffer, &end);
    if (errno != 0 || end == buffer) {
        return -1;
    }

    if (value > 1000.0) {
        value /= 1000.0;
    }

    if (value < -40.0 || value > 180.0) {
        return -1;
    }

    *temperature_c = value;
    return 0;
}

static int read_cpu_temperature(double *temperature_c) {
    const char *thermal_root = "/sys/class/thermal";
    DIR *directory;
    struct dirent *entry;
    char fallback_path[PATH_BUFFER_SIZE] = "";
    int found = -1;

    directory = opendir(thermal_root);
    if (directory == NULL) {
        return -1;
    }

    while ((entry = readdir(directory)) != NULL) {
        char type_path[PATH_BUFFER_SIZE];
        char temp_path[PATH_BUFFER_SIZE];
        char type[TYPE_BUFFER_SIZE];
        int type_path_length;
        int temp_path_length;

        if (strncmp(entry->d_name, "thermal_zone", 12) != 0) {
            continue;
        }

        type_path_length = snprintf(
            type_path, sizeof(type_path), "%s/%s/type", thermal_root, entry->d_name);
        temp_path_length = snprintf(
            temp_path, sizeof(temp_path), "%s/%s/temp", thermal_root, entry->d_name);

        if (type_path_length < 0 || (size_t) type_path_length >= sizeof(type_path) ||
            temp_path_length < 0 || (size_t) temp_path_length >= sizeof(temp_path)) {
            continue;
        }

        if (fallback_path[0] == '\0') {
            snprintf(fallback_path, sizeof(fallback_path), "%s", temp_path);
        }

        if (read_text_file(type_path, type, sizeof(type)) != 0) {
            continue;
        }

        lowercase(type);
        if (strstr(type, "cpu") != NULL ||
            strstr(type, "soc") != NULL ||
            strstr(type, "package") != NULL) {
            if (read_temperature_value(temp_path, temperature_c) == 0) {
                found = 0;
                break;
            }
        }
    }

    closedir(directory);

    if (found == 0) {
        return 0;
    }

    if (fallback_path[0] != '\0') {
        return read_temperature_value(fallback_path, temperature_c);
    }

    return read_temperature_value(
        "/sys/devices/virtual/thermal/thermal_zone0/temp", temperature_c);
}

static int read_memory_available(double *memory_available_mb) {
    FILE *file;
    char line[256];
    unsigned long long available_kb = 0;
    unsigned long long free_kb = 0;
    unsigned long long buffers_kb = 0;
    unsigned long long cached_kb = 0;

    file = fopen("/proc/meminfo", "r");
    if (file == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned long long value = 0;

        if (sscanf(line, "MemAvailable: %llu kB", &value) == 1) {
            available_kb = value;
        } else if (sscanf(line, "MemFree: %llu kB", &value) == 1) {
            free_kb = value;
        } else if (sscanf(line, "Buffers: %llu kB", &value) == 1) {
            buffers_kb = value;
        } else if (sscanf(line, "Cached: %llu kB", &value) == 1) {
            cached_kb = value;
        }
    }

    fclose(file);

    if (available_kb == 0) {
        available_kb = free_kb + buffers_kb + cached_kb;
    }

    if (available_kb == 0) {
        return -1;
    }

    *memory_available_mb = (double) available_kb / 1024.0;
    return 0;
}

static int read_cpu_counters(uint64_t *total, uint64_t *idle) {
    FILE *file;
    char line[512];
    unsigned long long user = 0;
    unsigned long long nice = 0;
    unsigned long long system = 0;
    unsigned long long idle_ticks = 0;
    unsigned long long iowait = 0;
    unsigned long long irq = 0;
    unsigned long long softirq = 0;
    unsigned long long steal = 0;
    int fields;

    file = fopen("/proc/stat", "r");
    if (file == NULL) {
        return -1;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return -1;
    }
    fclose(file);

    fields = sscanf(
        line,
        "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
        &user,
        &nice,
        &system,
        &idle_ticks,
        &iowait,
        &irq,
        &softirq,
        &steal);

    if (fields < 4) {
        return -1;
    }

    *idle = (uint64_t) idle_ticks + (uint64_t) iowait;
    *total = (uint64_t) user + (uint64_t) nice + (uint64_t) system +
             (uint64_t) idle_ticks + (uint64_t) iowait + (uint64_t) irq +
             (uint64_t) softirq + (uint64_t) steal;
    return 0;
}

static int read_cpu_usage(telemetry_sampler_t *sampler, double *cpu_percent) {
    uint64_t total;
    uint64_t idle;

    if (read_cpu_counters(&total, &idle) != 0) {
        return -1;
    }

    if (!sampler->cpu_initialized) {
        sampler->previous_total = total;
        sampler->previous_idle = idle;
        sampler->previous_cpu_percent = 0.0;
        sampler->cpu_initialized = true;
        *cpu_percent = 0.0;
        return 0;
    }

    if (total > sampler->previous_total) {
        const uint64_t total_delta = total - sampler->previous_total;
        const uint64_t idle_delta = idle >= sampler->previous_idle
                                        ? idle - sampler->previous_idle
                                        : 0;
        const uint64_t busy_delta =
            idle_delta <= total_delta ? total_delta - idle_delta : 0;
        double usage = 100.0 * (double) busy_delta /
                       (double) total_delta;

        if (usage < 0.0) {
            usage = 0.0;
        } else if (usage > 100.0) {
            usage = 100.0;
        }
        sampler->previous_cpu_percent = usage;
    }

    sampler->previous_total = total;
    sampler->previous_idle = idle;
    *cpu_percent = sampler->previous_cpu_percent;
    return 0;
}

static int read_uptime(double *uptime_seconds) {
    FILE *file = fopen("/proc/uptime", "r");
    int result;

    if (file == NULL) {
        return -1;
    }

    result = fscanf(file, "%lf", uptime_seconds) == 1 ? 0 : -1;
    fclose(file);
    return result;
}

static void make_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm utc_time;

    if (gmtime_r(&now, &utc_time) == NULL ||
        strftime(buffer, size, "%Y-%m-%dT%H:%M:%SZ", &utc_time) == 0) {
        snprintf(buffer, size, "unknown");
    }
}

void telemetry_sampler_init(telemetry_sampler_t *sampler) {
    if (sampler == NULL) {
        return;
    }

    memset(sampler, 0, sizeof(*sampler));
}

int telemetry_read(telemetry_sampler_t *sampler, telemetry_sample_t *sample) {
    int failures = 0;

    if (sampler == NULL || sample == NULL) {
        return -1;
    }

    memset(sample, 0, sizeof(*sample));
    sample->cpu_temperature_c = -1.0;
    sample->memory_available_mb = -1.0;
    sample->cpu_usage_percent = -1.0;
    sample->uptime_seconds = -1.0;

    sample->temperature_valid =
        read_cpu_temperature(&sample->cpu_temperature_c) == 0;
    sample->memory_valid =
        read_memory_available(&sample->memory_available_mb) == 0;
    sample->cpu_valid =
        read_cpu_usage(sampler, &sample->cpu_usage_percent) == 0;

    if (read_uptime(&sample->uptime_seconds) != 0) {
        sample->uptime_seconds = -1.0;
        failures++;
    }

    make_timestamp(sample->timestamp, sizeof(sample->timestamp));

    failures += sample->temperature_valid ? 0 : 1;
    failures += sample->memory_valid ? 0 : 1;
    failures += sample->cpu_valid ? 0 : 1;
    return failures == 4 ? -1 : 0;
}
