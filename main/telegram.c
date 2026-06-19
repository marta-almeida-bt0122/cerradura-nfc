/**
 * telegram.c — Bot de Telegram para control remoto de la cerradura.
 *
 * Credenciales (CONFIG_TELEGRAM_TOKEN, CONFIG_TELEGRAM_CHAT_ID) compiladas
 * directamente desde menuconfig. Sin lectura de NVS para tokens.
 *
 * Long-polling (getUpdates?timeout=30). TLS verificado con bundle mbedTLS.
 */

#include "telegram.h"
#include "solenoid.h"
#include "storage.h"
#include "thingsboard.h"
#include "wifi.h"
#include "driver/gpio.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "TELEGRAM";

#define GPIO_REED          21
#define HTTP_BUF_SIZE      8192
#define GETUP_TIMEOUT_MS   35000   /* > 30 s de long-poll */
#define SEND_TIMEOUT_MS    10000
#define NOTIFY_TIMEOUT_MS   5000   /* para telegram_notify (llamada externa) */

/* Buffer estático para acumular respuestas HTTP del long-polling */
static char s_http_buf[HTTP_BUF_SIZE];
static int  s_http_len;

/* ── Manejador de eventos HTTP ─────────────────────────────────────────── */

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA &&
        !esp_http_client_is_chunked_response(evt->client)) {
        int space = HTTP_BUF_SIZE - 1 - s_http_len;
        if (space > 0) {
            int copy = (evt->data_len < space) ? evt->data_len : space;
            memcpy(s_http_buf + s_http_len, evt->data, copy);
            s_http_len += copy;
            s_http_buf[s_http_len] = '\0';
        } else {
            ESP_LOGW(TAG, "Buffer HTTP lleno, respuesta truncada");
        }
    }
    return ESP_OK;
}

/* ── Envío de mensaje (solo desde task_telegram) ────────────────────────── */

static void send_message(const char *text)
{
    if (!text || strlen(CONFIG_TELEGRAM_TOKEN) == 0) return;

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.telegram.org/bot%s/sendMessage",
             CONFIG_TELEGRAM_TOKEN);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "chat_id", CONFIG_TELEGRAM_CHAT_ID);
    cJSON_AddStringToObject(body, "text",    text);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return;

    s_http_len = 0;
    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = SEND_TIMEOUT_MS,
        .event_handler     = http_event_cb,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body_str, (int)strlen(body_str));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error enviando mensaje: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    cJSON_free(body_str);
}

/* ── Procesamiento de comandos ─────────────────────────────────────────── */

static void handle_command(const char *text)
{
    char reply[1024];

    /* /abrir */
    if (strcmp(text, "/abrir") == 0) {
        solenoid_cmd_t cmd = SOLENOID_CMD_OPEN;
        xQueueSend(solenoid_queue, &cmd, pdMS_TO_TICKS(200));
        log_add_entry("", "Bot Telegram", "Telegram", "ABIERTO");
        tb_set_last_access("", "Bot Telegram", "Telegram", "ABIERTO");
        send_message("Cajón abierto.");

    /* /cerrar */
    } else if (strcmp(text, "/cerrar") == 0) {
        solenoid_cmd_t cmd = SOLENOID_CMD_CLOSE;
        xQueueSend(solenoid_queue, &cmd, pdMS_TO_TICKS(200));
        log_add_entry("", "Bot Telegram", "Telegram", "CERRADO");
        tb_set_last_access("", "Bot Telegram", "Telegram", "CERRADO");
        send_message("Cajón cerrado.");

    /* /estado */
    } else if (strcmp(text, "/estado") == 0) {
        bool abierto = (gpio_get_level(GPIO_REED) == 0);
        snprintf(reply, sizeof(reply),
                 "Estado del cajón: %s", abierto ? "ABIERTO" : "CERRADO");
        send_message(reply);

    /* /adduid <UID> <nombre> */
    } else if (strncmp(text, "/adduid ", 8) == 0) {
        const char *args = text + 8;
        while (*args == ' ') args++;

        const char *space = strchr(args, ' ');
        if (!space || space == args) {
            send_message("Uso: /adduid <UID> <nombre>\n"
                         "Ejemplo: /adduid A1B2C3D4 Tarjeta_Marta");
            return;
        }

        char uid[UID_MAX_LEN] = {0};
        size_t uid_len = (size_t)(space - args);
        if (uid_len >= UID_MAX_LEN) uid_len = UID_MAX_LEN - 1;
        strncpy(uid, args, uid_len);

        const char *name = space + 1;
        while (*name == ' ') name++;
        if (strlen(name) == 0) {
            send_message("Uso: /adduid <UID> <nombre>\n"
                         "Ejemplo: /adduid A1B2C3D4 Tarjeta_Marta");
            return;
        }

        char name_trunc[NAME_MAX_LEN];
        strncpy(name_trunc, name, sizeof(name_trunc) - 1);
        name_trunc[sizeof(name_trunc) - 1] = '\0';

        esp_err_t err = storage_add_card(uid, name_trunc);
        if (err == ESP_OK) {
            snprintf(reply, sizeof(reply),
                     "Tarjeta añadida:\nUID: %s\nNombre: %s", uid, name_trunc);
        } else if (err == ESP_ERR_NO_MEM) {
            snprintf(reply, sizeof(reply), "Lista llena (máx. %d tarjetas).", MAX_CARDS);
        } else {
            snprintf(reply, sizeof(reply), "Error añadiendo tarjeta: %s",
                     esp_err_to_name(err));
        }
        send_message(reply);

    /* /removeuid <UID> */
    } else if (strncmp(text, "/removeuid ", 11) == 0) {
        const char *uid = text + 11;
        while (*uid == ' ') uid++;
        if (strlen(uid) == 0) {
            send_message("Uso: /removeuid <UID>");
            return;
        }
        esp_err_t err = storage_remove_card(uid);
        if (err == ESP_OK) {
            snprintf(reply, sizeof(reply), "Tarjeta '%s' eliminada.", uid);
        } else if (err == ESP_ERR_NOT_FOUND) {
            snprintf(reply, sizeof(reply), "UID '%s' no encontrado.", uid);
        } else {
            snprintf(reply, sizeof(reply), "Error: %s", esp_err_to_name(err));
        }
        send_message(reply);

    /* /listar */
    } else if (strcmp(text, "/listar") == 0) {
        send_message(storage_list_cards());

    /* /log */
    } else if (strcmp(text, "/log") == 0) {
        send_message(log_get_last(10));

    /* /version */
    } else if (strcmp(text, "/version") == 0) {
        snprintf(reply, sizeof(reply), "Firmware versión: %s", CONFIG_FW_VERSION);
        send_message(reply);

    } else {
        send_message(
            "Comandos disponibles:\n"
            "/abrir\n"
            "/cerrar\n"
            "/estado\n"
            "/adduid <UID> <nombre>\n"
            "/removeuid <UID>\n"
            "/listar\n"
            "/log\n"
            "/version");
    }
}

