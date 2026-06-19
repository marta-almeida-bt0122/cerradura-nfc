#include "storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <time.h>

static const char *TAG = "STORAGE";

#define CARDS_NAMESPACE  "nfc_cards"   /* Tarjetas NFC autorizadas */
#define LOG_NAMESPACE    "access_log"  /* Log circular de accesos  */

/* Buffers estáticos para las funciones que devuelven cadenas */
static char s_list_buf[1024];
static char s_log_buf[2048];

/* ══════════════════════════════════════════════════════════════════════════
 * Inicialización NVS
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t storage_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS dañada o versión nueva, borrando partición...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS inicializada correctamente");
    } else {
        ESP_LOGE(TAG, "Error inicializando NVS: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Helpers internos
 * ══════════════════════════════════════════════════════════════════════════ */

/* Lee el contador de tarjetas; retorna 0 si no existe aún. */
static int cards_get_count(nvs_handle_t h)
{
    uint32_t cnt = 0;
    nvs_get_u32(h, "count", &cnt);  /* error ignorado → valor por defecto 0 */
    return (int)cnt;
}

/* Busca uid en el handle ya abierto. Retorna índice o -1. */
static int cards_find_in_handle(nvs_handle_t h, int count, const char *uid)
{
    char key[32], stored[UID_MAX_LEN];
    for (int i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "u_%d", i);
        size_t len = sizeof(stored);
        if (nvs_get_str(h, key, stored, &len) == ESP_OK) {
            if (strcasecmp(stored, uid) == 0) return i;
        }
    }
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Gestión de tarjetas NFC
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t storage_add_card(const char *uid, const char *name)
{
    if (!uid || !name || strlen(uid) == 0 || strlen(uid) >= UID_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(CARDS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    int count = cards_get_count(h);
    int idx   = cards_find_in_handle(h, count, uid);

    char key[32];

    if (idx >= 0) {
        /* UID ya existe: actualizar solo el nombre */
        snprintf(key, sizeof(key), "n_%d", idx);
        err = nvs_set_str(h, key, name);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Tarjeta '%s' actualizada con nombre '%s'", uid, name);
        return err;
    }

    if (count >= MAX_CARDS) {
        ESP_LOGE(TAG, "Lista llena (%d tarjetas)", MAX_CARDS);
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }

    /* Nueva tarjeta */
    snprintf(key, sizeof(key), "u_%d",  count);
    err = nvs_set_str(h, key, uid);

    snprintf(key, sizeof(key), "n_%d", count);
    if (err == ESP_OK) err = nvs_set_str(h, key, name);

    snprintf(key, sizeof(key), "t_%d",   count);
    if (err == ESP_OK) err = nvs_set_u32(h, key, (uint32_t)time(NULL));

    if (err == ESP_OK) err = nvs_set_u32(h, "count", (uint32_t)(count + 1));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Tarjeta añadida: UID='%s' nombre='%s' (total: %d)",
                 uid, name, count + 1);
    }
    return err;
}

