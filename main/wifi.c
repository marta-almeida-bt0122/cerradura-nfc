#include "wifi.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "WIFI";

#define WIFI_CONNECTED_BIT  BIT0

static EventGroupHandle_t s_wifi_eg;
static volatile bool      s_connected = false;

/* ── Manejador de eventos ──────────────────────────────────────────────── */

static void event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Iniciando conexión WiFi...");
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *info =
            (wifi_event_sta_disconnected_t *)event_data;
        s_connected = false;
        xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "WiFi desconectado (razón=%d), reconectando...", info->reason);
        esp_wifi_connect();  /* reconexión indefinida sin límite de intentos */

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Conectado. IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

/* ── API pública ───────────────────────────────────────────────────────── */

esp_err_t wifi_init_sta(void)
{
    s_wifi_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    /* Credenciales desde menuconfig (compiladas en el firmware) */
    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid,
            CONFIG_WIFI_SSID,     sizeof(wifi_cfg.sta.ssid)     - 1);
    strncpy((char *)wifi_cfg.sta.password,
            CONFIG_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode =
        (strlen(CONFIG_WIFI_PASSWORD) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_LOGI(TAG, "Conectando a SSID: '%s'", CONFIG_WIFI_SSID);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return s_connected;
}

void wifi_wait_connected(void)
{
    ESP_LOGI(TAG, "Esperando conexión WiFi (reintento indefinido)...");
    xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT,
                        pdFALSE,   /* no limpiar el bit */
                        pdFALSE,   /* cualquiera de los bits */
                        portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi listo");
}