/* ── Long-polling ──────────────────────────────────────────────────────── */

static long long get_updates(long long offset)
{
    if (strlen(CONFIG_TELEGRAM_TOKEN) == 0) return -1;

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.telegram.org/bot%s/getUpdates?offset=%lld&timeout=30",
             CONFIG_TELEGRAM_TOKEN, offset);

    s_http_len = 0;
    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = GETUP_TIMEOUT_MS,
        .event_handler     = http_event_cb,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "getUpdates error: %s", esp_err_to_name(err));
        return -1;
    }
    if (s_http_len == 0) return -1;

    cJSON *root = cJSON_Parse(s_http_buf);
    if (!root) {
        ESP_LOGE(TAG, "JSON inválido en respuesta de getUpdates");
        return -1;
    }

    cJSON *ok_item = cJSON_GetObjectItem(root, "ok");
    if (!cJSON_IsTrue(ok_item)) {
        ESP_LOGE(TAG, "Telegram respondió ok=false");
        cJSON_Delete(root);
        return -1;
    }

    long long new_offset = offset;
    cJSON *result = cJSON_GetObjectItem(root, "result");
    int n = cJSON_GetArraySize(result);

    for (int i = 0; i < n; i++) {
        cJSON *update    = cJSON_GetArrayItem(result, i);
        cJSON *update_id = cJSON_GetObjectItem(update, "update_id");
        if (!cJSON_IsNumber(update_id)) continue;

        long long uid_val = (long long)update_id->valuedouble;
        if (uid_val + 1 > new_offset) new_offset = uid_val + 1;

        cJSON *message = cJSON_GetObjectItem(update, "message");
        if (!message) continue;

        cJSON *chat    = cJSON_GetObjectItem(message, "chat");
        cJSON *chat_id = cJSON_GetObjectItem(chat,    "id");
        if (!cJSON_IsNumber(chat_id)) continue;

        char chat_id_str[32];
        snprintf(chat_id_str, sizeof(chat_id_str), "%.0f", chat_id->valuedouble);
        if (strcmp(chat_id_str, CONFIG_TELEGRAM_CHAT_ID) != 0) {
            ESP_LOGW(TAG, "Mensaje ignorado de chat_id no autorizado: %s", chat_id_str);
            continue;
        }

        cJSON *text_j = cJSON_GetObjectItem(message, "text");
        if (!cJSON_IsString(text_j) || !text_j->valuestring) continue;

        ESP_LOGI(TAG, "Comando recibido: '%s'", text_j->valuestring);
        handle_command(text_j->valuestring);
    }

    cJSON_Delete(root);
    return new_offset;
}

/* ── API pública ───────────────────────────────────────────────────────── */

void telegram_notify(const char *text)
{
    if (!text || !wifi_is_connected() || strlen(CONFIG_TELEGRAM_TOKEN) == 0) return;

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.telegram.org/bot%s/sendMessage",
             CONFIG_TELEGRAM_TOKEN);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "chat_id", CONFIG_TELEGRAM_CHAT_ID);
    cJSON_AddStringToObject(body, "text",    text);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return;

    /* Cliente HTTP independiente: no comparte s_http_buf con el long-polling */
    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = NOTIFY_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body_str, (int)strlen(body_str));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "telegram_notify error: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    cJSON_free(body_str);
}

void task_telegram(void *pvParameters)
{
    if (strlen(CONFIG_TELEGRAM_TOKEN) == 0) {
        ESP_LOGE(TAG, "Token de Telegram vacío. Configura CONFIG_TELEGRAM_TOKEN en menuconfig.");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Tarea Telegram iniciada (chat_id=%s, token=%.10s...)",
             CONFIG_TELEGRAM_CHAT_ID, CONFIG_TELEGRAM_TOKEN);

    long long offset = 0;

    while (1) {
        if (!wifi_is_connected()) {
            ESP_LOGW(TAG, "WiFi no disponible, esperando...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        long long new_offset = get_updates(offset);
        if (new_offset >= 0) {
            offset = new_offset;
        } else {
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}
