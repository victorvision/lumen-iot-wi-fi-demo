#include <stdlib.h>
#include <string.h>

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs.h"

#include "LumenProtocol.h"
#include "lumen_iot_wifi.h"
#include "lumen_iot_internal.h"

#define LUMEN_IOT_WIFI_MAX_SCAN_RESULTS 20
#define LUMEN_IOT_WIFI_MAX_SAVED_NETWORKS 15
#define LUMEN_IOT_WIFI_NVS_NAMESPACE "lumen_wifi"
#define LUMEN_IOT_WIFI_NVS_NETWORKS_KEY "saved_networks"
#define LUMEN_IOT_WIFI_PASSWORD_MAX_LEN 64
#define LUMEN_IOT_WIFI_SSID_MAX_LEN 33
#define LUMEN_IOT_WIFI_IP_STR_MAX_LEN 16

typedef struct
{
    char ssid[LUMEN_IOT_WIFI_SSID_MAX_LEN];
    char password[LUMEN_IOT_WIFI_PASSWORD_MAX_LEN];
    bool auto_connect;
} lumen_iot_wifi_saved_network_t;

static const char *TAG = "lumen_iot_wifi";

static wifi_ap_record_t s_scan_results[LUMEN_IOT_WIFI_MAX_SCAN_RESULTS];
static uint16_t s_scan_result_count = 0;

static char s_current_ssid[LUMEN_IOT_WIFI_SSID_MAX_LEN] = {0};

static void lumen_iot_wifi_copy_ssid(const wifi_ap_record_t *entry, char *out, size_t out_size)
{
    size_t ssid_length = strnlen((const char *)entry->ssid, sizeof(entry->ssid));
    size_t copy_len = ssid_length;

    if (copy_len > out_size - 1)
    {
        copy_len = out_size - 1;
    }

    memcpy(out, entry->ssid, copy_len);
    out[copy_len] = '\0';
}

static void lumen_iot_wifi_load_saved_networks(lumen_iot_wifi_saved_network_t *networks)
{
    size_t total_size = sizeof(lumen_iot_wifi_saved_network_t) * LUMEN_IOT_WIFI_MAX_SAVED_NETWORKS;
    memset(networks, 0, total_size);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LUMEN_IOT_WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        return;
    }

    size_t length = total_size;
    err = nvs_get_blob(handle, LUMEN_IOT_WIFI_NVS_NETWORKS_KEY, networks, &length);
    nvs_close(handle);

    if (err != ESP_OK || length != total_size)
    {
        memset(networks, 0, total_size);
    }
}

static esp_err_t lumen_iot_wifi_save_saved_networks(const lumen_iot_wifi_saved_network_t *networks)
{
    size_t total_size = sizeof(lumen_iot_wifi_saved_network_t) * LUMEN_IOT_WIFI_MAX_SAVED_NETWORKS;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LUMEN_IOT_WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK)
    {
        err = nvs_set_blob(handle, LUMEN_IOT_WIFI_NVS_NETWORKS_KEY, networks, total_size);
        if (err == ESP_OK)
        {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    return err;
}

static int lumen_iot_wifi_find_saved_network(const lumen_iot_wifi_saved_network_t *networks, const char *ssid)
{
    for (int i = 0; i < LUMEN_IOT_WIFI_MAX_SAVED_NETWORKS; i++)
    {
        if (networks[i].ssid[0] != '\0' && strcmp(networks[i].ssid, ssid) == 0)
        {
            return i;
        }
    }

    return -1;
}

static int lumen_iot_wifi_find_empty_slot(const lumen_iot_wifi_saved_network_t *networks)
{
    for (int i = 0; i < LUMEN_IOT_WIFI_MAX_SAVED_NETWORKS; i++)
    {
        if (networks[i].ssid[0] == '\0')
        {
            return i;
        }
    }

    return -1;
}

esp_err_t lumen_iot_wifi_scan_to_list(uint16_t list_address, uint8_t max_items)
{
    if (max_items == 0 || max_items > LUMEN_IOT_WIFI_MAX_SCAN_RESULTS)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    uint16_t number = max_items;
    err = esp_wifi_scan_get_ap_records(&number, s_scan_results);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(err));
        return err;
    }

    s_scan_result_count = number;

    for (uint16_t i = 0; i < number; i++)
    {
        char ssid_buffer[LUMEN_IOT_WIFI_SSID_MAX_LEN];
        lumen_iot_wifi_copy_ssid(&s_scan_results[i], ssid_buffer, sizeof(ssid_buffer));
        size_t ssid_length = strnlen(ssid_buffer, sizeof(ssid_buffer));
        lumen_write_variable_list(list_address, i, (uint8_t *)ssid_buffer, ssid_length + 1);
    }

    for (uint16_t i = number; i < max_items; i++)
    {
        lumen_write_variable_list(list_address, i, (uint8_t *)"", 1);
    }

    return ESP_OK;
}

