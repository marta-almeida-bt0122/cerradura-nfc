#include "solenoid.h"
#include "thingsboard.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SOLENOID";

/* ── Pines del puente H L298N ───────────────────────────────────────────── */
#define GPIO_DIR    18   /* Dirección: HIGH = abrir, LOW = cerrar (GPIO normal) */
#define GPIO_EN     19   /* Enable: controlado por LEDC PWM para reducir tensión */

/* ── Configuración LEDC para PWM en EN ─────────────────────────────────── */
#define LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_TIMER      LEDC_TIMER_0
#define LEDC_CHANNEL    LEDC_CHANNEL_0
#define LEDC_FREQ_HZ    1000               /* Frecuencia PWM: 1 kHz            */
#define LEDC_RES_BITS   LEDC_TIMER_13_BIT  /* Resolución: 13 bits (0–8191)     */
#define LEDC_DUTY_MAX   8191               /* 2^13 - 1                          */

/* Duty cycle para limitar 9 V → 6 V efectivos:
 * 6 / 9 = 0.6667 → 67 % × 8191 = 5487.97 ≈ 5488 */
#define LEDC_DUTY_67PCT 5488

/* Duración del pulso de actuación del solenoide latching SK0730 */
#define PULSE_MS        150

QueueHandle_t solenoid_queue;

/* ── Helpers internos ──────────────────────────────────────────────────── */

/** Activa el PWM en EN al duty indicado y lo para al terminar el pulso. */
static void solenoid_pulse(int dir_level)
{
    /* 1. Establecer dirección */
    gpio_set_level(GPIO_DIR, dir_level);

    /* 2. Activar PWM al 67 % (9 V × 0.67 ≈ 6 V efectivos) */
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, LEDC_DUTY_67PCT));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));

    /* 3. Mantener durante el tiempo de actuación */
    vTaskDelay(pdMS_TO_TICKS(PULSE_MS));

    /* 4. Apagar PWM (duty = 0 → EN a LOW) */
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));
}

/* ── API pública ───────────────────────────────────────────────────────── */

void solenoid_init(void)
{
    /* ── Configurar LEDC para el pin EN (PWM) ── */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_RES_BITS,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t channel_cfg = {
        .speed_mode = LEDC_MODE,
        .channel    = LEDC_CHANNEL,
        .timer_sel  = LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = GPIO_EN,
        .duty       = 0,      /* Iniciar con EN apagado */
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));

    /* ── Configurar DIR como GPIO de salida normal ── */
    gpio_config_t dir_io = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << GPIO_DIR),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&dir_io));
    gpio_set_level(GPIO_DIR, 0);

    /* ── Queue de comandos ── */
    solenoid_queue = xQueueCreate(5, sizeof(solenoid_cmd_t));
    if (!solenoid_queue) {
        ESP_LOGE(TAG, "Error creando queue del solenoide");
    }

    ESP_LOGI(TAG, "Solenoide configurado: 9V -> 6V via PWM 67%%"
                  "  (DIR=GPIO%d, EN=GPIO%d, %dHz, duty=%d/%d)",
             GPIO_DIR, GPIO_EN, LEDC_FREQ_HZ, LEDC_DUTY_67PCT, LEDC_DUTY_MAX);
}

void task_solenoid(void *pvParameters)
{
    solenoid_cmd_t cmd;
    while (1) {
        if (xQueueReceive(solenoid_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            switch (cmd) {
            case SOLENOID_CMD_OPEN:
                ESP_LOGI(TAG, "ABRIENDO cajón");
                solenoid_pulse(1);           /* DIR=HIGH, pulso PWM 67 %% */
                tb_set_lock_state(true);     /* Notificar a ThingsBoard */
                break;
            case SOLENOID_CMD_CLOSE:
                ESP_LOGI(TAG, "CERRANDO cajón");
                solenoid_pulse(0);           /* DIR=LOW, pulso PWM 67 %% */
                tb_set_lock_state(false);    /* Notificar a ThingsBoard */
                break;
            default:
                ESP_LOGW(TAG, "Comando desconocido: %d", cmd);
                break;
            }
        }
    }
}
