#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <time.h>
#include <unistd.h>

#define PATH_SIZE 512
#define COMMAND_SIZE 64

typedef struct {
    char request_path[PATH_SIZE];
    unsigned int poll_interval_ms;
    unsigned int reboot_delay_sec;
    bool reboot_enabled;
} command_config_t;

static volatile sig_atomic_t running = 1;

static void stop_runner(int signal_number) {
    (void) signal_number;
    running = 0;
}

static const char *environment_or_default(const char *name,
                                          const char *default_value) {
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : default_value;
}

static unsigned int read_unsigned(const char *name,
                                  unsigned int default_value,
                                  unsigned int minimum,
                                  unsigned int maximum) {
    const char *text = getenv(name);
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] == '\0') {
        return default_value;
    }

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < minimum || value > maximum) {
        fprintf(stderr, "Invalid %s value: %s\n", name, text);
        exit(EXIT_FAILURE);
    }
    return (unsigned int) value;
}

static bool read_boolean(const char *name, bool default_value) {
    const char *text = getenv(name);

    if (text == NULL || text[0] == '\0') {
        return default_value;
    }
    if (strcmp(text, "1") == 0 || strcmp(text, "true") == 0 ||
        strcmp(text, "on") == 0) {
        return true;
    }
    if (strcmp(text, "0") == 0 || strcmp(text, "false") == 0 ||
        strcmp(text, "off") == 0) {
        return false;
    }

    fprintf(stderr, "Invalid %s value: %s\n", name, text);
    exit(EXIT_FAILURE);
}

static void load_config(command_config_t *config) {
    memset(config, 0, sizeof(*config));
    snprintf(
        config->request_path,
        sizeof(config->request_path),
        "%s",
        environment_or_default(
            "COMMAND_REQUEST_PATH",
            "/var/lib/smart-guard/command-request"));
    config->poll_interval_ms =
        read_unsigned("COMMAND_POLL_INTERVAL_MS", 250, 50, 5000);
    config->reboot_delay_sec =
        read_unsigned("COMMAND_REBOOT_DELAY_SEC", 2, 1, 30);
    config->reboot_enabled =
        read_boolean("COMMAND_REBOOT_ENABLED", true);
}

static void sleep_milliseconds(unsigned int milliseconds) {
    struct timespec delay;

    delay.tv_sec = (time_t) (milliseconds / 1000U);
    delay.tv_nsec = (long) (milliseconds % 1000U) * 1000000L;
    while (running && nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static int take_request(const char *path, char *command, size_t command_size) {
    FILE *file;

    file = fopen(path, "r");
    if (file == NULL) {
        return errno == ENOENT ? 0 : -1;
    }

    if (fgets(command, (int) command_size, file) == NULL) {
        fclose(file);
        unlink(path);
        return -1;
    }
    fclose(file);
    command[strcspn(command, "\r\n")] = '\0';

    if (unlink(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "WARNING Cannot remove command request %s: %s\n",
                path, strerror(errno));
    }
    return 1;
}

static int execute_reboot(const command_config_t *config) {
    if (!config->reboot_enabled) {
        fprintf(stderr, "WARNING Reboot command rejected by configuration\n");
        return -1;
    }

    fprintf(
        stderr,
        "INFO Reboot command accepted; rebooting in %u second(s)\n",
        config->reboot_delay_sec);
    fflush(NULL);
    sleep_milliseconds(config->reboot_delay_sec * 1000U);
    if (!running) {
        return 0;
    }

    sync();
    if (reboot(RB_AUTOBOOT) != 0) {
        fprintf(stderr, "ERROR reboot system call failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int execute_command(const command_config_t *config,
                           const char *command) {
    if (strcmp(command, "reboot") == 0) {
        return execute_reboot(config);
    }

    fprintf(stderr, "WARNING Unsupported queued command: %s\n", command);
    return -1;
}

int main(void) {
    command_config_t config;
    struct sigaction action;

    load_config(&config);
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_runner;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    fprintf(
        stderr,
        "INFO Command runner started: request=%s reboot=%s\n",
        config.request_path,
        config.reboot_enabled ? "enabled" : "disabled");

    while (running) {
        char command[COMMAND_SIZE] = "";
        const int result =
            take_request(config.request_path, command, sizeof(command));

        if (result > 0) {
            execute_command(&config, command);
        } else if (result < 0) {
            fprintf(
                stderr,
                "WARNING Cannot read command request %s: %s\n",
                config.request_path,
                strerror(errno));
        }
        sleep_milliseconds(config.poll_interval_ms);
    }

    fprintf(stderr, "INFO Command runner stopping\n");
    return EXIT_SUCCESS;
}
