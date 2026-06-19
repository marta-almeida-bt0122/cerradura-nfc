#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Inicializa el módulo WiFi en modo STA y comienza la conexión.
 * Lee credenciales directamente de CONFIG_WIFI_SSID / CONFIG_WIFI_PASSWORD
 * (valores de menuconfig). Llama a esp_netif_init() y
 * esp_event_loop_create_default() internamente.
 */
esp_err_t wifi_init_sta(void);

/** Retorna true si hay conexión activa con IP asignada. */
bool wifi_is_connected(void);

/**
 * Bloquea hasta que la conexión WiFi esté establecida.
 * Si las credenciales son incorrectas, reintenta indefinidamente.
 * No retorna hasta tener IP.
 */
void wifi_wait_connected(void);
