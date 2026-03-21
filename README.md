# pulse_decoder

Identify PWM pulse-width-encoded signals on multiple GPIO lines simultaneously using the ESP32 MCPWM capture peripheral.

## What this component does

Many embedded systems encode information as pulse width — a keypad that sends a short pulse for key A, a medium pulse for key B, and a long pulse for key C on the same wire. This component lets you register a set of expected pulse widths per GPIO line and receive a callback telling you **which signal arrived on which line**, without worrying about timers, ISRs, or ticks.

Typical use cases:
- **Matrix keypads** wired to a single input line per row, sending different pulse widths per column
- **IR or RF receivers** that encode commands as PWM width
- **Industrial sensors** using pulse-width encoding for status signals
- Any system where **N distinct signals** are multiplexed on a single line via pulse width

---

## Features

- Up to **6 simultaneous GPIO lines** (2 MCPWM groups × 3 capture channels)
- **Any number of signals per line** — register as many pulse widths as needed
- **Configurable tolerance** per decoder — handles real-world pulse jitter
- **Two-stage filtering**: ISR pre-filter drops obvious noise before the queue; task-level matching identifies the signal
- **Callback-based API** with a clean virtual interface (`startMonitoring`, `stopMonitoring`, `destroy`)
- **Multi-chip compatible**: ESP32, ESP32-S3, ESP32-C3, ESP32-H2 — timer resolution differences handled automatically at runtime
- Thread-safe: single shared queue and task, all instances serialised internally

---

## Hardware requirements

- Any ESP32-family chip with MCPWM peripheral
- One GPIO input per line you want to monitor

---

## Installation

### Using the ESP-IDF Component Manager

Add to your project's `idf_component.yml`:

```yaml
dependencies:
  your_namespace/pulse_decoder: "^1.0.0"
```

Then run:
```bash
idf.py update-dependencies
```

### Manual installation

Clone into your project's `components/` directory:
```bash
cd components
git clone https://github.com/your_username/pulse_decoder
```

---

## Quick start

```c
#include "pulse_decoder.h"

/* Define the pulse widths your hardware sends, in microseconds */
static const uint32_t keypad_widths_us[] = { 500, 1000, 1500, 2000 };

/* YOUR queue — events from the callback go here */
static QueueHandle_t s_event_queue;

/*
 * Callback fires inside the library's internal task.
 * Keep it minimal — just post to your own queue and return.
 * Do all real processing in your own task (see below).
 */
static void on_pulse(pulse_decoder_event_data_t *event, void *context)
{
    pulse_decoder_event_data_t copy = *event;
    xQueueSend(s_event_queue, &copy, 0);
}

void app_main(void)
{
    s_event_queue = xQueueCreate(20, sizeof(pulse_decoder_event_data_t));

    pulse_decoder_config_t cfg = {
        .gpio_num        = 5,               /* GPIO to monitor          */
        .pulse_widths_us = keypad_widths_us, /* expected signal widths   */
        .total_signals   = 4,               /* number of entries above  */
        .tolerance_us    = 100,             /* ± matching tolerance     */
        .cb              = on_pulse,
        .context         = NULL,
    };

    pulse_decoder_interface_t *decoder;
    ESP_ERROR_CHECK(pulseDecoderCreate(&cfg, &decoder));
    decoder->startMonitoring(decoder);
}
```

---

## Callback rule — the most important thing to understand

The library processes all GPIO lines from a **single internal FreeRTOS task**. Your callback is called from inside that task. If your callback does anything slow — printing, state machines, waiting — it blocks pulse processing on **every other line** for that entire duration.

```
Library internal task
  │
  ├── pulse on line 0 → your callback()  ← if this is slow...
  ├── pulse on line 1                    ← ...this waits
  ├── pulse on line 2                    ← ...and this
  └── pulse on line 3                    ← ...and this
```

**The correct pattern** is to post the event to your own queue in the callback and do all real work in your own task:

```c
/* CORRECT — callback is one line */
static void on_pulse(pulse_decoder_event_data_t *event, void *context)
{
    xQueueSend(s_event_queue, event, 0);   /* post and return immediately */
}

/* Your task handles the event independently */
static void my_processing_task(void *args)
{
    pulse_decoder_event_data_t event;
    while (1) {
        xQueueReceive(s_event_queue, &event, portMAX_DELAY);
        /* do whatever you need here — log, drive outputs, update state */
        ESP_LOGI(TAG, "Line %d: signal %d", event.line_number, event.source_number);
    }
}
```

