#ifndef VIDEOS_DOMAIN_H
#define VIDEOS_DOMAIN_H

#include "cws/cws.h"

void get_all_videos(cws_request_t* req, cws_response_t* res);
void get_video_by_id(cws_request_t* req, cws_response_t* res);

#endif