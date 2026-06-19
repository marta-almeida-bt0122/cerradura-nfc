/**
 * nfc.c — Interfaz con el lector Pepper C1 (Eccel Technology) por UART2.
 *
 * PROTOCOLO BINARIO DEL PEPPER C1:
 *   [0xAA][0x00][LEN][CMD][STATUS][CARD_TYPE][UID_LEN][UID0..UIDN][CHK]
 *
 *   · 0xAA 0x00  — cabecera de sincronismo
 *   · LEN        — número de bytes desde CMD hasta CHK (inclusive)
 *   · CMD        — 0x01 = tarjeta detectada/seleccionada
 *   · STATUS     — 0x00 = éxito
 *   · CARD_TYPE  — 0x01 = MIFARE Classic 1K, 0x02 = 4K, etc.
 *   · UID_LEN    — longitud del UID en bytes (normalmente 4 o 7)
 *   · UID0..UIDN — bytes del UID
 *   · CHK        — XOR de todos los bytes desde CMD hasta UIDN
 *
 * NOTA: Verificar con el datasheet de la versión de firmware del Pepper C1.
 * Si CMD o el offset del UID difieren, ajustar NFC_CMD_CARD y NFC_PAYLOAD_UID_OFFSET.
 */

#include "nfc.h"
#include "solenoid.h"
#include "storage.h"
#include "telegram.h"
#include "thingsboard.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "NFC";

/* ── Configuración de hardware ─────────────────────────────────────────── */

#define UART_PORT       UART_NUM_2
#define UART_BAUD       115200
#define GPIO_UART_TX    17      /* ESP32 TX → Pepper C1 RX */
#define GPIO_UART_RX    16      /* Pepper C1 TX → ESP32 RX */
#define UART_BUF_SIZE   256

#define GPIO_LED_GREEN  22
#define GPIO_LED_RED    23

/* ── Constantes del protocolo Pepper C1 ───────────────────────────────── */

#define NFC_SOF1              0xAA
#define NFC_SOF2              0x00
#define NFC_CMD_CARD          0x01  /* Tarjeta detectada/seleccionada */
#define NFC_STATUS_OK         0x00
/* payload: [CMD][STATUS][CARD_TYPE][UID_LEN][UID0..UIDN][CHK]
 * índice:     0      1       2          3       4..       fin   */
#define NFC_PAYLOAD_UID_OFFSET   4  /* Primer byte del UID dentro del payload */
#define NFC_MAX_PAYLOAD_LEN     32
#define NFC_UID_STR_MAX_LEN     20  /* 7 bytes × 2 hex + '\0' = 15; margen extra */

#define NFC_DEBOUNCE_MS  3000   /* Misma tarjeta ignorada durante 3 s */

/* ── Estado del parser ─────────────────────────────────────────────────── */

typedef enum {
    PARSE_SOF1 = 0,
    PARSE_SOF2,
    PARSE_LEN,
    PARSE_DATA,
} parse_state_t;

/* ── Helpers internos ──────────────────────────────────────────────────── */

static void led_set(int green, int red)
{
    gpio_set_level(GPIO_LED_GREEN, green);
    gpio_set_level(GPIO_LED_RED,   red);
}

static void bytes_to_hex(const uint8_t *bytes, int len, char *out)
{
    for (int i = 0; i < len; i++) {
        sprintf(out + i * 2, "%02X", bytes[i]);
    }
    out[len * 2] = '\0';
}

/**
 * Procesa un byte mediante la máquina de estados.
 * Cuando se recibe una trama completa y válida, escribe el UID en uid_out
 * y retorna true.
 */
static bool parse_byte(uint8_t byte,
                        parse_state_t *state,
                        uint8_t *payload,
                        uint8_t *expected_len,
                        uint8_t *received,
                        char *uid_out)
{
    switch (*state) {
    case PARSE_SOF1:
        if (byte == NFC_SOF1) *state = PARSE_SOF2;
        break;

    case PARSE_SOF2:
        *state = (byte == NFC_SOF2) ? PARSE_LEN : PARSE_SOF1;
        break;

    case PARSE_LEN:
        if (byte == 0 || byte > NFC_MAX_PAYLOAD_LEN) {
            *state = PARSE_SOF1;
            break;
        }
        *expected_len = byte;
        *received     = 0;
        *state = PARSE_DATA;
        break;

    case PARSE_DATA:
        payload[(*received)++] = byte;
        if (*received < *expected_len) break;

        /* Trama completa: verificar checksum XOR */
        *state = PARSE_SOF1;

        {
            uint8_t chk = 0;
            for (int i = 0; i < *expected_len - 1; i++) chk ^= payload[i];

            if (chk != payload[*expected_len - 1]) {
                ESP_LOGW(TAG, "Checksum incorrecto (calc=0x%02X recv=0x%02X)",
                         chk, payload[*expected_len - 1]);
                break;
            }

            uint8_t cmd    = payload[0];
            uint8_t status = payload[1];

            if (cmd != NFC_CMD_CARD) {
                ESP_LOGD(TAG, "Trama ignorada (cmd=0x%02X)", cmd);
                break;
            }
            if (status != NFC_STATUS_OK) {
                ESP_LOGW(TAG, "Status de error: 0x%02X", status);
                break;
            }

            /* payload[3] = UID_LEN, payload[4..] = UID bytes */
            if (*expected_len < NFC_PAYLOAD_UID_OFFSET + 1) {
                ESP_LOGW(TAG, "Trama demasiado corta para UID");
                break;
            }

            uint8_t uid_len = payload[3];
            if ((uint8_t)(NFC_PAYLOAD_UID_OFFSET + uid_len) >= *expected_len) {
                ESP_LOGW(TAG, "UID_LEN=%d excede la trama", uid_len);
                break;
            }

            bytes_to_hex(&payload[NFC_PAYLOAD_UID_OFFSET], uid_len, uid_out);
            return true;
        }
    }
    return false;
}

