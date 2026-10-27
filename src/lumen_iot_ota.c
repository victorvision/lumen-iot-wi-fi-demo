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
#define LUMEN_IOT_OTA_NEGOTIATION_TIMEOUT_MS 5000
#define LUMEN_IOT_OTA_SEND_TIMEOUT_MS 5000
#define LUMEN_IOT_OTA_FINISH_TIMEOUT_MS 5000

static const char *TAG = "lumen_iot_ota";

typedef bool (*lumen_iot_ota_send_fn)(uint8_t *data, uint32_t length);

static uint32_t lumen_iot_ota_baudrate_enum_to_uint32(lumen_baudrate_serial_update_t baudrate)
{
    switch (baudrate)
    {
    case k115200:
        return 115200;
    case k250000:
        return 250000;
    case k694444:
        return 694444;
    case k892857:
        return 892857;
    case k1250000:
        return 1250000;
    case k2083333:
        return 2083333;
    case k3125000:
        return 3125000;
    default:
        ESP_LOGE(TAG, "unknown baudrate enum (%d), falling back to 115200", (int)baudrate);
        return 115200;
    }
}

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

static esp_err_t lumen_iot_ota_negotiate_baudrate(lumen_baudrate_serial_update_t transfer_baudrate)
{
    int64_t last_tick_us = esp_timer_get_time();
    int retries = 0;
    int loop_count = 0;
    bool negotiated = false;

    while (!negotiated)
    {
        negotiated = lumen_project_and_firmware_update_set_new_baudrate(transfer_baudrate);
        if (negotiated)
        {
            break;
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us - last_tick_us >= (int64_t)LUMEN_IOT_OTA_TICK_INTERVAL_MS * 10000)
        {
            lumen_project_and_firmware_update_tick(LUMEN_IOT_OTA_TICK_INTERVAL_MS);
            last_tick_us = now_us;

            retries++;
            if (retries > 500)
            {
                ESP_LOGW(TAG, "waiting for ack baudrate, attempt %d", retries);
                return ESP_ERR_TIMEOUT;
            }
        }

        loop_count++;
        if (loop_count >= 50)
        {
            loop_count = 0;
            vTaskDelay(1);
        }
    }

    esp_err_t err = lumen_iot_set_uart_baudrate(lumen_iot_ota_baudrate_enum_to_uint32(transfer_baudrate));
    vTaskDelay(pdMS_TO_TICKS(1000));
    return err;
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

static esp_err_t lumen_iot_ota_finish_and_reset(uint32_t default_baudrate)
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

    esp_err_t err = lumen_iot_set_uart_baudrate(default_baudrate);
    vTaskDelay(pdMS_TO_TICKS(LUMEN_IOT_OTA_REBOOT_WAIT_MS));
    return err;
}

static esp_err_t lumen_iot_ota_stream_and_send(const char *url, lumen_iot_ota_send_fn send_fn)
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

    uint8_t buffer[LUMEN_IOT_OTA_BLOCK_SIZE];
    int64_t remaining = content_length;

    while (remaining > 0)
    {
        int to_read = LUMEN_IOT_OTA_BLOCK_SIZE;
        if (remaining < LUMEN_IOT_OTA_BLOCK_SIZE)
        {
            to_read = (int)remaining;
        }

        int read_len = esp_http_client_read(client, (char *)buffer, to_read);

        if (read_len <= 0)
        {
            ESP_LOGE(TAG, "esp_http_client_read failed: %d", read_len);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
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

static esp_err_t lumen_iot_ota_run(const char *url, lumen_iot_ota_send_fn send_fn, uint16_t reboot_address, uint32_t default_baudrate, lumen_baudrate_serial_update_t transfer_baudrate)
{
    if (lumen_iot_is_ota_in_progress())
    {
        ESP_LOGE(TAG, "update already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    lumen_iot_set_ota_in_progress(true);
    lumen_iot_ota_send_reboot_packet(reboot_address);

    esp_err_t err = lumen_iot_ota_negotiate_baudrate(transfer_baudrate);
    if (err == ESP_OK)
    {
        err = lumen_iot_ota_stream_and_send(url, send_fn);
        if (err == ESP_OK)
        {
            err = lumen_iot_ota_finish_and_reset(default_baudrate);
        }
        else
        {
            lumen_iot_set_uart_baudrate(default_baudrate);
        }
    }

    lumen_iot_set_ota_in_progress(false);
    return err;
}

esp_err_t lumen_iot_display_update_firmware(const char *url, uint16_t reboot_address, uint32_t default_baudrate, lumen_baudrate_serial_update_t transfer_baudrate)
{
    if (url == NULL || url[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    return lumen_iot_ota_run(url, lumen_firmware_update_send_data, reboot_address, default_baudrate, transfer_baudrate);
}

esp_err_t lumen_iot_display_update_project(const char *url, uint16_t reboot_address, uint32_t default_baudrate, lumen_baudrate_serial_update_t transfer_baudrate)
{
    if (url == NULL || url[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    return lumen_iot_ota_run(url, lumen_project_update_send_data, reboot_address, default_baudrate, transfer_baudrate);
}