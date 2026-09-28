#include "videos_routes.h"
#include "domain/videos_domain.h"

#include "authorization.h"

cws_router_t* videos_routes(void) {
    cws_router_t* r = cws_router_new();
    if (!r) return NULL;

    cws_router_add(r, CWS_M_GET, "/thumbnails/:name", get_thumbnail);
    cws_router_add(r, CWS_M_GET, "/",              get_all_videos);
    cws_router_add(r, CWS_M_GET, "/popular",       get_popular_videos);
    cws_router_add(r, CWS_M_PUT,  "/:id/views",    insert_view);
    cws_router_add(r, CWS_M_GET,  "/:id/series/relacionados",
                   get_series_videos);
    cws_router_add(r, CWS_M_GET, "/:id",           get_video_by_id);

    /* POST /external — registro de video externo (URL m3u8 + miniatura).
     * Ruta protegida (cookie access_token + x-csrf-token + IP) vía el
     * middleware web de autorización. */
    cws_router_t* prot = cws_router_new();
    if (prot) {
        cws_router_use(prot, cws_mw_authorization_verify);
        cws_router_add(prot, CWS_M_POST, "/", insert_external_video);
        cws_router_mount(r, "/external", prot);
    }

    return r;
}