#pragma once

#include "esp_http_server.h"

httpd_handle_t get_httpd_handle();

//renamed bc of conflict with esp-idf's mdns
void init_mdns();

void api_init();
esp_err_t about_handler(httpd_req_t* req);
esp_err_t system_config_get_handler(httpd_req_t* req);
esp_err_t system_config_post_handler(httpd_req_t* req);