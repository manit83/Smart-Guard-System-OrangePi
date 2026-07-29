#ifndef TEST_STUB_CURL_H
#define TEST_STUB_CURL_H

#include <stddef.h>

typedef void CURL;
typedef void curl_mime;
typedef void curl_mimepart;
typedef int CURLcode;

struct curl_slist {
    char *data;
    struct curl_slist *next;
};

#define CURLE_OK 0
#define CURL_GLOBAL_DEFAULT 3L
#define CURL_ERROR_SIZE 256
#define CURL_ZERO_TERMINATED ((size_t) -1)
#define CURLUSESSL_NONE 0
#define CURLUSESSL_ALL 3

#define CURLOPT_URL 1
#define CURLOPT_USERNAME 2
#define CURLOPT_PASSWORD 3
#define CURLOPT_MAIL_FROM 4
#define CURLOPT_MAIL_RCPT 5
#define CURLOPT_HTTPHEADER 6
#define CURLOPT_MIMEPOST 7
#define CURLOPT_CONNECTTIMEOUT 8
#define CURLOPT_TIMEOUT 9
#define CURLOPT_NOSIGNAL 10
#define CURLOPT_SSL_VERIFYPEER 11
#define CURLOPT_SSL_VERIFYHOST 12
#define CURLOPT_ERRORBUFFER 13
#define CURLOPT_USE_SSL 14
#define CURLOPT_CAINFO 15

CURLcode curl_global_init(long flags);
void curl_global_cleanup(void);
CURL *curl_easy_init(void);
void curl_easy_cleanup(CURL *curl);
CURLcode curl_easy_setopt(CURL *curl, int option, ...);
CURLcode curl_easy_perform(CURL *curl);
const char *curl_easy_strerror(CURLcode code);

struct curl_slist *curl_slist_append(
    struct curl_slist *list,
    const char *text);
void curl_slist_free_all(struct curl_slist *list);

curl_mime *curl_mime_init(CURL *curl);
void curl_mime_free(curl_mime *mime);
curl_mimepart *curl_mime_addpart(curl_mime *mime);
CURLcode curl_mime_data(
    curl_mimepart *part,
    const char *data,
    size_t data_size);
CURLcode curl_mime_type(curl_mimepart *part, const char *mime_type);
CURLcode curl_mime_filedata(curl_mimepart *part, const char *file_name);
CURLcode curl_mime_filename(curl_mimepart *part, const char *file_name);
CURLcode curl_mime_encoder(curl_mimepart *part, const char *encoding);

#endif
