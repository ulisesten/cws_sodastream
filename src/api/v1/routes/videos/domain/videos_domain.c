#include "videos_domain.h"
#include <stdio.h>
#include <string.h>

void get_all_videos(cws_request_t* req, cws_response_t* res) {
    (void)req;
    const char* body = "{\"videos\":[]}";
    cws_response_body(res, body, strlen(body), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

void get_video_by_id(cws_request_t* req, cws_response_t* res) {
    const char* id = cws_request_param(req, "id");
    char body[256];
    int n = snprintf(body, sizeof(body),
                     "{\"id\":\"%s\",\"title\":\"Sample Video\"}",
                     id ? id : "unknown");
    if (n < 0) n = 0;
    cws_response_body(res, body, (size_t)n, CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}