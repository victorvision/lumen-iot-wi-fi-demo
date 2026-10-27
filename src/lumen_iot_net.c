#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"

#include "LumenProtocol.h"
#include "lumen_iot_wifi.h"
#include "lumen_iot_internal.h"

#define LUMEN_IOT_NET_HTTP_BUFFER_SIZE 1024
#define LUMEN_IOT_NET_BODY_BUFFER_SIZE 64
#define LUMEN_IOT_NET_JSON_BODY_BUFFER_SIZE 96
#define LUMEN_IOT_NET_JSON_PATH_MAX_LEN 128

static const char *TAG = "lumen_iot_net";

static char s_http_response_buffer[LUMEN_IOT_NET_HTTP_BUFFER_SIZE];
static int s_http_response_length;

static SemaphoreHandle_t s_http_mutex = NULL;
static portMUX_TYPE s_http_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t lumen_iot_net_get_mutex(void)
{
    if (s_http_mutex == NULL)
    {
        portENTER_CRITICAL(&s_http_mutex_init_lock);
        if (s_http_mutex == NULL)
        {
            s_http_mutex = xSemaphoreCreateMutex();
        }
        portEXIT_CRITICAL(&s_http_mutex_init_lock);
    }
    return s_http_mutex;
}

void lumen_iot_net_init(void)
{
    lumen_iot_net_get_mutex();
}

static esp_err_t lumen_iot_net_check_http_status(esp_http_client_handle_t client, const char *context)
{
    int status_code = esp_http_client_get_status_code(client);
    if (status_code < 200 || status_code >= 300)
    {
        ESP_LOGE(TAG, "%s: unexpected status code %d", context, status_code);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t lumen_iot_net_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA)
    {
        int copy_length = evt->data_len;
        if (s_http_response_length + copy_length >= LUMEN_IOT_NET_HTTP_BUFFER_SIZE)
        {
            copy_length = LUMEN_IOT_NET_HTTP_BUFFER_SIZE - s_http_response_length - 1;
        }

        if (copy_length > 0)
        {
            memcpy(s_http_response_buffer + s_http_response_length, evt->data, copy_length);
            s_http_response_length += copy_length;
            s_http_response_buffer[s_http_response_length] = '\0';
        }
    }

    return ESP_OK;
}

