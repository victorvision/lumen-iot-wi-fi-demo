#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "LumenProtocol.h"
#include "lumen_iot_wifi.h"

#define WIFI_LIST_ADDRESS 200
#define WIFI_LIST_MAX_ITEMS 10
#define WIFI_RESCAN_ADDRESS 201
#define WIFI_LIST_INDEX_ADDRESS 202
#define WIFI_PASSWORD_ADDRESS 204
#define WIFI_LOGIN_BUTTON_ADDRESS 205
#define WIFI_CONNECTION_STATUS_ADDRESS 206
#define WIFI_SIGNAL_LEVEL_ADDRESS 208
#define TEMPERATURE_ADDRESS 209
#define TEST_POST_ADDRESS 211
#define TEST_NUMBER_POST_ADDRESS 214
#define TEST_MQTT_PUBLISH_ADDRESS 217
#define SENSOR_NAME_ADDRESS 218
#define SEND_PRJ_ADDRESS 219
#define SEND_FW_ADDRESS 220
#define SENSOR_TEMP_JSON_ADDRESS 221
#define PUBLIC_IP_ADDRESS 222
#define TEST_POST_JSON_ADDRESS 223
#define WIFI_MANUAL_SSID_ADDRESS 224
#define WIFI_FORGET_ADDRESS 226
#define WIFI_SSID_ADDRESS 227
#define WIFI_IP_ADDRESS 228
#define WIFI_ERROR_ADDRESS 229

#define WIFI_PASSWORD_SAVED_ADDRESS 233
#define WIFI_AUTO_CONNECT_TOGGLE_ADDRESS 231

#define WEATHER_URL "URL_HERE"
#define OTA_DEFAULT_BAUDRATE 115200
#define OTA_TRANSFER_BAUDRATE k892857
#define REBOOT_ADDRESS 102

static const char *s_ota_url = "URL_HERE";
static const char *s_ota_url_fw = "URL_HERE";

static volatile bool s_wifi_connected = false;

static bool s_mqtt_initialized = false;

static void send_project_task(void *pvParameters)
{
    esp_err_t err = lumen_iot_display_update_project(s_ota_url, REBOOT_ADDRESS, OTA_DEFAULT_BAUDRATE, OTA_TRANSFER_BAUDRATE);
    if (err != ESP_OK)
    {
        ESP_LOGE("ota", "Fail to send project: %s", esp_err_to_name(err));
    }
    lumen_iot_wifi_scan_to_list(WIFI_LIST_ADDRESS, WIFI_LIST_MAX_ITEMS);
    vTaskDelete(NULL);
}

static void send_firmware_task(void *pvParameters)
{
    esp_err_t err = lumen_iot_display_update_firmware(s_ota_url_fw, REBOOT_ADDRESS, OTA_DEFAULT_BAUDRATE, OTA_TRANSFER_BAUDRATE);
    if (err != ESP_OK)
    {
        ESP_LOGE("ota", "Fail to send firmware: %s", esp_err_to_name(err));
    }
    lumen_iot_wifi_scan_to_list(WIFI_LIST_ADDRESS, WIFI_LIST_MAX_ITEMS);
    vTaskDelete(NULL);
}

