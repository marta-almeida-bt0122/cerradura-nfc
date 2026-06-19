/**
 * thingsboard.c — Integración con ThingsBoard Demo via MQTT.
 *
 * Publica cada 30 s (o en cada evento) un JSON de telemetría:
 *   { "door_open", "battery_voltage", "last_uid", "last_card_name",
 *     "last_access_reason", "last_access_result", "lock_state" }
 *
 * Atributos de alarma en v1/devices/me/attributes:
 *   { "door_alarm": bool, "battery_alarm": bool, "battery_voltage": número }
 *
 * Token: CONFIG_TB_TOKEN (menuconfig). ADC: GPIO34, divisor 100kΩ+47kΩ.
 */

#include "thingsboard.h"
#include "wifi.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "cJSON.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "TB";

/* ── Hardware ───────────────────────────────────────────────────────────── */

#define GPIO_REED         21
#define ADC_CHANNEL       ADC_CHANNEL_6      /* GPIO34 → ADC1_CH6          */
#define ADC_ATTEN         ADC_ATTEN_DB_12    /* Rango 0–~3.1 V (ESP-IDF 5.x) */
#define ADC_BITWIDTH      ADC_BITWIDTH_12
#define ADC_SAMPLES       10

#define ADC_VREF          3.3f
#define ADC_R_UPPER       100.0f             /* kΩ (en serie con batería)   */
#define ADC_R_LOWER       47.0f             /* kΩ (a GND)                  */
#define ADC_MAX_RAW       4095.0f

/* ── MQTT / ThingsBoard ─────────────────────────────────────────────────── */

#define TB_BROKER_URI         "mqtt://demo.thingsboard.io:1883"
#define TB_TOPIC_TELEMETRY    "v1/devices/me/telemetry"
#define TB_TOPIC_ATTRIBUTES   "v1/devices/me/attributes"
#define TB_PUBLISH_PERIOD_MS  30000

#define MQTT_CONNECTED_BIT    BIT0

/* ── Estado compartido (protegido por s_mutex) ──────────────────────────── */

static SemaphoreHandle_t     s_mutex;
static SemaphoreHandle_t     s_publish_sem;
static EventGroupHandle_t    s_mqtt_eg;

static char  s_last_uid[24]        = "";
static char  s_last_card_name[36]  = "";
static char  s_last_reason[12]     = "";
static char  s_last_result[12]     = "";
static bool  s_lock_open           = false;

/* ── Estado de alarmas (solo desde task_thingsboard) ────────────────────── */

static bool        s_door_alarm    = false;
static bool        s_bat_alarm     = false;
static bool        s_door_timing   = false;
static TickType_t  s_door_tick     = 0;

/* ── Token y cliente MQTT ───────────────────────────────────────────────── */

static char                     s_tb_token[128] = "";
static esp_mqtt_client_handle_t s_mqtt_client   = NULL;

/* ── ADC ────────────────────────────────────────────────────────────────── */

static adc_oneshot_unit_handle_t s_adc_handle = NULL;

/* ══════════════════════════════════════════════════════════════════════════
 * Helpers internos
 * ══════════════════════════════════════════════════════════════════════════ */

static float read_battery_voltage(void)
{
    if (!s_adc_handle) return 0.0f;

    long sum = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        int raw = 0;
        adc_oneshot_read(s_adc_handle, ADC_CHANNEL, &raw);
        sum += raw;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    float avg  = (float)(sum / ADC_SAMPLES);
    float v_adc = (avg / ADC_MAX_RAW) * ADC_VREF;
    float v_bat  = v_adc * (ADC_R_UPPER + ADC_R_LOWER) / ADC_R_LOWER;
    ESP_LOGD(TAG, "ADC avg=%.0f  V_adc=%.3fV  V_bat=%.2fV", avg, v_adc, v_bat);
    return v_bat;
}

static void check_alarms(bool door_open, float v_bat)
{
    s_bat_alarm = (v_bat < TB_BATTERY_ALARM_V);

    if (door_open) {
        if (!s_door_timing) {
            s_door_tick   = xTaskGetTickCount();
            s_door_timing = true;
        }
        TickType_t elapsed = xTaskGetTickCount() - s_door_tick;
        s_door_alarm = (elapsed >= pdMS_TO_TICKS((uint32_t)TB_DOOR_ALARM_SEC * 1000U));
    } else {
        s_door_timing = false;
        s_door_alarm  = false;
    }
}

