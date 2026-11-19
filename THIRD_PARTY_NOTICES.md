# Third-Party Notices

This component and example project build upon external software with their own licenses and copyright notices:

- **ESP-IDF** (Espressif IoT Development Framework): Primarily licensed under the **Apache License, Version 2.0**, with specific components and bundled libraries governed by their own licenses. See the `LICENSE` and `COPYRIGHT` files in your ESP-IDF installation for full license texts and component-level attributions.
- **Bundled ESP-IDF / Open-Source Subsystems** used via `CMakeLists.txt` and `app_main`:
  - **FreeRTOS** (task/RTOS headers and scheduler)
  - **lwIP** (TCP/IP stack via `esp_netif` / networking)
  - **mbedTLS** (TLS/SSL for HTTPS and MQTTS)
  - **cJSON** (`json` component for JSON parsing/generation)
  - **ESP-IDF MQTT client, HTTP client, Wi-Fi, NVS, driver, log, and timer components**

If you distribute compiled binaries or firmware images, retain applicable copyright notices and license terms from ESP-IDF and its bundled third-party components in your distribution documentation.