#include <string.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "LumenProtocol.h"
#include "lumen_iot_wifi.h"
#include "lumen_iot_internal.h"

#define LUMEN_IOT_WIFI_ERROR_MAX_LEN 64

static const char *TAG = "lumen_iot_core";

static uart_port_t s_uart_num = UART_NUM_1;
static bool s_sntp_initialized = false;
static volatile bool s_has_ip = false;
static volatile bool s_ota_in_progress = false;

static char s_last_wifi_error[LUMEN_IOT_WIFI_ERROR_MAX_LEN] = {0};
static volatile bool s_wifi_error_pending = false;

void lumen_write_bytes(uint8_t *data, uint32_t length)
{
    uart_write_bytes(s_uart_num, data, length);
}

uint16_t lumen_get_byte(void)
{
    size_t available_bytes;
    uart_get_buffered_data_len(s_uart_num, &available_bytes);

    if (available_bytes > 0)
    {
        uint8_t received_byte;
        uart_read_bytes(s_uart_num, &received_byte, 1, 0);
        return received_byte;
    }

    return DATA_NULL;
}

static const char *lumen_iot_wifi_reason_to_message(uint8_t reason)
{
    switch (reason)
    {
    case WIFI_REASON_NO_AP_FOUND:
        return "Network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "Incorrect password";
    case WIFI_REASON_ASSOC_FAIL:
        return "Failed to associate with the network";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "Signal lost (out of range)";
    case WIFI_REASON_CONNECTION_FAIL:
        return "Connection failure";
    default:
        return NULL;
    }
}

void lumen_iot_wifi_set_last_error(const char *message)
{
    strlcpy(s_last_wifi_error, message, sizeof(s_last_wifi_error));
    s_wifi_error_pending = true;
}

void lumen_iot_wifi_clear_last_error(void)
{
    if (s_last_wifi_error[0] != '\0')
    {
        s_last_wifi_error[0] = '\0';
        s_wifi_error_pending = true;
    }
}

bool lumen_iot_wifi_consume_pending_error(char *out_buffer, size_t out_size)
{
    if (!s_wifi_error_pending)
    {
        return false;
    }

    strlcpy(out_buffer, s_last_wifi_error, out_size);
    s_wifi_error_pending = false;
    return true;
}

static void lumen_iot_on_wifi_disconnected(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;

    s_has_ip = false;

    wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
    const char *message = lumen_iot_wifi_reason_to_message(event->reason);

    if (message != NULL)
    {
        ESP_LOGW(TAG, "wifi disconnected, reason=%d (%s)", event->reason, message);
        lumen_iot_wifi_set_last_error(message);
    }
    else
    {
        ESP_LOGI(TAG, "wifi disconnected, reason=%d", event->reason);
    }
}

static void lumen_iot_on_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;

    s_has_ip = true;
    lumen_iot_wifi_clear_last_error();

    if (!s_sntp_initialized)
    {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&config);
        s_sntp_initialized = true;
    }
}

bool lumen_iot_wifi_has_ip(void)
{
    return s_has_ip;
}

void lumen_iot_set_ota_in_progress(bool value)
{
    s_ota_in_progress = value;
}

bool lumen_iot_is_ota_in_progress(void)
{
    return s_ota_in_progress;
}

esp_err_t lumen_iot_wifi_init(const lumen_iot_uart_config_t *uart_config)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        err = nvs_flash_erase();
        if (err != ESP_OK)
        {
            return err;
        }

        err = nvs_flash_init();
    }

    if (err != ESP_OK)
    {
        return err;
    }

    s_uart_num = uart_config->uart_num;

    uart_config_t uart_conf = {
        .baud_rate = uart_config->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    err = uart_param_config(s_uart_num, &uart_conf);
    if (err != ESP_OK)
    {
        return err;
    }

    err = uart_set_pin(s_uart_num, uart_config->tx_pin, uart_config->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK)
    {
        return err;
    }

    err = uart_driver_install(s_uart_num, uart_config->rx_buffer_size, 0, 0, NULL, 0);
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &lumen_iot_on_got_ip, NULL, NULL);
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &lumen_iot_on_wifi_disconnected, NULL, NULL);
    if (err != ESP_OK)
    {
        return err;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_init_cfg);
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        return err;
    }

    esp_wifi_set_ps(WIFI_PS_NONE);
    lumen_iot_net_init();
    return ESP_OK;
}

esp_err_t lumen_iot_set_uart_baudrate(int baud_rate)
{
    esp_err_t err = uart_set_baudrate(s_uart_num, baud_rate);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "uart_set_baudrate failed: %s", esp_err_to_name(err));
        return err;
    }

    return uart_flush(s_uart_num);
}