/* ── API pública ───────────────────────────────────────────────────────── */

void nfc_init(void)
{
    uart_config_t uart_cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT,
                                 GPIO_UART_TX, GPIO_UART_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT,
                                        UART_BUF_SIZE * 2, 0, 0, NULL, 0));

    gpio_config_t leds = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << GPIO_LED_GREEN) | (1ULL << GPIO_LED_RED),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&leds));
    led_set(0, 0);

    ESP_LOGI(TAG, "NFC inicializado (UART2 RX=GPIO%d TX=GPIO%d %d bps)",
             GPIO_UART_RX, GPIO_UART_TX, UART_BAUD);
}

void task_nfc(void *pvParameters)
{
    uint8_t       raw[UART_BUF_SIZE];
    parse_state_t state        = PARSE_SOF1;
    uint8_t       payload[NFC_MAX_PAYLOAD_LEN];
    uint8_t       expected_len = 0;
    uint8_t       received     = 0;
    char          uid_str[NFC_UID_STR_MAX_LEN]  = "";
    char          last_uid[NFC_UID_STR_MAX_LEN] = "";
    TickType_t    last_time    = 0;

    ESP_LOGI(TAG, "Tarea NFC iniciada, esperando tarjetas...");

    while (1) {
        int n = uart_read_bytes(UART_PORT, raw, sizeof(raw), pdMS_TO_TICKS(50));

        for (int i = 0; i < n; i++) {
            if (!parse_byte(raw[i], &state, payload,
                            &expected_len, &received, uid_str)) {
                continue;
            }

            /* ── UID detectado: aplicar debounce ── */
            TickType_t now = xTaskGetTickCount();
            if (strcasecmp(uid_str, last_uid) == 0 &&
                (now - last_time) < pdMS_TO_TICKS(NFC_DEBOUNCE_MS)) {
                continue;
            }
            strncpy(last_uid, uid_str, sizeof(last_uid) - 1);
            last_time = now;

            ESP_LOGI(TAG, "Tarjeta detectada: UID=%s", uid_str);

            /* ── Buscar tarjeta en NVS ── */
            char card_name[NAME_MAX_LEN] = "DESCONOCIDO";
            int  card_idx = storage_find_card(uid_str);

            if (card_idx >= 0) {
                /* Tarjeta autorizada */
                storage_get_card_name(uid_str, card_name, sizeof(card_name));
                ESP_LOGI(TAG, "Acceso CONCEDIDO: UID=%s Nombre=%s",
                         uid_str, card_name);

                /* Notificar ThingsBoard */
                tb_set_last_access(uid_str, card_name, "NFC", "ABIERTO");

                /* Abrir cajón */
                solenoid_cmd_t cmd = SOLENOID_CMD_OPEN;
                xQueueSend(solenoid_queue, &cmd, pdMS_TO_TICKS(100));

                /* Registrar en log */
                log_add_entry(uid_str, card_name, "NFC", "ABIERTO");

                /* Feedback visual */
                led_set(1, 0);
                vTaskDelay(pdMS_TO_TICKS(2000));
                led_set(0, 0);

            } else {
                /* Tarjeta no autorizada */
                ESP_LOGW(TAG, "Acceso DENEGADO: UID=%s", uid_str);

                /* Notificar ThingsBoard */
                tb_set_last_access(uid_str, "DESCONOCIDO", "NFC", "DENEGADO");

                /* Registrar en log */
                log_add_entry(uid_str, "DESCONOCIDO", "NFC", "DENEGADO");

                /* Feedback visual inmediato */
                led_set(0, 1);
                vTaskDelay(pdMS_TO_TICKS(2000));
                led_set(0, 0);

                /* Notificación Telegram (puede tardar hasta NOTIFY_TIMEOUT_MS) */
                char notif[72];
                snprintf(notif, sizeof(notif),
                         "⚠️ Acceso denegado: UID %s", uid_str);
                telegram_notify(notif);
            }
        }
    }
}
