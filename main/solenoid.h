#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Comandos enviados a la queue del solenoide */
typedef enum {
    SOLENOID_CMD_OPEN = 0,   /* Pulso DIR=HIGH → abre el cajón */
    SOLENOID_CMD_CLOSE,      /* Pulso DIR=LOW  → cierra el cajón */
} solenoid_cmd_t;

/**
 * Queue para enviar comandos a task_solenoid.
 * Declarada extern aquí; definida e inicializada en solenoid.c.
 * Disponible tras llamar a solenoid_init().
 */
extern QueueHandle_t solenoid_queue;

/** Inicializa los GPIOs del puente H y crea la queue. */
void solenoid_init(void);

/** Tarea FreeRTOS: espera comandos en solenoid_queue y activa el solenoide. */
void task_solenoid(void *pvParameters);