```c
/* WRONG — blocks the library */
static void on_pulse(pulse_decoder_event_data_t *event, void *context)
{
    ESP_LOGI(TAG, "got pulse");          /* slow */
    update_display(event->source_number); /* potentially slow */
    vTaskDelay(pdMS_TO_TICKS(10));       /* never do this */
}
```

---

## API reference

### `pulseDecoderCreate`

```c
esp_err_t pulseDecoderCreate(pulse_decoder_config_t    *config,
                              pulse_decoder_interface_t **out_if);
```

Creates a decoder instance. On success, `*out_if` points to the decoder's interface.

**`pulse_decoder_config_t` fields:**

| Field | Type | Description |
|---|---|---|
| `gpio_num` | `uint8_t` | GPIO to monitor |
| `pulse_widths_us` | `uint32_t *` | Array of expected pulse widths in µs |
| `total_signals` | `uint8_t` | Length of `pulse_widths_us` |
| `tolerance_us` | `uint32_t` | ± matching window in µs |
| `cb` | `pulseDecoderEventCallback` | Called when a signal is identified |
| `context` | `void *` | Passed to `cb`, can be NULL |

---

### `pulse_decoder_interface_t`

The interface returned by `pulseDecoderCreate` exposes three methods:

```c
decoder->startMonitoring(decoder);  /* enable capture, start receiving events  */
decoder->stopMonitoring(decoder);   /* disable capture, no more events          */
decoder->destroy(decoder);          /* release all resources, free the object   */
```

`destroy` can be called without calling `stopMonitoring` first — it handles both.

---

### `pulse_decoder_event_data_t`

```c
typedef struct {
    uint8_t line_number;    /* the gpio_num the pulse arrived on          */
    uint8_t source_number;  /* index into pulse_widths_us[] that matched  */
} pulse_decoder_event_data_t;
```

---

## Multiple lines example

```c
#define NUM_LINES 4
static const uint8_t  RX_GPIOS[]       = { 5, 7, 9, 11 };
static const uint32_t widths_us[]      = { 500, 1000, 1500 };
static pulse_decoder_interface_t *decoders[NUM_LINES];

for (int i = 0; i < NUM_LINES; i++) {
    pulse_decoder_config_t cfg = {
        .gpio_num        = RX_GPIOS[i],
        .pulse_widths_us = (uint32_t *)widths_us,
        .total_signals   = 3,
        .tolerance_us    = 100,
        .cb              = on_pulse,
        .context         = NULL,
    };
    ESP_ERROR_CHECK(pulseDecoderCreate(&cfg, &decoders[i]));
    decoders[i]->startMonitoring(decoders[i]);
}
```

---

## Capacity and limits

| Parameter | Limit |
|---|---|
| GPIO lines (decoder instances) | 6 |
| Signals per line | Unlimited (heap allocated) |
| MCPWM groups used | 2 (3 lines each) |
| Internal queue depth | 100 events |

---

## Choosing tolerance

Tolerance is the ± window (in µs) within which a received pulse is considered a match for a registered width.

- Too tight → valid pulses rejected when there is natural jitter
- Too loose → adjacent signals overlap and the wrong ID is returned

A safe rule of thumb: set tolerance to less than half the gap between your closest two signal widths.

```
Widths: 500, 1000, 1500    Gap = 500 µs    → tolerance < 250 µs
Widths: 500, 600,  700     Gap = 100 µs    → tolerance < 50 µs
```

---

## Chip compatibility

| Chip | MCPWM timer resolution | Notes |
|---|---|---|
| ESP32 | 80 MHz (fixed, APB clock) | `resolution_hz` config field is ignored by hardware |
| ESP32-S3 | 1 MHz (configurable) | Full prescaler support |
| ESP32-C3 | 1 MHz (configurable) | Full prescaler support |
| ESP32-H2 | 1 MHz (configurable) | Full prescaler support |

The component detects the actual hardware resolution at runtime using `mcpwm_capture_timer_get_resolution()` and uses that value for all tick↔µs conversions. No code changes or compile-time flags are needed when switching chips.

---

## Running the tests

Tests use Unity and require a **physical jumper wire** between `TX_GPIO` (pin 4) and `RX_GPIO` (pin 5).

```bash
cd test
idf.py build flash monitor
```

Press a key when the Unity menu appears to run all tests or individual cases.

---

## License

MIT