esp_err_t lumen_iot_wifi_select_network(uint32_t index)
{
    if (index >= s_scan_result_count)
    {
        return ESP_ERR_INVALID_ARG;
    }

    lumen_iot_wifi_copy_ssid(&s_scan_results[index], s_current_ssid, sizeof(s_current_ssid));
    lumen_iot_wifi_clear_last_error();
    return ESP_OK;
}

esp_err_t lumen_iot_wifi_set_manual_ssid(lumen_data_type_t type, lumen_data_t data)
{
    if (type != kString || data._string[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_current_ssid, data._string, sizeof(s_current_ssid));
    lumen_iot_wifi_clear_last_error();
    return ESP_OK;
}

esp_err_t lumen_iot_wifi_store_password(lumen_data_type_t type, lumen_data_t data)
{
    if (s_current_ssid[0] == '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }

    char password_buffer[LUMEN_IOT_WIFI_PASSWORD_MAX_LEN];

    switch (type)
    {
    case kString:
        strlcpy(password_buffer, data._string, sizeof(password_buffer));
        break;
    case kS8:
        snprintf(password_buffer, sizeof(password_buffer), "%d", data._s8);
        break;
    case kU8:
        snprintf(password_buffer, sizeof(password_buffer), "%u", data._u8);
        break;
    case kS16:
        snprintf(password_buffer, sizeof(password_buffer), "%d", data._s16);
        break;
    case kU16:
        snprintf(password_buffer, sizeof(password_buffer), "%u", data._u16);
        break;
    case kS32:
        snprintf(password_buffer, sizeof(password_buffer), "%ld", (long)data._s32);
        break;
    case kU32:
        snprintf(password_buffer, sizeof(password_buffer), "%lu", (unsigned long)data._u32);
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    lumen_iot_wifi_saved_network_t networks[LUMEN_IOT_WIFI_MAX_SAVED_NETWORKS];
    lumen_iot_wifi_load_saved_networks(networks);

    int index = lumen_iot_wifi_find_saved_network(networks, s_current_ssid);
    if (index < 0)
    {
        index = lumen_iot_wifi_find_empty_slot(networks);
        if (index < 0)
        {
            return ESP_ERR_NO_MEM;
        }

        strlcpy(networks[index].ssid, s_current_ssid, sizeof(networks[index].ssid));
        networks[index].auto_connect = true;
    }

    strlcpy(networks[index].password, password_buffer, sizeof(networks[index].password));

    return lumen_iot_wifi_save_saved_networks(networks);
}

esp_err_t lumen_iot_wifi_login(void)
{
    if (s_current_ssid[0] == '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }

    lumen_iot_wifi_clear_last_error();

    lumen_iot_wifi_saved_network_t networks[LUMEN_IOT_WIFI_MAX_SAVED_NETWORKS];
    lumen_iot_wifi_load_saved_networks(networks);

    int index = lumen_iot_wifi_find_saved_network(networks, s_current_ssid);
    if (index < 0 || networks[index].password[0] == '\0')
    {
        ESP_LOGE(TAG, "no saved password for network '%s'", s_current_ssid);
        return ESP_ERR_NOT_FOUND;
    }

    wifi_config_t wifi_config = {0};

    size_t ssid_length = strnlen(s_current_ssid, sizeof(wifi_config.sta.ssid));
    memcpy(wifi_config.sta.ssid, s_current_ssid, ssid_length);
    strlcpy((char *)wifi_config.sta.password, networks[index].password, sizeof(wifi_config.sta.password));

    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED)
    {
        ESP_LOGE(TAG, "esp_wifi_disconnect failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    return esp_wifi_connect();
}

esp_err_t lumen_iot_wifi_forget_network(void)
{
    if (s_current_ssid[0] == '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }

    lumen_iot_wifi_saved_network_t networks[LUMEN_IOT_WIFI_MAX_SAVED_NETWORKS];
    lumen_iot_wifi_load_saved_networks(networks);

    int index = lumen_iot_wifi_find_saved_network(networks, s_current_ssid);
    if (index < 0)
    {
        ESP_LOGW(TAG, "network '%s' had no saved password", s_current_ssid);
        return ESP_ERR_NOT_FOUND;
    }

    memset(&networks[index], 0, sizeof(networks[index]));
    esp_err_t err = lumen_iot_wifi_save_saved_networks(networks);

    ESP_LOGI(TAG, "forgot network '%s'", s_current_ssid);
    return err;
}

esp_err_t lumen_iot_wifi_update_password_saved_status(uint16_t address)
{
    bool has_password = false;

    if (s_current_ssid[0] != '\0')
    {
        lumen_iot_wifi_saved_network_t networks[LUMEN_IOT_WIFI_MAX_SAVED_NETWORKS];
        lumen_iot_wifi_load_saved_networks(networks);

        int index = lumen_iot_wifi_find_saved_network(networks, s_current_ssid);
        if (index >= 0 && networks[index].password[0] != '\0')
        {
            has_password = true;
        }
    }

    lumen_packet_t packet = {address, kBool};
    packet.data._bool = has_password;
    lumen_write_packet(&packet);
    return ESP_OK;
}

esp_err_t lumen_iot_wifi_update_auto_connect_status(uint16_t address)
{
    bool auto_connect = false;

    if (s_current_ssid[0] != '\0')
    {
        lumen_iot_wifi_saved_network_t networks[LUMEN_IOT_WIFI_MAX_SAVED_NETWORKS];
        lumen_iot_wifi_load_saved_networks(networks);

        int index = lumen_iot_wifi_find_saved_network(networks, s_current_ssid);
        if (index >= 0)
        {
            auto_connect = networks[index].auto_connect;
        }
    }

    lumen_packet_t packet = {address, kBool};
    packet.data._bool = auto_connect;
    lumen_write_packet(&packet);
    return ESP_OK;
}

esp_err_t lumen_iot_wifi_set_auto_connect(bool enabled)
{
    if (s_current_ssid[0] == '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }

    lumen_iot_wifi_saved_network_t networks[LUMEN_IOT_WIFI_MAX_SAVED_NETWORKS];
    lumen_iot_wifi_load_saved_networks(networks);

    int index = lumen_iot_wifi_find_saved_network(networks, s_current_ssid);
    if (index < 0)
    {
        index = lumen_iot_wifi_find_empty_slot(networks);
        if (index < 0)
        {
            return ESP_ERR_NO_MEM;
        }

        strlcpy(networks[index].ssid, s_current_ssid, sizeof(networks[index].ssid));
        networks[index].password[0] = '\0';
    }

    networks[index].auto_connect = enabled;

    return lumen_iot_wifi_save_saved_networks(networks);
}

static int32_t lumen_iot_wifi_find_auto_connect_candidate(void)
{
    lumen_iot_wifi_saved_network_t networks[LUMEN_IOT_WIFI_MAX_SAVED_NETWORKS];
    lumen_iot_wifi_load_saved_networks(networks);

    int32_t best_index = -1;
    int8_t best_rssi = INT8_MIN;

    for (uint16_t i = 0; i < s_scan_result_count; i++)
    {
        char ssid_buffer[LUMEN_IOT_WIFI_SSID_MAX_LEN];
        lumen_iot_wifi_copy_ssid(&s_scan_results[i], ssid_buffer, sizeof(ssid_buffer));

        int saved_index = lumen_iot_wifi_find_saved_network(networks, ssid_buffer);
        if (saved_index < 0)
        {
            continue;
        }

        bool has_password = false;
        if (networks[saved_index].password[0] != '\0')
        {
            has_password = true;
        }

        bool auto_connect = networks[saved_index].auto_connect;

        if (has_password && auto_connect && s_scan_results[i].rssi > best_rssi)
        {
            best_rssi = s_scan_results[i].rssi;
            best_index = (int32_t)i;
        }
    }

    return best_index;
}

esp_err_t lumen_iot_wifi_try_auto_connect(uint16_t list_index_address)
{
    if (lumen_iot_wifi_has_ip())
    {
        return ESP_ERR_INVALID_STATE;
    }

    int32_t index = lumen_iot_wifi_find_auto_connect_candidate();
    if (index < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = lumen_iot_wifi_select_network((uint32_t)index);
    if (err != ESP_OK)
    {
        return err;
    }

    lumen_packet_t index_packet = {list_index_address, kS32};
    index_packet.data._s32 = index;
    lumen_write_packet(&index_packet);

    ESP_LOGI(TAG, "auto-connect: trying '%s' (index %ld)", s_current_ssid, (long)index);
    return lumen_iot_wifi_login();
}

esp_err_t lumen_iot_wifi_update_status(uint16_t connection_address, uint16_t signal_address, uint16_t ssid_address, int32_t *out_connection_state, int32_t *out_signal_level)
{
    wifi_ap_record_t ap_info;
    int32_t connection_state = 0;
    int32_t signal_level = 0;
    char ssid_buffer[LUMEN_IOT_WIFI_SSID_MAX_LEN] = {0};

    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    {
        connection_state = 1;
        lumen_iot_wifi_copy_ssid(&ap_info, ssid_buffer, sizeof(ssid_buffer));

        if (ap_info.rssi >= -55)
        {
            signal_level = 4;
        }
        else if (ap_info.rssi >= -66)
        {
            signal_level = 3;
        }
        else if (ap_info.rssi >= -77)
        {
            signal_level = 2;
        }
        else
        {
            signal_level = 1;
        }
    }

    lumen_packet_t connection_packet = {connection_address, kS32};
    connection_packet.data._s32 = connection_state;
    lumen_write_packet(&connection_packet);

    lumen_packet_t signal_packet = {signal_address, kS32};
    signal_packet.data._s32 = signal_level;
    lumen_write_packet(&signal_packet);

    lumen_packet_t ssid_packet = {ssid_address, kString};
    strlcpy(ssid_packet.data._string, ssid_buffer, MAX_STRING_SIZE);
    lumen_write_packet(&ssid_packet);

    if (out_connection_state != NULL)
    {
        *out_connection_state = connection_state;
    }

    if (out_signal_level != NULL)
    {
        *out_signal_level = signal_level;
    }

    return ESP_OK;
}

esp_err_t lumen_iot_wifi_get_ip(char *out_buffer, size_t buffer_size)
{
    if (out_buffer == NULL || buffer_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(netif, &ip_info);
    if (err != ESP_OK)
    {
        return err;
    }

    if (ip_info.ip.addr == 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(out_buffer, buffer_size, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

esp_err_t lumen_iot_wifi_update_ip(uint16_t ip_address)
{
    char ip_buffer[LUMEN_IOT_WIFI_IP_STR_MAX_LEN] = {0};

    esp_err_t err = lumen_iot_wifi_get_ip(ip_buffer, sizeof(ip_buffer));

    lumen_packet_t packet = {ip_address, kString};
    strlcpy(packet.data._string, ip_buffer, MAX_STRING_SIZE);
    lumen_write_packet(&packet);

    return err;
}

esp_err_t lumen_iot_wifi_report_error(uint16_t error_address)
{
    char error_buffer[64];
    if (!lumen_iot_wifi_consume_pending_error(error_buffer, sizeof(error_buffer)))
    {
        return ESP_ERR_NOT_FOUND;
    }

    lumen_packet_t packet = {error_address, kString};
    strlcpy(packet.data._string, error_buffer, MAX_STRING_SIZE);
    lumen_write_packet(&packet);
    return ESP_OK;
}