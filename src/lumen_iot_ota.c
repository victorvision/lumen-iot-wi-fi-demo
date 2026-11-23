#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "LumenProtocol.h"
#include "lumen_iot_wifi.h"
#include "lumen_iot_internal.h"

#define LUMEN_IOT_OTA_BLOCK_SIZE 1660
#define LUMEN_IOT_OTA_TICK_INTERVAL_MS 10
#define LUMEN_IOT_OTA_REBOOT_WAIT_MS 2500
#define LUMEN_IOT_OTA_SEND_TIMEOUT_MS 5000
#define LUMEN_IOT_OTA_FINISH_TIMEOUT_MS 5000

static const char *TAG = "lumen_iot_ota";

typedef bool (*lumen_iot_ota_send_fn)(uint8_t *data, uint32_t length);

static void lumen_iot_ota_send_reboot_packet(uint16_t reboot_address)
{
    lumen_packet_t reboot_packet = {reboot_address, kBool};
    reboot_packet.data._bool = true;
    lumen_write_packet(&reboot_packet);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void lumen_iot_ota_tick_delay(void)
{
    lumen_project_and_firmware_update_tick(LUMEN_IOT_OTA_TICK_INTERVAL_MS);
    vTaskDelay(pdMS_TO_TICKS(LUMEN_IOT_OTA_TICK_INTERVAL_MS));
}

static esp_err_t lumen_iot_ota_send_block(lumen_iot_ota_send_fn send_fn, uint8_t *data, uint32_t length)
{
    int64_t start_us = esp_timer_get_time();

    while (!send_fn(data, length))
    {
        if ((esp_timer_get_time() - start_us) > ((int64_t)LUMEN_IOT_OTA_SEND_TIMEOUT_MS * 10000))
        {
            return ESP_ERR_TIMEOUT;
        }
        lumen_iot_ota_tick_delay();
    }

    return ESP_OK;
}

static esp_err_t lumen_iot_ota_finish_and_reset(void)
{
    int64_t start_us = esp_timer_get_time();

    while (!lumen_project_and_firmware_update_finish_and_reset())
    {
        if ((esp_timer_get_time() - start_us) > ((int64_t)LUMEN_IOT_OTA_FINISH_TIMEOUT_MS * 10000))
        {
            return ESP_ERR_TIMEOUT;
        }
        lumen_iot_ota_tick_delay();
    }
    vTaskDelay(pdMS_TO_TICKS(LUMEN_IOT_OTA_REBOOT_WAIT_MS));
    return ESP_OK;
}

static esp_err_t lumen_iot_ota_stream_and_send(const char *url, lumen_iot_ota_send_fn send_fn, uint16_t reboot_address)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 15000,
    };
    lumen_iot_net_apply_server_verification(&config);

    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_http_client_open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0)
    {
        ESP_LOGE(TAG, "invalid content length: %lld", content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code < 200 || status_code >= 300)
    {
        ESP_LOGE(TAG, "unexpected status code: %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    lumen_iot_ota_send_reboot_packet(reboot_address);

    uint8_t buffer[LUMEN_IOT_OTA_BLOCK_SIZE];
    int64_t remaining = content_length;

    while (remaining > 0)
    {
        int to_read = LUMEN_IOT_OTA_BLOCK_SIZE;
        if (remaining < LUMEN_IOT_OTA_BLOCK_SIZE)
        {
            to_read = (int)remaining;
        }
        int read_len = 0;
        while (read_len < to_read)
        {
            int n = esp_http_client_read(client, (char *)buffer + read_len, to_read - read_len);
            if (n <= 0)
            {
                ESP_LOGE(TAG, "esp_http_client_read failed: %d", n);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return ESP_FAIL;
            }
            read_len += n;
            lumen_iot_ota_tick_delay();
        }

        err = lumen_iot_ota_send_block(send_fn, buffer, (uint32_t)read_len);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "send_block timed out");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return err;
        }

        remaining -= read_len;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

static esp_err_t lumen_iot_ota_run(const char *url, lumen_iot_ota_send_fn send_fn, uint16_t reboot_address)
{
    if (lumen_iot_is_ota_in_progress())
    {
        ESP_LOGE(TAG, "update already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    lumen_iot_set_ota_in_progress(true);

    esp_err_t err = lumen_iot_ota_stream_and_send(url, send_fn, reboot_address);
    if (err == ESP_OK)
    {
        err = lumen_iot_ota_finish_and_reset();
    }

    lumen_iot_set_ota_in_progress(false);
    return err;
}

esp_err_t lumen_iot_display_update_firmware(const char *url, uint16_t reboot_address)
{
    if (url == NULL || url[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    return lumen_iot_ota_run(url, lumen_firmware_update_send_data, reboot_address);
}

esp_err_t lumen_iot_display_update_project(const char *url, uint16_t reboot_address)
{
    if (url == NULL || url[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    return lumen_iot_ota_run(url, lumen_project_update_send_data, reboot_address);
}