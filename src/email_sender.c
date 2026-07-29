#define _POSIX_C_SOURCE 200809L

#include "email_sender.h"

#include <curl/curl.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SMTP_PASSWORD_SIZE 512

static void set_error(char *error, size_t size, const char *format, ...) {
    va_list arguments;

    if (error == NULL || size == 0) {
        return;
    }

    va_start(arguments, format);
    vsnprintf(error, size, format, arguments);
    va_end(arguments);
}

static char *trim(char *text) {
    char *end;

    while (*text == ' ' || *text == '\t') {
        text++;
    }
    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    *end = '\0';
    return text;
}

static int envelope_address(
    const char *address,
    char *buffer,
    size_t buffer_size) {
    int length;
    size_t address_length = strlen(address);

    if (address_length >= 2 &&
        address[0] == '<' &&
        address[address_length - 1] == '>') {
        length = snprintf(buffer, buffer_size, "%s", address);
    } else {
        length = snprintf(buffer, buffer_size, "<%s>", address);
    }
    return length < 0 || (size_t) length >= buffer_size ? -1 : 0;
}

static int add_recipients(
    const char *email_to,
    struct curl_slist **recipients,
    char *error,
    size_t error_size) {
    char *copy;
    char *cursor;
    char *save = NULL;
    int count = 0;

    copy = strdup(email_to);
    if (copy == NULL) {
        set_error(error, error_size, "cannot allocate recipient list");
        return -1;
    }

    cursor = strtok_r(copy, ",", &save);
    while (cursor != NULL) {
        char *address = trim(cursor);
        char envelope[SG_TEXT_SIZE + 4];
        struct curl_slist *updated;

        if (*address == '\0' ||
            strchr(address, '\r') != NULL ||
            strchr(address, '\n') != NULL ||
            envelope_address(address, envelope, sizeof(envelope)) != 0) {
            free(copy);
            set_error(error, error_size, "EMAIL_TO contains an invalid address");
            return -1;
        }

        updated = curl_slist_append(*recipients, envelope);
        if (updated == NULL) {
            free(copy);
            set_error(error, error_size, "cannot allocate SMTP recipient");
            return -1;
        }
        *recipients = updated;
        count++;
        cursor = strtok_r(NULL, ",", &save);
    }

    free(copy);
    if (count == 0) {
        set_error(error, error_size, "EMAIL_TO contains no recipients");
        return -1;
    }
    return 0;
}

static int add_header(
    struct curl_slist **headers,
    const char *name,
    const char *value,
    char *error,
    size_t error_size) {
    char line[1024];
    struct curl_slist *updated;
    int length = snprintf(line, sizeof(line), "%s: %s", name, value);

    if (length < 0 || (size_t) length >= sizeof(line)) {
        set_error(error, error_size, "%s header is too long", name);
        return -1;
    }
    updated = curl_slist_append(*headers, line);
    if (updated == NULL) {
        set_error(error, error_size, "cannot allocate %s header", name);
        return -1;
    }
    *headers = updated;
    return 0;
}

static int make_mail_date(char *buffer, size_t buffer_size) {
    time_t now = time(NULL);
    struct tm utc;

    if (now == (time_t) -1 || gmtime_r(&now, &utc) == NULL) {
        return -1;
    }
    return strftime(
        buffer,
        buffer_size,
        "%a, %d %b %Y %H:%M:%S +0000",
        &utc) == 0 ? -1 : 0;
}

int sg_email_global_init(char *error, size_t error_size) {
    CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);

    if (result != CURLE_OK) {
        set_error(
            error,
            error_size,
            "libcurl initialization failed: %s",
            curl_easy_strerror(result));
        return -1;
    }
    return 0;
}

void sg_email_global_cleanup(void) {
    curl_global_cleanup();
}

