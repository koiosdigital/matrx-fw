#ifndef CERTMGR_H
#define CERTMGR_H

#include <esp_err.h>

esp_err_t certmgr_handler(uint32_t session_id, const uint8_t* inbuf, ssize_t inlen, uint8_t** outbuf, ssize_t* outlen, void* priv_data);

#endif