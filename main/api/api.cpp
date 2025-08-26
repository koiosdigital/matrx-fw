#include "api.h"

#include "mdns.h"
#include "esp_http_server.h"
#include "kd_common.h"
#include "cJSON.h"
#include <esp_app_desc.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "static_files.h"
#include "config.h"

/* Empty handle to esp_http_server */
httpd_handle_t kd_server = NULL;

httpd_handle_t get_httpd_handle() {
    return kd_server;
}

void init_mdns() {
    mdns_init();
    const char* hostname = kd_common_get_wifi_hostname();
    mdns_hostname_set(hostname);

    //esp_app_desc
    const esp_app_desc_t* app_desc = esp_app_get_description();

    mdns_txt_item_t serviceTxtData[4] = {
        {"model", FIRMWARE_VARIANT},
        {"type", "matrx"},
        { "version", app_desc->version }
    };

    ESP_ERROR_CHECK(mdns_service_add(NULL, "_koiosdigital", "_tcp", 80, serviceTxtData, 4));
}

void server_init() {
    /* Generate default configuration */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 50;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_start(&kd_server, &config);
}

esp_err_t root_handler(httpd_req_t* req) {
    const char* response = "Welcome to the KD Matrix API!";
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

esp_err_t about_handler(httpd_req_t* req) {
    const esp_app_desc_t* app_desc = esp_app_get_description();

    // Create JSON response
    cJSON* json = cJSON_CreateObject();
    if (json == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON* model = cJSON_CreateString(FIRMWARE_VARIANT);
    cJSON* type = cJSON_CreateString("matrx");
    cJSON* version = cJSON_CreateString(app_desc->version);

    cJSON_AddItemToObject(json, "model", model);
    cJSON_AddItemToObject(json, "type", type);
    cJSON_AddItemToObject(json, "version", version);

    char* json_string = cJSON_Print(json);
    if (json_string == NULL) {
        cJSON_Delete(json);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_string, strlen(json_string));

    free(json_string);
    cJSON_Delete(json);

    return ESP_OK;
}

esp_err_t system_config_get_handler(httpd_req_t* req) {
    system_config_t system_config = config_get_system_config();

    // Create JSON response
    cJSON* json = cJSON_CreateObject();
    if (json == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Add screen configuration fields
    cJSON* screen_enabled = cJSON_CreateBool(system_config.screen_enabled);
    cJSON* screen_brightness = cJSON_CreateNumber(system_config.screen_brightness);
    cJSON* auto_brightness_enabled = cJSON_CreateBool(system_config.auto_brightness_enabled);

    cJSON_AddItemToObject(json, "screen_enabled", screen_enabled);
    cJSON_AddItemToObject(json, "screen_brightness", screen_brightness);
    cJSON_AddItemToObject(json, "auto_brightness_enabled", auto_brightness_enabled);

    char* json_string = cJSON_Print(json);
    if (json_string == NULL) {
        cJSON_Delete(json);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_string, strlen(json_string));

    free(json_string);
    cJSON_Delete(json);

    return ESP_OK;
}

esp_err_t system_config_post_handler(httpd_req_t* req) {
    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        else {
            httpd_resp_send_500(req);
        }
        return ESP_FAIL;
    }
    content[ret] = '\0';

    // Parse JSON
    cJSON* json = cJSON_Parse(content);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON format");
        return ESP_FAIL;
    }

    // Get current system config as starting point
    system_config_t new_system_config = config_get_system_config();

    // Validate and extract fields
    cJSON* screen_enabled_json = cJSON_GetObjectItem(json, "screen_enabled");
    cJSON* screen_brightness_json = cJSON_GetObjectItem(json, "screen_brightness");
    cJSON* auto_brightness_enabled_json = cJSON_GetObjectItem(json, "auto_brightness_enabled");

    // Track which system config fields need updating
    bool update_screen_enabled = false;
    bool update_brightness = false;
    bool update_auto_brightness = false;

    // Validate screen_enabled if present
    if (cJSON_IsBool(screen_enabled_json)) {
        new_system_config.screen_enabled = cJSON_IsTrue(screen_enabled_json);
        update_screen_enabled = true;
    }

    // Validate screen_brightness if present
    if (cJSON_IsNumber(screen_brightness_json)) {
        double brightness_val = cJSON_GetNumberValue(screen_brightness_json);
        if (brightness_val >= 0 && brightness_val <= 255) {
            new_system_config.screen_brightness = (uint8_t)brightness_val;
            update_brightness = true;
        }
    }

    // Validate auto_brightness_enabled if present
    if (cJSON_IsBool(auto_brightness_enabled_json)) {
        new_system_config.auto_brightness_enabled = cJSON_IsTrue(auto_brightness_enabled_json);
        update_auto_brightness = true;
    }

    cJSON_Delete(json);

    // Update system configuration (only update fields that were provided)
    if (update_screen_enabled || update_brightness || update_auto_brightness) {
        esp_err_t config_ret = config_update_system_config(&new_system_config,
            update_screen_enabled,
            update_brightness,
            update_auto_brightness);
        if (config_ret != ESP_OK) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    // Create response with the complete current configuration
    system_config_t current_system_config = config_get_system_config();

    cJSON* response_json = cJSON_CreateObject();
    if (response_json == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Add all configuration fields to response
    cJSON_AddItemToObject(response_json, "screen_enabled", cJSON_CreateBool(current_system_config.screen_enabled));
    cJSON_AddItemToObject(response_json, "screen_brightness", cJSON_CreateNumber(current_system_config.screen_brightness));
    cJSON_AddItemToObject(response_json, "auto_brightness_enabled", cJSON_CreateBool(current_system_config.auto_brightness_enabled));

    char* response_string = cJSON_Print(response_json);
    if (response_string == NULL) {
        cJSON_Delete(response_json);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response_string, strlen(response_string));

    free(response_string);
    cJSON_Delete(response_json);

    return ESP_OK;
}

void api_init() {
    init_mdns();
    server_init();

    httpd_handle_t server = get_httpd_handle();

    httpd_uri_t about_uri = {
        .uri = "/api/about",
        .method = HTTP_GET,
        .handler = about_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &about_uri);

    httpd_uri_t system_config_get_uri = {
        .uri = "/api/system/config",
        .method = HTTP_GET,
        .handler = system_config_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &system_config_get_uri);

    httpd_uri_t system_config_post_uri = {
        .uri = "/api/system/config",
        .method = HTTP_POST,
        .handler = system_config_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &system_config_post_uri);
}