#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/** Inicializa UART2 y los GPIOs de los LEDs. */
void nfc_init(void);

/**
 * Tarea FreeRTOS: lee continuamente UART2, parsea tramas del Pepper C1,
 * extrae UIDs y gestiona acceso (solenoide + LEDs).
 */
void task_nfc(void *pvParameters);