static esp_err_t lumen_iot_net_fill_packet_raw(lumen_packet_t *packet)
{
    if (packet->type == kString)
    {
        int copy_len = s_http_response_length;
        if (copy_len > MAX_STRING_SIZE - 1)
        {
            copy_len = MAX_STRING_SIZE - 1;
        }

        memcpy(packet->data._string, s_http_response_buffer, copy_len);
        packet->data._string[copy_len] = '\0';
        return ESP_OK;
    }

    size_t expected_size = lumen_iot_type_size(packet->type);
    if (expected_size == 0 || (size_t)s_http_response_length < expected_size)
    {
        ESP_LOGE(TAG, "response too small for type: got %d bytes, expected %u", s_http_response_length, (unsigned)expected_size);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(&packet->data, s_http_response_buffer, expected_size);
    return ESP_OK;
}

esp_err_t lumen_iot_extract_json_value(const char *json_text, const char *json_path, lumen_packet_t *packet)
{
    cJSON *root = cJSON_Parse(json_text);
    if (root == NULL)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    char path_copy[LUMEN_IOT_NET_JSON_PATH_MAX_LEN];
    strlcpy(path_copy, json_path, sizeof(path_copy));

    cJSON *current = root;
    char *token = strtok(path_copy, ".");
    while (token != NULL && current != NULL)
    {
        current = cJSON_GetObjectItemCaseSensitive(current, token);
        token = strtok(NULL, ".");
    }

    esp_err_t err = ESP_OK;

    if (current == NULL)
    {
        err = ESP_ERR_NOT_FOUND;
    }
    else if (packet->type == kString)
    {
        if (cJSON_IsString(current))
        {
            strlcpy(packet->data._string, current->valuestring, MAX_STRING_SIZE);
        }
        else if (cJSON_IsNumber(current))
        {
            char number_buffer[32];
            snprintf(number_buffer, sizeof(number_buffer), "%g", current->valuedouble);
            strlcpy(packet->data._string, number_buffer, MAX_STRING_SIZE);
        }
        else
        {
            err = ESP_ERR_INVALID_ARG;
        }
    }
    else if (packet->type == kBool)
    {
        if (cJSON_IsBool(current))
        {
            packet->data._bool = cJSON_IsTrue(current);
        }
        else
        {
            err = ESP_ERR_INVALID_ARG;
        }
    }
    else if (cJSON_IsNumber(current))
    {
        double value = current->valuedouble;

        switch (packet->type)
        {
        case kFloat:
            packet->data._float = (float)value;
            break;
        case kDouble:
            packet->data._double = value;
            break;
        case kS32:
            packet->data._s32 = (int32_t)value;
            break;
        case kU32:
            packet->data._u32 = (uint32_t)value;
            break;
        case kS16:
            packet->data._s16 = (int16_t)value;
            break;
        case kU16:
            packet->data._u16 = (uint16_t)value;
            break;
        case kS8:
            packet->data._s8 = (int8_t)value;
            break;
        case kU8:
            packet->data._u8 = (uint8_t)value;
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
        }
    }
    else
    {
        err = ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(root);
    return err;
}

static esp_err_t lumen_iot_net_perform_get(const char *url)
{
    s_http_response_length = 0;
    s_http_response_buffer[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = lumen_iot_net_http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        err = lumen_iot_net_check_http_status(client, "GET");
    }

    esp_http_client_cleanup(client);
    return err;
}

esp_err_t lumen_iot_net_https_get(const char *url, uint16_t address, lumen_data_type_t type)
{
    if (url == NULL || url[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (lumen_iot_is_ota_in_progress())
    {
        return ESP_ERR_INVALID_STATE;
    }

    SemaphoreHandle_t mutex = lumen_iot_net_get_mutex();
    xSemaphoreTake(mutex, portMAX_DELAY);

    esp_err_t err = lumen_iot_net_perform_get(url);
    if (err == ESP_OK)
    {
        lumen_packet_t packet = {address, type};
        err = lumen_iot_net_fill_packet_raw(&packet);
        if (err == ESP_OK)
        {
            lumen_write_packet(&packet);
        }
    }

    xSemaphoreGive(mutex);
    return err;
}

esp_err_t lumen_iot_net_https_get_json(const char *url, const char *json_path, uint16_t address, lumen_data_type_t type)
{
    if (url == NULL || url[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (lumen_iot_is_ota_in_progress())
    {
        return ESP_ERR_INVALID_STATE;
    }

    SemaphoreHandle_t mutex = lumen_iot_net_get_mutex();
    xSemaphoreTake(mutex, portMAX_DELAY);

    esp_err_t err = lumen_iot_net_perform_get(url);
    if (err == ESP_OK)
    {
        lumen_packet_t packet = {address, type};
        err = lumen_iot_extract_json_value(s_http_response_buffer, json_path, &packet);
        if (err == ESP_OK)
        {
            lumen_write_packet(&packet);
        }
    }

    xSemaphoreGive(mutex);
    return err;
}

esp_err_t lumen_iot_net_https_post(const char *url, lumen_data_type_t type, lumen_data_t data)
{
    if (url == NULL || url[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (lumen_iot_is_ota_in_progress())
    {
        return ESP_ERR_INVALID_STATE;
    }

    char body_buffer[LUMEN_IOT_NET_BODY_BUFFER_SIZE];
    size_t body_len;

    if (type == kString)
    {
        body_len = strnlen(data._string, MAX_STRING_SIZE);
        memcpy(body_buffer, data._string, body_len);
    }
    else
    {
        body_len = lumen_iot_type_size(type);
        if (body_len == 0 || body_len > sizeof(body_buffer))
        {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(body_buffer, &data, body_len);
    }

    SemaphoreHandle_t mutex = lumen_iot_net_get_mutex();
    xSemaphoreTake(mutex, portMAX_DELAY);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_post_field(client, body_buffer, body_len);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        err = lumen_iot_net_check_http_status(client, "POST");
    }

    esp_http_client_cleanup(client);
    xSemaphoreGive(mutex);
    return err;
}

static void lumen_iot_net_json_escape(const char *input, char *output, size_t output_size)
{
    size_t out_index = 0;

    for (size_t i = 0; input[i] != '\0' && out_index < output_size - 1; i++)
    {
        char c = input[i];

        if (c == '"' || c == '\\')
        {
            if (out_index >= output_size - 2)
            {
                break;
            }

            output[out_index] = '\\';
            out_index++;
        }

        output[out_index] = c;
        out_index++;
    }

    output[out_index] = '\0';
}

static int lumen_iot_net_format_json_body(const char *json_field, lumen_data_type_t type, lumen_data_t data, char *buffer, size_t buffer_size)
{
    if (type == kString)
    {
        char escaped[MAX_STRING_SIZE * 2];
        lumen_iot_net_json_escape(data._string, escaped, sizeof(escaped));
        return snprintf(buffer, buffer_size, "{\"%s\":\"%s\"}", json_field, escaped);
    }

    switch (type)
    {
    case kBool:
        if (data._bool)
        {
            return snprintf(buffer, buffer_size, "{\"%s\":true}", json_field);
        }
        return snprintf(buffer, buffer_size, "{\"%s\":false}", json_field);
    case kFloat:
        return snprintf(buffer, buffer_size, "{\"%s\":%g}", json_field, (double)data._float);
    case kDouble:
        return snprintf(buffer, buffer_size, "{\"%s\":%g}", json_field, data._double);
    case kS32:
        return snprintf(buffer, buffer_size, "{\"%s\":%ld}", json_field, (long)data._s32);
    case kU32:
        return snprintf(buffer, buffer_size, "{\"%s\":%lu}", json_field, (unsigned long)data._u32);
    case kS16:
        return snprintf(buffer, buffer_size, "{\"%s\":%d}", json_field, data._s16);
    case kU16:
        return snprintf(buffer, buffer_size, "{\"%s\":%u}", json_field, data._u16);
    case kS8:
        return snprintf(buffer, buffer_size, "{\"%s\":%d}", json_field, data._s8);
    case kU8:
        return snprintf(buffer, buffer_size, "{\"%s\":%u}", json_field, data._u8);
    default:
        return -1;
    }
}

esp_err_t lumen_iot_net_https_post_json(const char *url, const char *json_field, lumen_data_type_t type, lumen_data_t data)
{
    if (url == NULL || url[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (lumen_iot_is_ota_in_progress())
    {
        return ESP_ERR_INVALID_STATE;
    }

    char body_buffer[LUMEN_IOT_NET_JSON_BODY_BUFFER_SIZE];
    int body_len = lumen_iot_net_format_json_body(json_field, type, data, body_buffer, sizeof(body_buffer));

    if (body_len <= 0 || (size_t)body_len >= sizeof(body_buffer))
    {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t mutex = lumen_iot_net_get_mutex();
    xSemaphoreTake(mutex, portMAX_DELAY);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body_buffer, body_len);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        err = lumen_iot_net_check_http_status(client, "POST_JSON");
    }

    esp_http_client_cleanup(client);
    xSemaphoreGive(mutex);
    return err;
}

void lumen_iot_net_apply_server_verification(esp_http_client_config_t *config)
{
    config->crt_bundle_attach = esp_crt_bundle_attach;
}