static int send_email_message(
    const sg_config_t *config,
    const char *subject,
    const char *body,
    const char *attachment_path,
    const char *attachment_name,
    char *error,
    size_t error_size) {
    CURL *curl = NULL;
    curl_mime *mime = NULL;
    curl_mimepart *part;
    struct curl_slist *recipients = NULL;
    struct curl_slist *headers = NULL;
    char password[SMTP_PASSWORD_SIZE];
    char url[SG_TEXT_SIZE + 64];
    char envelope_from[SG_TEXT_SIZE + 4];
    char mail_date[64];
    char curl_error[CURL_ERROR_SIZE] = "";
    int length;
    CURLcode result;
    int status = -1;

    if (config == NULL || subject == NULL || body == NULL) {
        set_error(error, error_size, "email input is null");
        return -1;
    }

    if (sg_read_secret(
            config->smtp_password_file,
            password,
            sizeof(password),
            error,
            error_size) != 0) {
        return -1;
    }

    length = snprintf(
        url,
        sizeof(url),
        "%s://%s:%d",
        strcmp(config->smtp_security, "tls") == 0 ? "smtps" : "smtp",
        config->smtp_host,
        config->smtp_port);
    if (length < 0 || (size_t) length >= sizeof(url)) {
        set_error(error, error_size, "SMTP URL is too long");
        goto cleanup;
    }

    if (make_mail_date(mail_date, sizeof(mail_date)) != 0) {
        set_error(error, error_size, "cannot format the email Date header");
        goto cleanup;
    }
    if (envelope_address(
            config->smtp_from,
            envelope_from,
            sizeof(envelope_from)) != 0) {
        set_error(error, error_size, "SMTP_FROM is too long");
        goto cleanup;
    }

    curl = curl_easy_init();
    if (curl == NULL) {
        set_error(error, error_size, "cannot initialize an SMTP transfer");
        goto cleanup;
    }

    if (add_recipients(
            config->email_to,
            &recipients,
            error,
            error_size) != 0 ||
        add_header(&headers, "To", config->email_to, error, error_size) != 0 ||
        add_header(&headers, "From", config->smtp_from, error, error_size) != 0 ||
        add_header(&headers, "Subject", subject, error, error_size) != 0 ||
        add_header(&headers, "Date", mail_date, error, error_size) != 0) {
        goto cleanup;
    }

    mime = curl_mime_init(curl);
    if (mime == NULL) {
        set_error(error, error_size, "cannot create the email MIME body");
        goto cleanup;
    }

    part = curl_mime_addpart(mime);
    if (part == NULL ||
        curl_mime_data(part, body, CURL_ZERO_TERMINATED) != CURLE_OK ||
        curl_mime_type(part, "text/plain; charset=UTF-8") != CURLE_OK) {
        set_error(error, error_size, "cannot create the email text part");
        goto cleanup;
    }

    if (attachment_path != NULL && attachment_path[0] != '\0') {
        part = curl_mime_addpart(mime);
        if (part == NULL ||
            curl_mime_filedata(part, attachment_path) != CURLE_OK ||
            curl_mime_filename(
                part,
                attachment_name != NULL
                    ? attachment_name
                    : "smart-guard-alert.jpg") != CURLE_OK ||
            curl_mime_type(part, "image/jpeg") != CURLE_OK ||
            curl_mime_encoder(part, "base64") != CURLE_OK) {
            set_error(
                error,
                error_size,
                "cannot attach %s",
                attachment_path);
            goto cleanup;
        }
    }

#define SETOPT(option, value)                                                \
    do {                                                                     \
        result = curl_easy_setopt(curl, option, value);                      \
        if (result != CURLE_OK) {                                            \
            set_error(                                                       \
                error,                                                       \
                error_size,                                                  \
                "cannot configure SMTP: %s",                                 \
                curl_easy_strerror(result));                                 \
            goto cleanup;                                                    \
        }                                                                    \
    } while (0)

    SETOPT(CURLOPT_URL, url);
    SETOPT(CURLOPT_USERNAME, config->smtp_username);
    SETOPT(CURLOPT_PASSWORD, password);
    SETOPT(CURLOPT_MAIL_FROM, envelope_from);
    SETOPT(CURLOPT_MAIL_RCPT, recipients);
    SETOPT(CURLOPT_HTTPHEADER, headers);
    SETOPT(CURLOPT_MIMEPOST, mime);
    SETOPT(CURLOPT_CONNECTTIMEOUT, 10L);
    SETOPT(CURLOPT_TIMEOUT, 30L);
    SETOPT(CURLOPT_NOSIGNAL, 1L);
    SETOPT(CURLOPT_SSL_VERIFYPEER, 1L);
    SETOPT(CURLOPT_SSL_VERIFYHOST, 2L);
    SETOPT(CURLOPT_ERRORBUFFER, curl_error);

    if (strcmp(config->smtp_security, "starttls") == 0 ||
        strcmp(config->smtp_security, "tls") == 0) {
        SETOPT(CURLOPT_USE_SSL, (long) CURLUSESSL_ALL);
    } else {
        SETOPT(CURLOPT_USE_SSL, (long) CURLUSESSL_NONE);
    }

    if (config->smtp_ca_file[0] != '\0') {
        SETOPT(CURLOPT_CAINFO, config->smtp_ca_file);
    }

#undef SETOPT

    result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        set_error(
            error,
            error_size,
            "SMTP transfer failed: %s%s%s",
            curl_easy_strerror(result),
            curl_error[0] == '\0' ? "" : " - ",
            curl_error);
        goto cleanup;
    }

    status = 0;

cleanup:
    memset(password, 0, sizeof(password));
    if (mime != NULL) {
        curl_mime_free(mime);
    }
    curl_slist_free_all(headers);
    curl_slist_free_all(recipients);
    if (curl != NULL) {
        curl_easy_cleanup(curl);
    }
    return status;
}

