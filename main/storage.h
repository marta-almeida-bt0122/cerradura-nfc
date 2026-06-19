#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define MAX_CARDS       20
#define MAX_LOG_ENTRIES 20
#define UID_MAX_LEN     20   /* "AABBCCDDEEFF00\0" = 15 bytes; margen extra */
#define NAME_MAX_LEN    32   /* Nombre o descripción de la tarjeta */

/**
 * Inicializa NVS. Borra la partición si está corrupta o tiene versión nueva.
 * Debe llamarse antes que cualquier otra función de este módulo.
 */
esp_err_t storage_init(void);

/* ══════════════════════════════════════════════════════════════════════════
 * Gestión de tarjetas NFC  (namespace NVS "nfc_cards")
 *
 * Claves por tarjeta (índice 0..MAX_CARDS-1):
 *   "uid_N"  → UID en string hexadecimal (ej. "A1B2C3D4")
 *   "name_N" → Nombre o descripción
 *   "ts_N"   → Timestamp Unix de registro (uint32)
 *   "count"  → Número total de tarjetas registradas (uint32)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Añade una tarjeta con nombre. Si el UID ya existe, actualiza el nombre.
 * Retorna ESP_ERR_NO_MEM si la lista está llena (MAX_CARDS).
 */
esp_err_t storage_add_card(const char *uid, const char *name);

/**
 * Elimina la tarjeta con ese UID (comparación sin distinción de mayúsculas).
 * Retorna ESP_ERR_NOT_FOUND si el UID no existe.
 */
esp_err_t storage_remove_card(const char *uid);

/**
 * Busca un UID en la lista (sin distinción de mayúsculas).
 * Retorna el índice (0..N-1) o -1 si no está autorizado.
 */
int storage_find_card(const char *uid);

/**
 * Copia el nombre de la tarjeta con ese UID en 'name'.
 * Retorna ESP_ERR_NOT_FOUND si el UID no existe.
 */
esp_err_t storage_get_card_name(const char *uid, char *name, size_t name_len);

/**
 * Construye una cadena con la lista completa de tarjetas registradas.
 * Formato por línea: "N. UID - Nombre\n"
 * El buffer es estático; no llamar desde varias tareas simultáneamente.
 */
const char *storage_list_cards(void);

/* ══════════════════════════════════════════════════════════════════════════
 * Log circular de accesos  (namespace NVS "access_log")
 *
 * Almacena los últimos MAX_LOG_ENTRIES accesos (NFC o Telegram).
 * Claves por entrada (slot = total_entries % MAX_LOG_ENTRIES):
 *   "uid_N"   → UID de la tarjeta (puede ser "" si origen es Telegram)
 *   "name_N"  → Nombre de la tarjeta o "DESCONOCIDO"
 *   "mot_N"   → Motivo: "NFC" o "Telegram"
 *   "res_N"   → Resultado: "ABIERTO" o "DENEGADO"
 *   "ts_N"    → Timestamp Unix (uint32)
 *   "total"   → Total de entradas escritas desde el inicio (uint32)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Añade una entrada al log circular. Segura desde cualquier tarea FreeRTOS
 * (NVS usa bloqueo interno de partición).
 *
 * @param uid       UID de la tarjeta o "" si origen es Telegram
 * @param name      Nombre de la tarjeta o "DESCONOCIDO"
 * @param motivo    "NFC" o "Telegram"
 * @param resultado "ABIERTO" o "DENEGADO"
 */
void log_add_entry(const char *uid, const char *name,
                   const char *motivo, const char *resultado);

/**
 * Retorna una cadena con los últimos 'n' accesos (máx. MAX_LOG_ENTRIES).
 * Formato por línea: "N. [dd/mm HH:MM] UID | Nombre | Motivo | Resultado\n"
 * El buffer es estático; no llamar desde varias tareas simultáneamente.
 */
const char *log_get_last(int n);