static void wifi_status_task(void *pvParameters)
{
    int32_t connection_state;
    int32_t signal_level;

    while (1)
    {
        if (!lumen_iot_is_ota_in_progress())
        {
            lumen_iot_wifi_update_status(WIFI_CONNECTION_STATUS_ADDRESS, WIFI_SIGNAL_LEVEL_ADDRESS, WIFI_SSID_ADDRESS, &connection_state, &signal_level);
            lumen_iot_wifi_update_ip(WIFI_IP_ADDRESS);
            lumen_iot_wifi_report_error(WIFI_ERROR_ADDRESS);

            s_wifi_connected = (connection_state == 1);

            if (connection_state == 1 && lumen_iot_wifi_has_ip() && !s_mqtt_initialized)
            {
                lumen_iot_mqtt_init("mqtts://broker.example.com:8883", ca_pem, NULL, NULL);
                lumen_iot_mqtt_bind_topic_to_display("factory/sensor1/name", SENSOR_NAME_ADDRESS, kString);
                lumen_iot_mqtt_bind_topic_json_to_display("factory/sensor1/data", "temperature", SENSOR_TEMP_JSON_ADDRESS, kFloat);
                s_mqtt_initialized = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void weather_task(void *pvParameters)
{
    while (1)
    {
        if (!lumen_iot_is_ota_in_progress() && s_wifi_connected)
        {
            lumen_iot_net_https_get_json(WEATHER_URL, "current.temperature_2m", TEMPERATURE_ADDRESS, kFloat);
        }
        vTaskDelay(pdMS_TO_TICKS(6000));
    }
}

static void public_ip_task(void *pvParameters)
{
    while (1)
    {
        if (!lumen_iot_is_ota_in_progress() && s_wifi_connected)
        {
            lumen_iot_net_https_get("URL_HERE", PUBLIC_IP_ADDRESS, kString);
        }
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

void app_main(void)
{
    
    lumen_iot_uart_config_t uart_config = {
        .uart_num = UART_NUM_1,
        .tx_pin = 26,
        .rx_pin = 25,
        .baud_rate = 115200,
        .rx_buffer_size = 1024,
    };
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_ERROR_CHECK(lumen_iot_wifi_init(&uart_config));
    lumen_iot_net_init();

    xTaskCreate(wifi_status_task, "wifi_status_task", 4096, NULL, 5, NULL);
    xTaskCreate(weather_task, "weather_task", 8192, NULL, 5, NULL);
    xTaskCreate(public_ip_task, "public_ip_task", 4096, NULL, 5, NULL);

    lumen_iot_wifi_scan_to_list(WIFI_LIST_ADDRESS, WIFI_LIST_MAX_ITEMS);
    vTaskDelay(pdMS_TO_TICKS(2000));
    lumen_iot_wifi_try_auto_connect(WIFI_LIST_INDEX_ADDRESS);

    lumen_packet_t *current_packet;

    while (1)
    {
        while (!lumen_iot_is_ota_in_progress() && lumen_available() > 0)
        {
            current_packet = lumen_get_first_packet();
            if (current_packet == NULL)
            {
                continue;
            }

            switch (current_packet->address)
            {
            case WIFI_RESCAN_ADDRESS:
                lumen_iot_wifi_scan_to_list(WIFI_LIST_ADDRESS, WIFI_LIST_MAX_ITEMS);
                lumen_iot_wifi_try_auto_connect(WIFI_LIST_INDEX_ADDRESS);
                break;
            case WIFI_LIST_INDEX_ADDRESS:
                lumen_iot_wifi_select_network((uint32_t)current_packet->data._s32);
                lumen_iot_wifi_update_password_saved_status(WIFI_PASSWORD_SAVED_ADDRESS);
                lumen_iot_wifi_update_auto_connect_status(WIFI_AUTO_CONNECT_TOGGLE_ADDRESS);
                break;
            case WIFI_PASSWORD_ADDRESS:
                lumen_iot_wifi_store_password(kString, current_packet->data);
                lumen_iot_wifi_update_password_saved_status(WIFI_PASSWORD_SAVED_ADDRESS);
                lumen_iot_wifi_update_auto_connect_status(WIFI_AUTO_CONNECT_TOGGLE_ADDRESS);
                break;
            case WIFI_LOGIN_BUTTON_ADDRESS:
                lumen_iot_wifi_login();
                break;
            case TEST_POST_ADDRESS:
                lumen_iot_net_https_post("URL_HERE", kString, current_packet->data);
                break;
            case TEST_NUMBER_POST_ADDRESS:
                lumen_iot_net_https_post("URL_HERE", kDouble, current_packet->data);
                break;
            case TEST_POST_JSON_ADDRESS:
                lumen_iot_net_https_post_json("URL_HERE", "valor", kDouble, current_packet->data);
                break;
            case TEST_MQTT_PUBLISH_ADDRESS:
                lumen_iot_mqtt_publish("factory/sensor1/status", kString, current_packet->data);
                break;
            case SEND_PRJ_ADDRESS:
                xTaskCreate(send_project_task, "send_project_task", 8192, NULL, 5, NULL);
                break;
            case SEND_FW_ADDRESS:
                xTaskCreate(send_firmware_task, "send_firmware_task", 8192, NULL, 5, NULL);
                break;
            case WIFI_MANUAL_SSID_ADDRESS:
                lumen_iot_wifi_set_manual_ssid(kString, current_packet->data);
                lumen_iot_wifi_update_password_saved_status(WIFI_PASSWORD_SAVED_ADDRESS);
                lumen_iot_wifi_update_auto_connect_status(WIFI_AUTO_CONNECT_TOGGLE_ADDRESS);
                break;
            case WIFI_FORGET_ADDRESS:
                lumen_iot_wifi_forget_network();
                lumen_iot_wifi_update_password_saved_status(WIFI_PASSWORD_SAVED_ADDRESS);
                lumen_iot_wifi_update_auto_connect_status(WIFI_AUTO_CONNECT_TOGGLE_ADDRESS);
                break;
            case WIFI_AUTO_CONNECT_TOGGLE_ADDRESS:
                lumen_iot_wifi_set_auto_connect(current_packet->data._bool);
                lumen_iot_wifi_update_auto_connect_status(WIFI_AUTO_CONNECT_TOGGLE_ADDRESS);
                break;
            default:
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}