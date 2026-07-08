#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#define RENDER_API_BASE_URL "https://api.koiosdigital.net"

    void render_fetch_init();
    void render_fetch_request(const uint8_t* uuid16);

#ifdef __cplusplus
}
#endif
