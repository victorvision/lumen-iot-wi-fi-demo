# Lumen IoT Wi-Fi

Wi-Fi, HTTP, MQTT and display update library for the ESP32 and Smart Plus Displays. Built for [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/).

## Overview

Lumen IoT Wi-Fi connects a Smart Plus Display to the network through an ESP32. The ESP32 and the display talk by serial communication (UART). The library keeps all the network logic inside. You call simple C functions.

Tasks that need a lot of code are done with one function call, for example:

- Show the available Wi-Fi networks on the display.
- Type the Wi-Fi password with the display keyboard.
- Send and receive data over HTTP and MQTT.
- Update the display firmware or project from a URL.

The library uses ESP-IDF, the official framework from Espressif.

## Features

- **Wi-Fi login from the display:** List networks, select one, type the password on the display and connect.
- **Saved networks:** Passwords are saved in flash (NVS). The ESP32 can connect to a known network by itself.
- **Status on the display:** Show the connection state, signal level, network name, IP address and error messages.
- **HTTPS client:** GET and POST requests, with JSON support.
- **MQTT client:** Connect with TLS, bind topics to display variables and publish values.
- **Display updates over serial:** Send firmware or a project to the display. You only pass the URL of the file.

## Requirements

- ESP32 board with Wi-Fi.
- ESP-IDF v5.1 or newer.
- A Smart Plus Display connected to the ESP32 by UART.
- The [Lumen Protocol library](https://github.com/victorvision/Lumen-Protocol).

The library also uses these ESP-IDF components: Wi-Fi, `esp_netif`, HTTP client, MQTT, NVS and cJSON. All of them come with ESP-IDF.

## Installation

Copy the library into the `components` folder of your ESP-IDF project.

Add the component to the `CMakeLists.txt` of your main component:

```cmake
idf_component_register(SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event nvs_flash driver LumenProtocol lumen_iot_wifi)
```

## Initialization

Initialize the library once at startup. Set the UART pins and speed to match your board and your display.

Example:

```c
#include "lumen_iot_wifi.h"

void app_main(void)
{
    
    lumen_iot_uart_config_t uart_config = {
        .uart_num = UART_NUM_1,
        .tx_pin = 26,
        .rx_pin = 25,
        .baud_rate = 115200,
        .rx_buffer_size = 1024,
    };

    ESP_ERROR_CHECK(lumen_iot_wifi_init(&uart_config));
}
```

`lumen_iot_wifi_init` starts NVS, the UART and the Wi-Fi station.

Each variable on the display has an address. Most functions receive an address, so the library knows which display variable to read or write. The addresses come from your display project.

## Wi-Fi login flow

Call these functions from your display event handlers.

1. `lumen_iot_wifi_scan_to_list` scans and fills a display list with network names.
1. `lumen_iot_wifi_select_network` selects a network from the list. For a hidden network, use `lumen_iot_wifi_set_manual_ssid` instead.
1. `lumen_iot_wifi_store_password` saves the password for the selected network.
1. `lumen_iot_wifi_login` connects to the selected network.

To connect without user input after a restart, scan first and then call `lumen_iot_wifi_try_auto_connect`. It selects the saved network with the best signal that has auto-connect on.

## HTTP

Use the `https_get` functions to read a value and write it to the display. Use the `https_post` functions to send a value from your code.

Example:

```c

lumen_iot_net_https_get_json("https://api.example.com/weather", "main.temp", TEMPERATURE_ADDRESS, kFloat);
lumen_iot_net_https_post_json("https://api.example.com/data", "temperature", kFloat, value);
```

## MQTT

Start MQTT after the device has an IP address. Pass `NULL` as the CA certificate to use the ESP-IDF certificate bundle. Pass a PEM string to use your own CA.

Example:

```c
lumen_iot_mqtt_init("mqtts://broker.example.com:8883", ca_pem, NULL, NULL);

lumen_iot_mqtt_bind_topic_to_display("factory/sensor1/name", NAME_ADDRESS, kString);
lumen_iot_mqtt_bind_topic_json_to_display("factory/sensor1/data", "temperature", TEMPERATURE_ADDRESS, kFloat);
```

When a message arrives on a bound topic, the library writes the value to the display address. To send data from your code, use `lumen_iot_mqtt_publish`.

## Update the display over serial

Pass the URL of the compiled file. The library sends the reboot command to the display, changes the serial baudrate, sends the file and restores the baudrate.

```c
lumen_iot_display_update_firmware("https://example.com/firmware", REBOOT_ADDRESS, 115200, k1250000);
lumen_iot_display_update_project("https://example.com/project", REBOOT_ADDRESS, 115200, k1250000);
```

- `reboot_address`: Is the display address of the reboot variable.
- `default_baudrate`: Is the normal baudrate of your project.
- `transfer_baudrate`: Is the baudrate used during the transfer.

The functions block until the update ends. While an update runs, the other network functions return `ESP_ERR_INVALID_STATE`.

## API reference

All functions return `esp_err_t`, except where noted.

### Setup functions

- `lumen_iot_wifi_init`: Starts NVS, UART and Wi-Fi.
- `lumen_iot_set_uart_baudrate`: Changes the UART speed.

### Wi-Fi functions

- `lumen_iot_wifi_scan_to_list`: Scans for networks and writes the names to a display text list.
- `lumen_iot_wifi_select_network`: Selects a network from the last scan.
- `lumen_iot_wifi_set_manual_ssid`: Sets the network name typed by the user.
- `lumen_iot_wifi_store_password`: Saves the password of the selected network.
- `lumen_iot_wifi_login`: Connects to the selected network.
- `lumen_iot_wifi_forget_network`: Removes the selected network from the saved list.
- `lumen_iot_wifi_set_auto_connect`: Turns auto-connect on or off for the selected network.
- `lumen_iot_wifi_try_auto_connect`: Connects to the saved network with the best signal.

### Status functions

- `lumen_iot_wifi_update_status`: Writes connection state, signal level and network name to the display.
- `lumen_iot_wifi_update_ip`: Writes the IP address to the display.
- `lumen_iot_wifi_report_error`: Writes the last Wi-Fi error message to the display, if there is a new one.
- `lumen_iot_wifi_update_password_saved_status`: Writes to the display if the selected network has a saved password.
- `lumen_iot_wifi_update_auto_connect_status`: Writes to the display if the selected network has auto-connect on.
- `lumen_iot_wifi_get_ip`: Copies the IP address to a buffer.
- `lumen_iot_wifi_has_ip`: Returns `true` if the device has an IP address.
- `lumen_iot_is_ota_in_progress`: Returns `true` while a display update runs.

The connection state is `1` when connected and `0` when not. The signal level goes from `0` (not connected) to `4` (strong).

### HTTP functions

- `lumen_iot_net_https_get`: GET request. Writes the raw response to the display.
- `lumen_iot_net_https_get_json`: GET request. Writes one JSON field to the display.
- `lumen_iot_net_https_post`: POST request with a raw value.
- `lumen_iot_net_https_post_json`: POST request with a JSON body: `{"field": value}`.

### MQTT functions

- `lumen_iot_mqtt_init`: Connects to the broker.
- `lumen_iot_mqtt_bind_topic_to_display`: Writes the raw payload of a topic to a display address.
- `lumen_iot_mqtt_bind_topic_json_to_display`: Writes one JSON field of a topic payload to a display address.
- `lumen_iot_mqtt_publish`: Publishes a value to a topic.

### Display update functions

- `lumen_iot_display_update_firmware`: Sends firmware to the display from a URL.
- `lumen_iot_display_update_project`: Sends a project to the display from a URL.

## Security

- HTTPS requests check the server certificate with the ESP-IDF certificate bundle. The URL decides the protocol, so an `http://` URL is not encrypted.
- For MQTT, you can pass your own CA certificate. In this case, the library does not check the host name of the broker.
- Saved Wi-Fi passwords are stored in NVS. To protect them, enable NVS encryption in your ESP-IDF project (`CONFIG_NVS_ENCRYPTION`).

## Example project

This repository includes an example project. It shows some of the library functions and how the display and the ESP32 work together over Wi-Fi.

The ESP32 model used to test this example project was the ESP-WROOM-32.