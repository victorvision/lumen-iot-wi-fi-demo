#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/uart.h"
#include "LumenProtocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uart_port_t uart_num;
    int tx_pin;
    int rx_pin;
    int baud_rate;
    size_t rx_buffer_size;
} lumen_iot_uart_config_t;

esp_err_t lumen_iot_wifi_init(const lumen_iot_uart_config_t *uart_config);

esp_err_t lumen_iot_wifi_scan_to_list(uint16_t list_address, uint8_t max_items);
esp_err_t lumen_iot_wifi_select_network(uint32_t index);
esp_err_t lumen_iot_wifi_store_password(lumen_data_type_t type, lumen_data_t data);
esp_err_t lumen_iot_wifi_login(void);
esp_err_t lumen_iot_wifi_update_status(uint16_t connection_address, uint16_t signal_address, uint16_t ssid_address, int32_t *out_connection_state, int32_t *out_signal_level);
esp_err_t lumen_iot_wifi_set_manual_ssid(lumen_data_type_t type, lumen_data_t data);
esp_err_t lumen_iot_wifi_forget_network(void);

esp_err_t lumen_iot_wifi_get_ip(char *out_buffer, size_t buffer_size);
esp_err_t lumen_iot_wifi_update_ip(uint16_t ip_address);
esp_err_t lumen_iot_wifi_report_error(uint16_t error_address);
esp_err_t lumen_iot_wifi_update_password_saved_status(uint16_t address);
esp_err_t lumen_iot_wifi_update_auto_connect_status(uint16_t address);
esp_err_t lumen_iot_wifi_set_auto_connect(bool enabled);
esp_err_t lumen_iot_wifi_try_auto_connect(uint16_t list_index_address);

esp_err_t lumen_iot_net_https_get(const char *url, uint16_t address, lumen_data_type_t type);
esp_err_t lumen_iot_net_https_post(const char *url, lumen_data_type_t type, lumen_data_t data);

esp_err_t lumen_iot_mqtt_init(const char *uri, const char *ca_certificate_pem, const char *username, const char *password);
esp_err_t lumen_iot_mqtt_bind_topic_to_display(char *topic, uint16_t address, lumen_data_type_t type);
esp_err_t lumen_iot_mqtt_bind_topic_json_to_display(char *topic, const char *json_path, uint16_t address, lumen_data_type_t type);
esp_err_t lumen_iot_net_https_post_json(const char *url, const char *json_field, lumen_data_type_t type, lumen_data_t data);

esp_err_t lumen_iot_mqtt_publish(char *topic, lumen_data_type_t type, lumen_data_t data);
esp_err_t lumen_iot_net_https_get_json(const char *url, const char *json_path, uint16_t address, lumen_data_type_t type);

esp_err_t lumen_iot_set_uart_baudrate(int baud_rate);
bool lumen_iot_wifi_has_ip(void);
bool lumen_iot_is_ota_in_progress(void);
void lumen_iot_net_init(void);

esp_err_t lumen_iot_display_update_firmware(const char *url, uint16_t reboot_address, uint32_t default_baudrate, lumen_baudrate_serial_update_t transfer_baudrate);
esp_err_t lumen_iot_display_update_project(const char *url, uint16_t reboot_address, uint32_t default_baudrate, lumen_baudrate_serial_update_t transfer_baudrate);

#ifdef __cplusplus
}
#endif