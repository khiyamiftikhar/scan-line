/*
 * pulse_decoder.c
 *
 * Uses the ESP-IDF MCPWM Capture peripheral to measure PWM pulse widths and
 * identify which of N registered signals was received.
 *
 * Design notes
 * ────────────
 * • Up to 6 capture channels across 2 MCPWM groups (3 channels per group).
 * • A single FreeRTOS queue + task is shared by all instances.
 * • The ISR does only a tick-domain range check and enqueues raw tick values.
 * • The task converts ticks → µs and resolves the pulse ID.
 * • The interface is accessed through a pointer to the embedded member so that
 *   CONTAINER_OF always reconstructs the correct object address.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/mcpwm_cap.h"
#include "esp_err.h"
#include "esp_log.h"

#include "pulse_decoder.h"

/* ── Constants ───────────────────────────────────────────────────────────── */
static const char *TAG = "pulse-decoder";

#define MAX_CHANNELS            6
#define MAX_CHANNELS_PER_UNIT   3   /* ESP32 MCPWM: 3 capture channels / group */
#define TOTAL_CAPTURE_GROUPS    2
#define QUEUE_LENGTH            100

/* ── CONTAINER_OF ────────────────────────────────────────────────────────── */
#define CONTAINER_OF(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* ── Internal types ──────────────────────────────────────────────────────── */

typedef struct {
    mcpwm_cap_timer_handle_t      cap_timer;
    mcpwm_capture_timer_config_t  cap_conf;
} cap_timer_t;

/*
 * Class-level data — one instance shared by all pulse_decoder objects.
 * Holds the queue, the processing task, and the two possible timers.
 */
typedef struct {
    QueueHandle_t              queue;
    TaskHandle_t               capture_task;
    cap_timer_t                timer[TOTAL_CAPTURE_GROUPS];
    uint8_t                    object_count;
    pulseDecoderEventCallback  cb;
    void                      *context;
} pwm_capture_class_data_t;

static pwm_capture_class_data_t g_class = {0};

/*
 * Per-instance object.
 * pulse_widths[] is a Flexible Array Member sized to total_signals at malloc.
 */
typedef struct {
    uint8_t                    gpio_num;
    mcpwm_cap_timer_handle_t   cap_timer;      /* shared timer handle for the group  */
    mcpwm_cap_channel_handle_t cap_chan;
    pulse_decoder_interface_t  interface;       /* embedded vtable — always use &this */
    uint32_t                   time_stamp;      /* rising-edge tick captured in ISR   */
    uint8_t                    total_signals;
    uint32_t                   min_width_ticks; /* ISR filter: precomputed tick floor */
    uint32_t                   max_width_ticks; /* ISR filter: precomputed tick ceil  */
    uint32_t                   min_width_us;    /* task filter: µs floor              */
    uint32_t                   tolerance;       /* ± µs tolerance for ID matching     */
    uint32_t                   pulse_widths[];  /* sorted expected widths in µs (FAM) */
} pulse_decoder_t;

/* Item placed on the queue by the ISR */
typedef struct {
    const pulse_decoder_t *cap_obj;
    uint32_t               pulse_width_ticks;
} capture_event_data_t;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static int compare_uint32(const void *a, const void *b)
{
    uint32_t ua = *(const uint32_t *)a;
    uint32_t ub = *(const uint32_t *)b;
    return (ua > ub) - (ua < ub);
}

static void sort_uint32_array(const uint32_t *input,
                               uint32_t       *output,
                               size_t          n)
{
    memcpy(output, input, n * sizeof(uint32_t));
    qsort(output, n, sizeof(uint32_t), compare_uint32);
}

/* Resolve a received pulse width (µs) to an index in pulse_widths[]. */
static int match_pulse_id(const pulse_decoder_t *obj, uint32_t received_us)
{
    for (uint8_t i = 0; i < obj->total_signals; i++) {
        /* Unsigned subtraction — no abs() on signed types needed */
        uint32_t diff = (received_us >= obj->pulse_widths[i])
                        ? received_us - obj->pulse_widths[i]
                        : obj->pulse_widths[i] - received_us;

        if (diff <= obj->tolerance)
            return (int)i;
    }
    return ERR_CAPTURE_UNREGISTERD_PULSE_WIDTH;
}

/* ── FreeRTOS task ───────────────────────────────────────────────────────── */

static void task_processCaptureQueue(void *args)
{
    pwm_capture_class_data_t *cd = (pwm_capture_class_data_t *)args;
    capture_event_data_t      evt;

    while (1) {
        if (xQueueReceive(cd->queue, &evt, portMAX_DELAY) != pdTRUE)
            continue;

        const pulse_decoder_t *obj = evt.cap_obj;

        /* Timer resolution is 1 MHz so pulse_width_ticks is already in µs */
        uint32_t pulse_us = evt.pulse_width_ticks;

        if (pulse_us < obj->min_width_us)
            continue;

        int id = match_pulse_id(obj, pulse_us);
        if (id >= 0 && cd->cb) {
            pulse_decoder_event_data_t event = {
                .line_number   = obj->gpio_num,
                .source_number = (uint8_t)id,
            };
            cd->cb(&event, cd->context);
        }
    }
}

