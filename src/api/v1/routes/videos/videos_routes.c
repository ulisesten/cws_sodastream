#include "videos_routes.h"
#include "domain/videos_domain.h"

cws_router_t* videos_routes(void) {
    cws_router_t* r = cws_router_new();
    if (!r) return NULL;

    cws_router_add(r, CWS_M_GET, "/",              get_all_videos);
    cws_router_add(r, CWS_M_GET, "/popular",       get_popular_videos);
    cws_router_add(r, CWS_M_GET, "/:id",           get_video_by_id);

    return r;
}