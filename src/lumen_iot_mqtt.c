#include <string.h>
#include <stdbool.h>

#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

#include "LumenProtocol.h"
#include "lumen_iot_wifi.h"
#include "lumen_iot_internal.h"

#define LUMEN_IOT_MQTT_MAX_BINDINGS 10
#define LUMEN_IOT_MQTT_TOPIC_MAX_LEN 64
#define LUMEN_IOT_MQTT_JSON_PATH_MAX_LEN 64
#define LUMEN_IOT_MQTT_BODY_BUFFER_SIZE 64
#define LUMEN_IOT_MQTT_JSON_BUFFER_SIZE 256

static const char *TAG = "lumen_iot_mqtt";

typedef struct
{
    char topic[LUMEN_IOT_MQTT_TOPIC_MAX_LEN];
    char json_path[LUMEN_IOT_MQTT_JSON_PATH_MAX_LEN];
    uint16_t address;
    lumen_data_type_t type;
    bool in_use;
} lumen_iot_mqtt_binding_t;

static esp_mqtt_client_handle_t s_client;
static lumen_iot_mqtt_binding_t s_bindings[LUMEN_IOT_MQTT_MAX_BINDINGS];

static void lumen_iot_mqtt_apply_raw_payload(uint16_t address, lumen_data_type_t type, const char *payload, int payload_len)
{
    lumen_packet_t packet = {address, type};

    if (type == kString)
    {
        int copy_len = payload_len;
        if (copy_len > MAX_STRING_SIZE - 1)
        {
            copy_len = MAX_STRING_SIZE - 1;
        }

        memcpy(packet.data._string, payload, copy_len);
        packet.data._string[copy_len] = '\0';
        lumen_write_packet(&packet);
        return;
    }

    size_t expected_size = lumen_iot_type_size(type);
    if (expected_size == 0 || (size_t)payload_len < expected_size)
    {
        ESP_LOGE(TAG, "payload too small for type: got %d bytes, expected %u", payload_len, (unsigned)expected_size);
        return;
    }

    memcpy(&packet.data, payload, expected_size);
    lumen_write_packet(&packet);
}

static void lumen_iot_mqtt_apply_json_payload(uint16_t address, const char *json_path, lumen_data_type_t type, const char *payload, int payload_len)
{
    char json_buffer[LUMEN_IOT_MQTT_JSON_BUFFER_SIZE];

    int copy_len = payload_len;
    if (copy_len > (int)(sizeof(json_buffer) - 1))
    {
        copy_len = (int)(sizeof(json_buffer) - 1);
    }

    memcpy(json_buffer, payload, copy_len);
    json_buffer[copy_len] = '\0';

    lumen_packet_t packet = {address, type};
    if (lumen_iot_extract_json_value(json_buffer, json_path, &packet) == ESP_OK)
    {
        lumen_write_packet(&packet);
    }
}

static void lumen_iot_mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    if (event_id == MQTT_EVENT_CONNECTED)
    {
        for (int i = 0; i < LUMEN_IOT_MQTT_MAX_BINDINGS; i++)
        {
            if (s_bindings[i].in_use)
            {
                esp_mqtt_client_subscribe(s_client, s_bindings[i].topic, 1);
            }
        }
    }
    else if (event_id == MQTT_EVENT_DATA)
    {
        if (lumen_iot_is_ota_in_progress())
        {
            return;
        }

        for (int i = 0; i < LUMEN_IOT_MQTT_MAX_BINDINGS; i++)
        {
            if (!s_bindings[i].in_use)
            {
                continue;
            }

            size_t binding_topic_len = strlen(s_bindings[i].topic);
            if ((int)binding_topic_len != event->topic_len)
            {
                continue;
            }

            if (strncmp(s_bindings[i].topic, event->topic, event->topic_len) != 0)
            {
                continue;
            }

            if (s_bindings[i].json_path[0] != '\0')
            {
                lumen_iot_mqtt_apply_json_payload(s_bindings[i].address, s_bindings[i].json_path, s_bindings[i].type, event->data, event->data_len);
            }
            else
            {
                lumen_iot_mqtt_apply_raw_payload(s_bindings[i].address, s_bindings[i].type, event->data, event->data_len);
            }
            break;
        }
    }
    else if (event_id == MQTT_EVENT_ERROR)
    {
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
    }
}

esp_err_t lumen_iot_mqtt_init(const char *uri, const char *ca_certificate_pem, const char *username, const char *password)
{
    if (uri == NULL || uri[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_mqtt_client_config_t config = {
        .broker.address.uri = uri,
        .network.timeout_ms = 10000,
    };

    if (ca_certificate_pem != NULL)
    {
        config.broker.verification.skip_cert_common_name_check = true;
        config.broker.verification.certificate = ca_certificate_pem;
    }
    else
    {
        config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }

    if (username != NULL)
    {
        config.credentials.username = username;
    }

    if (password != NULL)
    {
        config.credentials.authentication.password = password;
    }

    s_client = esp_mqtt_client_init(&config);
    if (s_client == NULL)
    {
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, lumen_iot_mqtt_event_handler, NULL);
    if (err != ESP_OK)
    {
        return err;
    }

    return esp_mqtt_client_start(s_client);
}

static esp_err_t lumen_iot_mqtt_add_binding(const char *topic, const char *json_path, uint16_t address, lumen_data_type_t type)
{
    for (int i = 0; i < LUMEN_IOT_MQTT_MAX_BINDINGS; i++)
    {
        if (!s_bindings[i].in_use)
        {
            strlcpy(s_bindings[i].topic, topic, sizeof(s_bindings[i].topic));

            if (json_path != NULL)
            {
                strlcpy(s_bindings[i].json_path, json_path, sizeof(s_bindings[i].json_path));
            }
            else
            {
                s_bindings[i].json_path[0] = '\0';
            }

            s_bindings[i].address = address;
            s_bindings[i].type = type;
            s_bindings[i].in_use = true;

            if (s_client != NULL)
            {
                esp_mqtt_client_subscribe(s_client, (char *)topic, 1);
            }

            return ESP_OK;
        }
    }

    return ESP_ERR_NO_MEM;
}

esp_err_t lumen_iot_mqtt_bind_topic_to_display(char *topic, uint16_t address, lumen_data_type_t type)
{
    if (topic == NULL || topic[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    return lumen_iot_mqtt_add_binding(topic, NULL, address, type);
}

esp_err_t lumen_iot_mqtt_bind_topic_json_to_display(char *topic, const char *json_path, uint16_t address, lumen_data_type_t type)
{
    if (topic == NULL || topic[0] == '\0' || json_path == NULL || json_path[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    return lumen_iot_mqtt_add_binding(topic, json_path, address, type);
}

esp_err_t lumen_iot_mqtt_publish(char *topic, lumen_data_type_t type, lumen_data_t data)
{
    if (s_client == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (topic == NULL || topic[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    char body_buffer[LUMEN_IOT_MQTT_BODY_BUFFER_SIZE];
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

    int message_id = esp_mqtt_client_publish(s_client, (char *)topic, body_buffer, (int)body_len, 1, 0);
    if (message_id >= 0)
    {
        return ESP_OK;
    }

    return ESP_FAIL;
}