/* ── ISR callback ────────────────────────────────────────────────────────── */

static bool captureCallback(mcpwm_cap_channel_handle_t          cap_chan,
                             const mcpwm_capture_event_data_t   *edata,
                             void                               *user_data)
{
    (void)cap_chan;
    pulse_decoder_t *self          = (pulse_decoder_t *)user_data;
    BaseType_t       higher_prio   = pdFALSE;

    if (edata->cap_edge == MCPWM_CAP_EDGE_POS) {
        /* Record rising edge — nothing to enqueue yet */
        self->time_stamp = edata->cap_value;
    } else {
        uint32_t width = edata->cap_value - self->time_stamp;

        /* Quick tick-domain filter before hitting the queue */
        if (width < self->min_width_ticks || width > self->max_width_ticks)
            return false;

        capture_event_data_t evt = {
            .cap_obj           = self,
            .pulse_width_ticks = width,
        };
        xQueueSendFromISR(g_class.queue, &evt, &higher_prio);
    }

    return higher_prio == pdTRUE;
}

/* ── Interface implementations ───────────────────────────────────────────── */

static int startMonitoring(struct pulse_decoder_interface *self)
{
    pulse_decoder_t *obj = CONTAINER_OF(self, pulse_decoder_t, interface);
    esp_err_t        ret;

    ESP_LOGI(TAG, "Starting capture on GPIO %d", obj->gpio_num);

    ret = mcpwm_capture_channel_enable(obj->cap_chan);
    if (ret != ESP_OK) return ret;

    ret = mcpwm_capture_timer_enable(obj->cap_timer);
    if (ret != ESP_OK) return ret;

    return mcpwm_capture_timer_start(obj->cap_timer);
}

static int stopMonitoring(struct pulse_decoder_interface *self)
{
    pulse_decoder_t *obj = CONTAINER_OF(self, pulse_decoder_t, interface);
    esp_err_t        ret;

    ESP_LOGI(TAG, "Stopping capture on GPIO %d", obj->gpio_num);

    ret = mcpwm_capture_timer_stop(obj->cap_timer);
    if (ret != ESP_OK) return ret;

    return mcpwm_capture_channel_disable(obj->cap_chan);
}

/*
 * destroyDecoder
 *
 * Releases the capture channel and frees the object.
 * When the last object is destroyed the shared queue, task, and timers are
 * also torn down so the module returns to a clean initial state.
 *
 * NOTE: Timers are shared across up to three instances in the same MCPWM
 * group.  This implementation defers timer teardown until object_count hits
 * zero, which is safe as long as callers destroy all instances before creating
 * new ones.  Do not call destroy() from an ISR context.
 */
static int destroyDecoder(struct pulse_decoder_interface *self)
{
    pulse_decoder_t *obj = CONTAINER_OF(self, pulse_decoder_t, interface);
    esp_err_t        ret = ESP_OK;

    /* Disable then delete the individual capture channel */
    mcpwm_capture_channel_disable(obj->cap_chan);
    ret = mcpwm_del_capture_channel(obj->cap_chan);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "del_capture_channel failed: %s", esp_err_to_name(ret));

    /* Decrement before shared-resource teardown */
    if (g_class.object_count > 0)
        g_class.object_count--;

    /* Tear down shared resources when the last instance is gone */
    if (g_class.object_count == 0) {
        if (g_class.capture_task) {
            vTaskDelete(g_class.capture_task);
            g_class.capture_task = NULL;
        }
        if (g_class.queue) {
            vQueueDelete(g_class.queue);
            g_class.queue = NULL;
        }
        for (int g = 0; g < TOTAL_CAPTURE_GROUPS; g++) {
            if (g_class.timer[g].cap_timer) {
                mcpwm_capture_timer_stop(g_class.timer[g].cap_timer);
                mcpwm_capture_timer_disable(g_class.timer[g].cap_timer);
                mcpwm_del_capture_timer(g_class.timer[g].cap_timer);
                g_class.timer[g].cap_timer = NULL;
            }
        }
        ESP_LOGI(TAG, "All capture resources released");
    }

    free(obj);
    return (int)ret;
}

/* ── Class-level initialiser (called once per pulseDecoderCreate) ─────────── */

