/**
 * main.c — Sistema de Cerradura Electrónica con NFC y Bot de Telegram
 *
 * Flujo de arranque:
 *   1. NVS inicializado
 *   2. Primitivos de ThingsBoard creados
 *   3. Hardware inicializado (solenoide, NFC, reed switch)
 *   4. WiFi conectado (bloquea hasta obtener IP)
 *   5. Cinco tareas FreeRTOS lanzadas
 *
 * Configuración: exclusivamente por menuconfig (idf.py menuconfig).
 *   · WiFi:       CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD
 *   · Telegram:   CONFIG_TELEGRAM_TOKEN, CONFIG_TELEGRAM_CHAT_ID
 *   · ThingsBoard: CONFIG_TB_TOKEN
 *   · Versión:    CONFIG_FW_VERSION
 *
 * Hardware:
 *   · Lector NFC Pepper C1 — UART2 GPIO16(RX)/GPIO17(TX)
 *   · Solenoide latching SK0730 vía L298N — GPIO18(DIR), GPIO19(EN PWM)
 *   · Reed switch cajón — GPIO21 (INPUT_PULLUP, LOW=abierto)
 *   · LED verde acceso concedido — GPIO22
 *   · LED rojo  acceso denegado  — GPIO23
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "config.h"

#include "storage.h"
#include "wifi.h"
#include "nfc.h"
#include "telegram.h"
#include "solenoid.h"
#include "thingsboard.h"

static const char *TAG = "MAIN";

#define GPIO_REED  21   /* Reed switch: INPUT_PULLUP, LOW = cajón abierto */

/* ── Tarea de monitorización del reed switch ───────────────────────────── */

static void task_status(void *pvParameters)
{
    bool last_open = false;
    ESP_LOGI(TAG, "Monitorización del cajón activa (GPIO%d)", GPIO_REED);

    while (1) {
        bool is_open = (gpio_get_level(GPIO_REED) == 0);
        if (is_open != last_open) {
            ESP_LOGI(TAG, "Cajón: %s", is_open ? "ABIERTO" : "CERRADO");
            last_open = is_open;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ── Punto de entrada ──────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Sistema de Cerradura Electronica");
    ESP_LOGI(TAG, "  FW v" CONFIG_FW_VERSION);
    ESP_LOGI(TAG, "========================================");

    /* 1. NVS: siempre primero */
    ESP_ERROR_CHECK(storage_init());

    /* 2. Primitivos de ThingsBoard antes de solenoid_init()/nfc_init()
     *    para que tb_set_last_access() y tb_set_lock_state() sean seguros */
    tb_init();

    /* 3. Hardware físico */
    solenoid_init();
    nfc_init();

    gpio_config_t reed_cfg = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << GPIO_REED),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&reed_cfg));

    /* 4. WiFi: bloquea hasta obtener IP (reintentos indefinidos) */
    ESP_ERROR_CHECK(wifi_init_sta());
    wifi_wait_connected();

    /* 5. Tareas FreeRTOS
     *    Prioridades: solenoid > nfc > telegram > thingsboard = status
     *    Stack de telegram grande por mbedTLS + cJSON en el long-polling. */
    xTaskCreate(task_solenoid,    "solenoid",    2048,  NULL, 6, NULL);
    xTaskCreate(task_nfc,         "nfc",         4096,  NULL, 5, NULL);
    xTaskCreate(task_telegram,    "telegram",    16384, NULL, 4, NULL);
    xTaskCreate(task_thingsboard, "thingsboard", 6144,  NULL, 3, NULL);
    xTaskCreate(task_status,      "status",      2048,  NULL, 3, NULL);

    ESP_LOGI(TAG, "Sistema iniciado. Tareas en ejecucion.");
}
