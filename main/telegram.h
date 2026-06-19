#pragma once

/**
 * Tarea FreeRTOS: hace long-polling a la API de Telegram, procesa comandos
 * y envía respuestas de confirmación.
 * Solo acepta comandos del chat_id configurado en menuconfig
 * (CONFIG_TELEGRAM_CHAT_ID).
 */
void task_telegram(void *pvParameters);

/**
 * Envía un mensaje de notificación al chat ID configurado.
 * Crea su propio cliente HTTP independiente del long-polling.
 * Puede llamarse desde cualquier tarea FreeRTOS.
 * No hace nada si WiFi no está disponible o el token está vacío.
 */
void telegram_notify(const char *text);