static esp_err_t pulseDecoderClassDataInit(void)
{
    if (g_class.object_count >= MAX_CHANNELS) {
        ESP_LOGE(TAG, "Maximum capture channels (%d) already in use", MAX_CHANNELS);
        return ERR_CAPTURE_CAP_UNIT_EXCEED;
    }

    /* Create queue + task once, on first call */
    if (g_class.queue == NULL) {
        g_class.queue = xQueueCreate(QUEUE_LENGTH, sizeof(capture_event_data_t));
        if (!g_class.queue) {
            ESP_LOGE(TAG, "Failed to create capture queue");
            return ERR_CAPTURE_MEM_ALLOC;
        }

        if (xTaskCreate(task_processCaptureQueue,
                        "captureTask",
                        4096,
                        &g_class,
                        5,
                        &g_class.capture_task) != pdPASS) {
            vQueueDelete(g_class.queue);
            g_class.queue = NULL;
            ESP_LOGE(TAG, "Failed to create capture task");
            return ERR_CAPTURE_MEM_ALLOC;
        }
    }

    /*
     * A new MCPWM timer is needed each time we step into a new group, i.e.
     * at object_count == 0 and object_count == 3.
     */
    if (g_class.object_count % MAX_CHANNELS_PER_UNIT == 0) {
        uint8_t group = g_class.object_count / MAX_CHANNELS_PER_UNIT;

        mcpwm_capture_timer_config_t conf = {
            .clk_src      = MCPWM_CAPTURE_CLK_SRC_DEFAULT,
            .group_id     = (int)group,
            .resolution_hz = 1000000,   /* 1 tick = 1 µs — cap_value is directly in µs */
        };

        esp_err_t ret = mcpwm_new_capture_timer(&conf,
                                                 &g_class.timer[group].cap_timer);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create capture timer for group %d: %s",
                     group, esp_err_to_name(ret));
            return ret;
        }
        g_class.timer[group].cap_conf = conf;
        ESP_LOGI(TAG, "Created capture timer for group %d", group);
    }

    g_class.object_count++;
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t pulseDecoderCreate(pulse_decoder_config_t    *config,
                              pulse_decoder_interface_t **out_if)
{
    if (!config || !out_if || !config->pulse_widths_us || config->total_signals == 0)
        return ESP_ERR_INVALID_ARG;

    /* Reserve a slot in the class data */
    esp_err_t ret = pulseDecoderClassDataInit();
    if (ret != ESP_OK)
        return ret;

    /* Register callback / context (shared; last caller wins) */
    if (config->cb)
        g_class.cb = config->cb;
    if (config->context)
        g_class.context = config->context;

    /* Allocate object + FAM for pulse widths */
    pulse_decoder_t *obj =
        calloc(1, sizeof(pulse_decoder_t) +
                  config->total_signals * sizeof(uint32_t));
    if (!obj) {
        g_class.object_count--;
        return ESP_ERR_NO_MEM;
    }

    obj->gpio_num      = config->gpio_num;
    obj->total_signals = config->total_signals;
    obj->tolerance     = config->tolerance_us;

    /* Sort widths ascending so index 0 = narrowest pulse */
    sort_uint32_array(config->pulse_widths_us,
                      obj->pulse_widths,
                      config->total_signals);

    /*
     * Expand the filter window by tolerance on both sides so that a valid
     * pulse sitting ±tolerance away from the narrowest/widest registered
     * width is not discarded before match_pulse_id() can evaluate it.
     * Guard the lower bound against underflow.
     */
    uint32_t min_us = obj->pulse_widths[0];
    uint32_t max_us = obj->pulse_widths[config->total_signals - 1];

    obj->min_width_us = (min_us > config->tolerance_us)
                        ? min_us - config->tolerance_us
                        : 0;
    uint32_t filter_max_us = max_us + config->tolerance_us;

    /*
     * Because the timer is configured at 1 MHz (1 tick = 1 µs), the ISR's
     * cap_value is already in µs.  Tick thresholds are identical to the µs
     * thresholds — no conversion needed.
     */
    obj->min_width_ticks = obj->min_width_us;
    obj->max_width_ticks = filter_max_us;

    /* Select the timer for this instance's MCPWM group */
    uint8_t index  = g_class.object_count - 1;  /* already incremented */
    uint8_t group  = index / MAX_CHANNELS_PER_UNIT;
    obj->cap_timer = g_class.timer[group].cap_timer;

    /* Create capture channel */
    mcpwm_capture_channel_config_t ch_conf = {
        .gpio_num        = obj->gpio_num,
        .prescale        = 1,
        .flags.neg_edge  = true,
        .flags.pos_edge  = true,
    };

    ret = mcpwm_new_capture_channel(obj->cap_timer, &ch_conf, &obj->cap_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create capture channel: %s", esp_err_to_name(ret));
        goto err_free;
    }

    /* Register ISR callback */
    mcpwm_capture_event_callbacks_t cbs = { .on_cap = captureCallback };
    ret = mcpwm_capture_channel_register_event_callbacks(obj->cap_chan, &cbs, obj);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register callbacks: %s", esp_err_to_name(ret));
        mcpwm_del_capture_channel(obj->cap_chan);
        goto err_free;
    }

    /* Wire up the virtual interface */
    obj->interface.startMonitoring = startMonitoring;
    obj->interface.stopMonitoring  = stopMonitoring;
    obj->interface.destroy         = destroyDecoder;

    /*
     * Return a POINTER to the embedded interface, not a copy.
     * This is critical: CONTAINER_OF inside the vtable methods uses the
     * address of obj->interface to locate obj.  A copied struct would have a
     * different address and CONTAINER_OF would compute garbage.
     */
    *out_if = &obj->interface;

    ESP_LOGI(TAG, "Created decoder on GPIO %d (group %d, index %d)",
             obj->gpio_num, group, index);
    return ESP_OK;

err_free:
    free(obj);
    g_class.object_count--;
    return ret;
}