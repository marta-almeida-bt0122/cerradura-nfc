#pragma once

#include <stdbool.h>

/* ── Umbrales de alarma ─────────────────────────────────────────────────── */

/** Tensión mínima de batería (V) antes de disparar battery_alarm. */
#define TB_BATTERY_ALARM_V   7.0f

/** Tiempo (segundos) con cajón abierto antes de disparar door_alarm. */
#define TB_DOOR_ALARM_SEC    60

/**
 * Inicializa los primitivos FreeRTOS compartidos (mutex, semáforo, event group).
 * DEBE llamarse en app_main ANTES de solenoid_init() y nfc_init() para
 * garantizar que tb_set_last_access() y tb_set_lock_state() sean siempre seguros.
 */
void tb_init(void);

/**
 * Tarea FreeRTOS: gestiona la conexión MQTT a ThingsBoard y publica
 * telemetría cada 30 s o en cada evento (llamada a tb_trigger_publish).
 * Stack recomendado: 6144 bytes.
 */
void task_thingsboard(void *pvParameters);

/**
 * Registra el último acceso (UID, nombre, motivo, resultado) y dispara
 * una publicación inmediata de telemetría a ThingsBoard.
 * Segura desde cualquier tarea FreeRTOS.
 *
 * @param uid     UID de la tarjeta o "" si el origen es Telegram
 * @param name    Nombre de la tarjeta o "DESCONOCIDO"
 * @param reason  "NFC" o "Telegram"
 * @param result  "ABIERTO" o "DENEGADO"
 */
void tb_set_last_access(const char *uid, const char *name,
                        const char *reason, const char *result);

/**
 * Actualiza el estado del solenoide (cerradura) y dispara una publicación.
 * Llamar desde task_solenoid tras cada actuación.
 * Segura desde cualquier tarea FreeRTOS.
 */
void tb_set_lock_state(bool is_open);

/**
 * Señala a task_thingsboard que publique telemetría sin esperar el periodo
 * de 30 s. Semáforo binario: múltiples llamadas coalescen en un solo ciclo.
 */
void tb_trigger_publish(void);