esp_err_t storage_remove_card(const char *uid)
{
    if (!uid || strlen(uid) == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(CARDS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    int count = cards_get_count(h);
    int found = cards_find_in_handle(h, count, uid);

    if (found < 0) {
        ESP_LOGW(TAG, "UID '%s' no encontrado", uid);
        nvs_close(h);
        return ESP_ERR_NOT_FOUND;
    }

    /* Desplazar entradas posteriores para cerrar el hueco */
    char key[32], next[32];
    char stored_uid[UID_MAX_LEN], stored_name[NAME_MAX_LEN];
    uint32_t stored_ts;
    size_t len;

    for (int i = found; i < count - 1; i++) {
        snprintf(next, sizeof(next), "u_%d",  i + 1);
        len = sizeof(stored_uid); stored_uid[0] = '\0';
        nvs_get_str(h, next, stored_uid, &len);

        snprintf(next, sizeof(next), "n_%d", i + 1);
        len = sizeof(stored_name); stored_name[0] = '\0';
        nvs_get_str(h, next, stored_name, &len);

        snprintf(next, sizeof(next), "t_%d",   i + 1);
        stored_ts = 0;
        nvs_get_u32(h, next, &stored_ts);

        snprintf(key, sizeof(key), "u_%d",  i);
        nvs_set_str(h, key, stored_uid);
        snprintf(key, sizeof(key), "n_%d", i);
        nvs_set_str(h, key, stored_name);
        snprintf(key, sizeof(key), "t_%d",   i);
        nvs_set_u32(h, key, stored_ts);
    }

    /* Borrar la última posición sobrante */
    snprintf(key, sizeof(key), "u_%d",  count - 1); nvs_erase_key(h, key);
    snprintf(key, sizeof(key), "n_%d", count - 1); nvs_erase_key(h, key);
    snprintf(key, sizeof(key), "t_%d",   count - 1); nvs_erase_key(h, key);

    err = nvs_set_u32(h, "count", (uint32_t)(count - 1));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Tarjeta '%s' eliminada (quedan: %d)", uid, count - 1);
    }
    return err;
}

int storage_find_card(const char *uid)
{
    if (!uid || strlen(uid) == 0) return -1;

    nvs_handle_t h;
    if (nvs_open(CARDS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return -1;

    int count = cards_get_count(h);
    int idx   = cards_find_in_handle(h, count, uid);
    nvs_close(h);
    return idx;
}

esp_err_t storage_get_card_name(const char *uid, char *name, size_t name_len)
{
    if (!uid || !name || name_len == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(CARDS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) { name[0] = '\0'; return err; }

    int count = cards_get_count(h);
    int idx   = cards_find_in_handle(h, count, uid);

    if (idx < 0) {
        nvs_close(h);
        name[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }

    char key[32];
    snprintf(key, sizeof(key), "n_%d", idx);
    err = nvs_get_str(h, key, name, &name_len);
    nvs_close(h);
    return err;
}

const char *storage_list_cards(void)
{
    nvs_handle_t h;
    if (nvs_open(CARDS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        snprintf(s_list_buf, sizeof(s_list_buf), "Error accediendo a NVS.");
        return s_list_buf;
    }

    int count = cards_get_count(h);

    if (count == 0) {
        nvs_close(h);
        snprintf(s_list_buf, sizeof(s_list_buf), "No hay tarjetas registradas.");
        return s_list_buf;
    }

    snprintf(s_list_buf, sizeof(s_list_buf),
             "Tarjetas registradas (%d/%d):\n", count, MAX_CARDS);

    char key[32], uid[UID_MAX_LEN], name[NAME_MAX_LEN];
    size_t len;

    for (int i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "u_%d", i);
        len = sizeof(uid); uid[0] = '\0';
        nvs_get_str(h, key, uid, &len);

        snprintf(key, sizeof(key), "n_%d", i);
        len = sizeof(name); name[0] = '\0';
        nvs_get_str(h, key, name, &len);

        char line[72];
        snprintf(line, sizeof(line), "  %d. %s — %s\n", i + 1, uid, name);
        strncat(s_list_buf, line, sizeof(s_list_buf) - strlen(s_list_buf) - 1);
    }

    nvs_close(h);
    return s_list_buf;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Log circular de accesos
 * ══════════════════════════════════════════════════════════════════════════ */

void log_add_entry(const char *uid, const char *name,
                   const char *motivo, const char *resultado)
{
    nvs_handle_t h;
    if (nvs_open(LOG_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;

    uint32_t total = 0;
    nvs_get_u32(h, "total", &total);  /* primera vez devuelve NOT_FOUND → total=0 */

    int slot = (int)(total % (uint32_t)MAX_LOG_ENTRIES);
    char key[32];

    snprintf(key, sizeof(key), "u_%d",  slot);
    nvs_set_str(h, key, uid     ? uid     : "");

    snprintf(key, sizeof(key), "n_%d", slot);
    nvs_set_str(h, key, name    ? name    : "");

    snprintf(key, sizeof(key), "mot_%d",  slot);
    nvs_set_str(h, key, motivo  ? motivo  : "");

    snprintf(key, sizeof(key), "res_%d",  slot);
    nvs_set_str(h, key, resultado ? resultado : "");

    snprintf(key, sizeof(key), "t_%d",   slot);
    nvs_set_u32(h, key, (uint32_t)time(NULL));

    nvs_set_u32(h, "total", total + 1);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Log[%d]: %s | %s | %s | %s",
             slot,
             uid      ? uid      : "",
             name     ? name     : "",
             motivo   ? motivo   : "",
             resultado ? resultado : "");
}

const char *log_get_last(int n)
{
    if (n <= 0) { s_log_buf[0] = '\0'; return s_log_buf; }
    if (n > MAX_LOG_ENTRIES) n = MAX_LOG_ENTRIES;

    nvs_handle_t h;
    if (nvs_open(LOG_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        snprintf(s_log_buf, sizeof(s_log_buf), "Error accediendo al log.");
        return s_log_buf;
    }

    uint32_t total = 0;
    nvs_get_u32(h, "total", &total);

    /* Entradas disponibles: como máximo MAX_LOG_ENTRIES */
    int avail = (int)((total < (uint32_t)MAX_LOG_ENTRIES)
                      ? total
                      : (uint32_t)MAX_LOG_ENTRIES);
    if (n > avail) n = avail;

    if (n == 0) {
        nvs_close(h);
        snprintf(s_log_buf, sizeof(s_log_buf), "El log de accesos está vacío.");
        return s_log_buf;
    }

    snprintf(s_log_buf, sizeof(s_log_buf), "Últimos %d accesos:\n", n);

    char key[32], uid[UID_MAX_LEN], name[NAME_MAX_LEN], mot[12], res[12];
    uint32_t ts;
    size_t len;

    for (int i = 0; i < n; i++) {
        /* Slot de la entrada más reciente primero */
        int slot = (int)((total - 1u - (uint32_t)i) % (uint32_t)MAX_LOG_ENTRIES);

        snprintf(key, sizeof(key), "u_%d",  slot);
        len = sizeof(uid);  uid[0]  = '\0'; nvs_get_str(h, key, uid,  &len);

        snprintf(key, sizeof(key), "n_%d", slot);
        len = sizeof(name); name[0] = '\0'; nvs_get_str(h, key, name, &len);

        snprintf(key, sizeof(key), "mot_%d",  slot);
        len = sizeof(mot);  mot[0]  = '\0'; nvs_get_str(h, key, mot,  &len);

        snprintf(key, sizeof(key), "res_%d",  slot);
        len = sizeof(res);  res[0]  = '\0'; nvs_get_str(h, key, res,  &len);

        snprintf(key, sizeof(key), "t_%d",   slot);
        ts = 0; nvs_get_u32(h, key, &ts);

        /* Formatear timestamp (requiere NTP para fechas reales) */
        char time_str[18] = "---";
        if (ts > 0) {
            time_t t = (time_t)ts;
            struct tm *tm_info = localtime(&t);
            if (tm_info) {
                strftime(time_str, sizeof(time_str), "%d/%m %H:%M", tm_info);
            }
        }

        char line[128];
        snprintf(line, sizeof(line), "%d. [%s] %s | %s | %s | %s\n",
                 i + 1, time_str,
                 uid[0]  ? uid  : "-",
                 name[0] ? name : "-",
                 mot[0]  ? mot  : "-",
                 res[0]  ? res  : "-");
        strncat(s_log_buf, line, sizeof(s_log_buf) - strlen(s_log_buf) - 1);
    }

    nvs_close(h);
    return s_log_buf;
}