int sg_send_alert_email(
    const sg_config_t *config,
    const sg_vision_state_t *state,
    double temperature_c,
    char *error,
    size_t error_size) {
    char subject[SG_TEXT_SIZE + 128];
    char body[1024];
    int length;

    if (config == NULL || state == NULL) {
        set_error(error, error_size, "detection email input is null");
        return -1;
    }

    length = snprintf(
        subject,
        sizeof(subject),
        "%s: %d person%s detected",
        config->email_subject_prefix,
        state->persons,
        state->persons == 1 ? "" : "s");
    if (length < 0 || (size_t) length >= sizeof(subject)) {
        set_error(error, error_size, "email subject is too long");
        return -1;
    }

    length = snprintf(
        body,
        sizeof(body),
        "Smart Guard detection alert\n\n"
        "Guard mode: ARMED\n"
        "Student ID: %s\n"
        "People detected: %d\n"
        "Detection timestamp: %s\n"
        "Current CPU temperature: %.1f C\n\n"
        "The annotated camera image is attached as smart-guard-alert.jpg.\n",
        config->student_id,
        state->persons,
        state->timestamp,
        temperature_c);
    if (length < 0 || (size_t) length >= sizeof(body)) {
        set_error(error, error_size, "email body is too long");
        return -1;
    }

    return send_email_message(
        config,
        subject,
        body,
        config->latest_frame_path,
        "smart-guard-alert.jpg",
        error,
        error_size);
}

int sg_send_tamper_email(
    const sg_config_t *config,
    const char *timestamp,
    double temperature_c,
    unsigned long stale_seconds,
    char *error,
    size_t error_size) {
    struct stat information;
    const char *attachment = NULL;
    char subject[SG_TEXT_SIZE + 128];
    char body[1024];
    int length;

    if (config == NULL || timestamp == NULL) {
        set_error(error, error_size, "tamper email input is null");
        return -1;
    }

    length = snprintf(
        subject,
        sizeof(subject),
        "%s: CAMERA TAMPERING",
        config->email_subject_prefix);
    if (length < 0 || (size_t) length >= sizeof(subject)) {
        set_error(error, error_size, "tamper email subject is too long");
        return -1;
    }

    length = snprintf(
        body,
        sizeof(body),
        "Smart Guard camera tampering warning\n\n"
        "Student ID: %s\n"
        "Warning timestamp: %s\n"
        "No new camera frame for: %lu seconds\n"
        "Current CPU temperature: %.1f C\n"
        "Action: smart-guard-vision.service restart requested.\n\n"
        "A stale last frame is attached when one is available.\n",
        config->student_id,
        timestamp,
        stale_seconds,
        temperature_c);
    if (length < 0 || (size_t) length >= sizeof(body)) {
        set_error(error, error_size, "tamper email body is too long");
        return -1;
    }

    if (stat(config->latest_frame_path, &information) == 0 &&
        S_ISREG(information.st_mode) &&
        information.st_size > 0) {
        attachment = config->latest_frame_path;
    }

    return send_email_message(
        config,
        subject,
        body,
        attachment,
        "smart-guard-last-frame.jpg",
        error,
        error_size);
}

int sg_send_thermal_email(
    const sg_config_t *config,
    const char *timestamp,
    double temperature_c,
    double high_temperature_c,
    bool hot_profile_enabled,
    int camera_width,
    int camera_height,
    int camera_input_fps,
    double vision_output_fps,
    char *error,
    size_t error_size) {
    char subject[SG_TEXT_SIZE + 128];
    char body[1024];
    const char *event_name = hot_profile_enabled
        ? "THERMAL LIMIT ENABLED"
        : "THERMAL LIMIT CLEARED";
    int length;

    if (config == NULL || timestamp == NULL) {
        set_error(error, error_size, "thermal email input is null");
        return -1;
    }

    length = snprintf(
        subject,
        sizeof(subject),
        "%s: %s",
        config->email_subject_prefix,
        event_name);
    if (length < 0 || (size_t) length >= sizeof(subject)) {
        set_error(error, error_size, "thermal email subject is too long");
        return -1;
    }

    length = snprintf(
        body,
        sizeof(body),
        "Smart Guard adaptive thermal management\n\n"
        "Student ID: %s\n"
        "Event: %s\n"
        "Event timestamp: %s\n"
        "Current CPU temperature: %.1f C\n"
        "High-temperature threshold: %.1f C\n"
        "Active camera resolution: %dx%d\n"
        "Active camera input FPS: %d\n"
        "Active vision output FPS: %.1f\n"
        "Action: %s\n",
        config->student_id,
        event_name,
        timestamp,
        temperature_c,
        high_temperature_c,
        camera_width,
        camera_height,
        camera_input_fps,
        vision_output_fps,
        hot_profile_enabled
            ? "Reduced camera load and restarted the vision service."
            : "Restored the configured normal profile and restarted the vision service.");
    if (length < 0 || (size_t) length >= sizeof(body)) {
        set_error(error, error_size, "thermal email body is too long");
        return -1;
    }

    return send_email_message(
        config,
        subject,
        body,
        NULL,
        NULL,
        error,
        error_size);
}
