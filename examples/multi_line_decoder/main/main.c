/*
 * example_main.c
 *
 * Demonstrates pulse_decoder with 4 decoder objects, each watching its own
 * GPIO line for 3 different PWM pulse widths.
 *
 * KEY DESIGN PRINCIPLE — decouple callback from processing
 * ────────────────────────────────────────────────────────
 * The pulse_decoder library calls your callback from its own internal task.
 * That internal task is shared by ALL decoder instances.  If you do any
 * real work inside the callback — printing, logic, state machines — you
 * are blocking pulse processing for every other decoder on every other line.
 *
 * The correct pattern is:
 *
 *   callback()                     user_processing_task()
 *   ──────────────────             ──────────────────────────────
 *   post event to YOUR queue  →    receive from queue
 *                                  do whatever work you need here
 *
 * The callback must be as short as possible — ideally just one queue post.
 * All actual processing belongs in your own task, completely separate from
 * the library internals.
 *
 * Wiring for this example (physical jumper wires required)
 * ────────────────────────────────────────────────────────
 *   TX_GPIO_LINE_0 (GPIO 4)  →  RX_GPIO_LINE_0 (GPIO 5)
 *   TX_GPIO_LINE_1 (GPIO 6)  →  RX_GPIO_LINE_1 (GPIO 7)
 *   TX_GPIO_LINE_2 (GPIO 8)  →  RX_GPIO_LINE_2 (GPIO 9)
 *   TX_GPIO_LINE_3 (GPIO 10) →  RX_GPIO_LINE_3 (GPIO 11)
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"

#include "pulse_decoder.h"
#include "signal_generator.h"   /* demo only — not part of the library */

static const char *TAG = "example";

/* ── Pin assignments ─────────────────────────────────────────────────────── */

#define NUM_LINES   4

static const uint8_t TX_GPIOS[NUM_LINES] = { 25, 26, 27, 32 };
static const uint8_t RX_GPIOS[NUM_LINES] = { 16, 17, 18, 19 };
/* ── Signal definitions (same for every line in this example) ────────────── */

#define NUM_SIGNALS     3
#define TOLERANCE_US    100

static const uint32_t SIGNAL_WIDTHS_US[NUM_SIGNALS] = { 500, 1000, 1500 };

/* ── User event queue ────────────────────────────────────────────────────── */
/*
 * This queue is owned by the APPLICATION, not the library.
 * The callback posts here and immediately returns — keeping the library's
 * internal task free to process the next pulse.
 */
#define USER_QUEUE_LENGTH   20

static QueueHandle_t s_event_queue;

/* ── Callback — runs in pulse_decoder's internal task ───────────────────── */
/*
 * RULE: do the absolute minimum here.
 * One xQueueSendFromISR-safe queue post, nothing else.
 * No logging. No logic. No blocking calls.
 */
static void on_pulse_event(pulse_decoder_event_data_t *event, void *context)
{
    /* context is unused here — all events share one queue */
    (void)context;

    pulse_decoder_event_data_t evt_copy = *event;

    /* Non-blocking send — if the queue is full the event is dropped */
    xQueueSend(s_event_queue, &evt_copy, 0);
}

/* ── User processing task ────────────────────────────────────────────────── */
/*
 * This task is owned by the APPLICATION.
 * It does all the real work — printing, state updates, driving outputs,
 * whatever the application needs.  It runs completely independently of
 * the pulse_decoder library internals.
 */
static void user_processing_task(void *args)
{
    (void)args;
    pulse_decoder_event_data_t event;

    while (1) {
        /* Block until an event arrives — no busy waiting */
        if (xQueueReceive(s_event_queue, &event, portMAX_DELAY) != pdTRUE)
            continue;

        /*
         * Do all application processing here, freely.
         * The pulse_decoder library is completely unaffected by how long
         * this takes because we are in our own task, not the library's.
         */
        ESP_LOGI(TAG, "Line %d → Signal %d (%lu µs)",
                 event.line_number,
                 event.source_number,
                 (unsigned long)SIGNAL_WIDTHS_US[event.source_number]);
    }
}

/* ── Application entry point ─────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "pulse_decoder example — %d lines, %d signals each",
             NUM_LINES, NUM_SIGNALS);

    /* Create the user-owned event queue */
    s_event_queue = xQueueCreate(USER_QUEUE_LENGTH,
                                  sizeof(pulse_decoder_event_data_t));
    configASSERT(s_event_queue);

    /* Create the user processing task */
    xTaskCreate(user_processing_task,
                "user_proc",
                4096,
                NULL,
                5,
                NULL);

    /* Create one decoder per line */
    pulse_decoder_interface_t *ifaces[NUM_LINES] = {0};

    for (int i = 0; i < NUM_LINES; i++) {

        /* pulse_widths_us must remain valid for the lifetime of the decoder */
        pulse_decoder_config_t cfg = {
            .gpio_num        = RX_GPIOS[i],
            .pulse_widths_us = (uint32_t *)SIGNAL_WIDTHS_US,
            .total_signals   = NUM_SIGNALS,
            .tolerance_us    = TOLERANCE_US,
            .cb              = on_pulse_event,
            .context         = NULL,
        };

        esp_err_t err = pulseDecoderCreate(&cfg, &ifaces[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create decoder for line %d: %s",
                     i, esp_err_to_name(err));
            return;
        }

        ifaces[i]->startMonitoring(ifaces[i]);

        ESP_LOGI(TAG, "Decoder %d started on GPIO %d", i, RX_GPIOS[i]);
    }

    /*
     * Start signal generators on the TX pins.
     *
     * NOTE: signalGeneratorStart() and everything in signal_generator.c
     * is demo scaffolding only.  In a real application these pulses come
     * from external hardware and this code does not exist at all.
     */
    for (int i = 0; i < NUM_LINES; i++) {
        signalGeneratorStart(TX_GPIOS[i],
                              SIGNAL_WIDTHS_US,
                              NUM_SIGNALS,
                              500);   /* one pulse every 500 ms */
    }

    ESP_LOGI(TAG, "All decoders running. Waiting for events...");
}