static void publish_telemetry(void)
{
    if (!(xEventGroupGetBits(s_mqtt_eg) & MQTT_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "MQTT sin conexión, telemetría pospuesta");
        return;
    }

    bool  door_open = (gpio_get_level(GPIO_REED) == 0);
    float v_bat     = read_battery_voltage();

    /* Copiar estado compartido bajo mutex */
    char uid_copy[24]   = "";
    char name_copy[36]  = "";
    char reas_copy[12]  = "";
    char res_copy[12]   = "";
    bool lock_is_open   = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(uid_copy,  s_last_uid,       sizeof(uid_copy)  - 1);
    strncpy(name_copy, s_last_card_name, sizeof(name_copy) - 1);
    strncpy(reas_copy, s_last_reason,    sizeof(reas_copy) - 1);
    strncpy(res_copy,  s_last_result,    sizeof(res_copy)  - 1);
    lock_is_open = s_lock_open;
    xSemaphoreGive(s_mutex);

    check_alarms(door_open, v_bat);

    /* ── Telemetría ── */
    cJSON *tel = cJSON_CreateObject();
    cJSON_AddBoolToObject(tel,   "door_open",           door_open);
    cJSON_AddNumberToObject(tel, "battery_voltage",     (double)v_bat);
    cJSON_AddStringToObject(tel, "last_uid",            uid_copy);
    cJSON_AddStringToObject(tel, "last_card_name",      name_copy);
    cJSON_AddStringToObject(tel, "last_access_reason",  reas_copy);
    cJSON_AddStringToObject(tel, "last_access_result",  res_copy);
    cJSON_AddStringToObject(tel, "lock_state",          lock_is_open ? "open" : "closed");
    char *tel_str = cJSON_PrintUnformatted(tel);
    cJSON_Delete(tel);

    if (tel_str) {
        int id = esp_mqtt_client_publish(s_mqtt_client, TB_TOPIC_TELEMETRY,
                                         tel_str, 0, 1, 0);
        if (id >= 0) {
            ESP_LOGI(TAG, "Telemetría publicada: %s", tel_str);
        } else {
            ESP_LOGW(TAG, "Error publicando telemetría (id=%d)", id);
        }
        cJSON_free(tel_str);
    }

    /* ── Atributos de alarma ── */
    cJSON *attr = cJSON_CreateObject();
    cJSON_AddBoolToObject(attr,   "door_alarm",       s_door_alarm);
    cJSON_AddBoolToObject(attr,   "battery_alarm",    s_bat_alarm);
    cJSON_AddNumberToObject(attr, "battery_voltage",  (double)v_bat);
    char *attr_str = cJSON_PrintUnformatted(attr);
    cJSON_Delete(attr);

    if (attr_str) {
        esp_mqtt_client_publish(s_mqtt_client, TB_TOPIC_ATTRIBUTES,
                                attr_str, 0, 1, 0);
        if (s_door_alarm || s_bat_alarm) {
            ESP_LOGW(TAG, "Alarmas activas: door=%d battery=%d",
                     s_door_alarm, s_bat_alarm);
        }
        cJSON_free(attr_str);
    }
}

/* ── Manejador MQTT ─────────────────────────────────────────────────────── */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;

    switch (ev->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT conectado a ThingsBoard");
        xEventGroupSetBits(s_mqtt_eg, MQTT_CONNECTED_BIT);
        xSemaphoreGive(s_publish_sem);  /* publicar inmediatamente al reconectar */
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT desconectado, reintentando...");
        xEventGroupClearBits(s_mqtt_eg, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_ERROR:
        if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "MQTT error TCP: esp_tls=%d",
                     ev->error_handle->esp_tls_last_esp_err);
        }
        break;

    default:
        break;
    }
}

/* ── Inicialización de subsistemas ──────────────────────────────────────── */

static void adc_setup(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH,
        .atten    = ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL, &chan_cfg));
    ESP_LOGI(TAG, "ADC inicializado: GPIO34 (ADC1_CH6), atten=12dB, 12 bits");
}

static void mqtt_setup(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char client_id[32];
    snprintf(client_id, sizeof(client_id),
             "ESP32_%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = { .uri = TB_BROKER_URI },
        },
        .credentials = {
            .client_id = client_id,
            .username  = s_tb_token,
        },
        .session = {
            .keepalive = 60,
        },
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));
    ESP_LOGI(TAG, "MQTT iniciado  broker=%s  client_id=%s", TB_BROKER_URI, client_id);
}

/* ══════════════════════════════════════════════════════════════════════════
 * API pública
 * ══════════════════════════════════════════════════════════════════════════ */

void tb_init(void)
{
    s_mutex       = xSemaphoreCreateMutex();
    s_publish_sem = xSemaphoreCreateBinary();
    s_mqtt_eg     = xEventGroupCreate();
    configASSERT(s_mutex);
    configASSERT(s_publish_sem);
    configASSERT(s_mqtt_eg);
}

void tb_set_last_access(const char *uid, const char *name,
                        const char *reason, const char *result)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_last_uid,       uid    ? uid    : "", sizeof(s_last_uid)       - 1);
    strncpy(s_last_card_name, name   ? name   : "", sizeof(s_last_card_name) - 1);
    strncpy(s_last_reason,    reason ? reason : "", sizeof(s_last_reason)    - 1);
    strncpy(s_last_result,    result ? result : "", sizeof(s_last_result)    - 1);
    xSemaphoreGive(s_mutex);
    tb_trigger_publish();
}

void tb_set_lock_state(bool is_open)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_lock_open = is_open;
    xSemaphoreGive(s_mutex);
    tb_trigger_publish();
}

void tb_trigger_publish(void)
{
    xSemaphoreGive(s_publish_sem);  /* binario: múltiples llamadas coalescen */
}

void task_thingsboard(void *pvParameters)
{
    /* Token desde menuconfig (compilado en firmware) */
    strncpy(s_tb_token, CONFIG_TB_TOKEN, sizeof(s_tb_token) - 1);

    if (strlen(s_tb_token) == 0) {
        ESP_LOGE(TAG, "Sin token ThingsBoard. Configura CONFIG_TB_TOKEN en menuconfig.");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Token ThingsBoard cargado (%.8s...)", s_tb_token);

    /* Esperar conexión WiFi antes de iniciar MQTT */
    while (!wifi_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    adc_setup();
    mqtt_setup();

    ESP_LOGI(TAG, "Tarea ThingsBoard iniciada (periodo=%ds, alarma_puerta=%ds)",
             TB_PUBLISH_PERIOD_MS / 1000, TB_DOOR_ALARM_SEC);

    while (1) {
        xSemaphoreTake(s_publish_sem, pdMS_TO_TICKS(TB_PUBLISH_PERIOD_MS));

        if (!wifi_is_connected()) {
            ESP_LOGD(TAG, "WiFi caído, posponiendo telemetría");
            continue;
        }

        publish_telemetry();
    }
}
