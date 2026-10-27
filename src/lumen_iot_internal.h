#pragma once

#include <stddef.h>
#include "LumenProtocol.h"
#include "esp_http_client.h"

static inline size_t lumen_iot_type_size(lumen_data_type_t type)
{
    switch (type) {
        case kS8:
        case kU8:
        case kBool:
            return 1;
        case kS16:
        case kU16:
            return 2;
        case kS32:
        case kU32:
        case kFloat:
            return 4;
        case kDouble:
            return 8;
        default:
            return 0;
    }
}

void lumen_iot_net_apply_server_verification(esp_http_client_config_t *config);

void lumen_iot_set_ota_in_progress(bool value);

void lumen_iot_net_init(void);

esp_err_t lumen_iot_extract_json_value(const char *json_text, const char *json_path, lumen_packet_t *packet);

void lumen_iot_wifi_set_last_error(const char *message);
bool lumen_iot_wifi_consume_pending_error(char *out_buffer, size_t out_size);
void lumen_iot_wifi_clear_last_error(void);