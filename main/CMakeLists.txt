idf_component_register(
    SRCS
        "main.c"
        "nfc.c"
        "telegram.c"
        "solenoid.c"
        "storage.c"
        "wifi.c"
        "thingsboard.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_gpio     # driver/gpio.h          → main.c, nfc.c, solenoid.c
        esp_driver_uart     # driver/uart.h           → nfc.c  (UART2 Pepper C1)
        esp_driver_ledc     # driver/ledc.h           → solenoid.c (PWM 9V→6V)
        esp_adc             # esp_adc/adc_oneshot.h  → thingsboard.c (ADC batería)
        esp_wifi            # esp_wifi.h, esp_mac.h  → wifi.c, thingsboard.c
        esp_netif           # esp_netif.h             → wifi.c
        esp_event           # esp_event.h             → wifi.c (event loop)
        esp_http_client     # esp_http_client.h,
                            # esp_crt_bundle.h        → telegram.c (long-polling + TLS)
        mqtt                # mqtt_client.h           → thingsboard.c
        nvs_flash           # nvs_flash.h, nvs.h      → storage.c
        json                # cJSON.h                 → telegram.c, thingsboard.c
)
