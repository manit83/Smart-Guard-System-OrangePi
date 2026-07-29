#define main thermal_manager_program_main
#include "../src/thermal_manager.c"
#undef main

#include <assert.h>
#include <stdio.h>

int main(void) {
    thermal_config_t config;
    char directory[] = "/tmp/smart-guard-thermal-test.XXXXXX";
    char override_path[SG_PATH_SIZE];
    char state_path[SG_PATH_SIZE];
    char contents[1024];
    char error[SG_ERROR_SIZE];
    sg_thermal_mode_t mode;
    bool present;
    bool changed;

    assert(mkdtemp(directory) != NULL);
    assert(snprintf(
        override_path,
        sizeof(override_path),
        "%s/vision-thermal.env",
        directory) > 0);
    assert(snprintf(
        state_path,
        sizeof(state_path),
        "%s/thermal-mode",
        directory) > 0);

    memset(&config, 0, sizeof(config));
    snprintf(
        config.override_path,
        sizeof(config.override_path),
        "%s",
        override_path);
    config.hot_width = 320;
    config.hot_height = 240;
    config.hot_input_fps = 5;
    config.hot_output_fps = 1.0;
    config.hot_detection_width = 240;

    assert(synchronize_profile(
        &config,
        SG_THERMAL_HOT,
        &changed,
        error,
        sizeof(error)) == 0);
    assert(changed);
    assert(read_small_file(
        override_path,
        contents,
        sizeof(contents)) == 0);
    assert(strstr(contents, "CAMERA_WIDTH=320\n") != NULL);
    assert(strstr(contents, "CAMERA_HEIGHT=240\n") != NULL);
    assert(strstr(contents, "CAMERA_INPUT_FPS=5\n") != NULL);
    assert(strstr(contents, "VISION_OUTPUT_FPS=1.00\n") != NULL);
    assert(strstr(contents, "VISION_DETECTION_WIDTH=240\n") != NULL);

    assert(synchronize_profile(
        &config,
        SG_THERMAL_HOT,
        &changed,
        error,
        sizeof(error)) == 0);
    assert(!changed);

    assert(persist_mode(
        state_path,
        SG_THERMAL_HOT,
        &changed,
        error,
        sizeof(error)) == 0);
    assert(changed);
    assert(read_mode_file(state_path, &mode, &present) == 0);
    assert(present);
    assert(mode == SG_THERMAL_HOT);

    assert(synchronize_profile(
        &config,
        SG_THERMAL_NORMAL,
        &changed,
        error,
        sizeof(error)) == 0);
    assert(changed);
    assert(access(override_path, F_OK) != 0);
    assert(errno == ENOENT);

    assert(unlink(state_path) == 0);
    assert(rmdir(directory) == 0);

    puts("thermal file transition tests passed");
    return 0;
}
