# pulse_decoder

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-blue)
![Espressif Component Registry](https://img.shields.io/badge/Espressif-Component%20Registry-orange)
![License](https://img.shields.io/badge/license-MIT-green)

ESP-IDF component for **identifying PWM pulse-width-encoded signals** on multiple GPIO lines simultaneously, using the MCPWM capture peripheral.

Instead of polling or writing ISR code yourself, register the pulse widths your hardware sends and receive a callback telling you which signal arrived on which line.

---

# Features

- Monitor up to **6 GPIO lines** simultaneously
- Register **any number of signals** per line, each with a distinct pulse width
- Configurable **per-decoder tolerance** for real-world jitter
- **Two-stage filtering**: ISR tick filter drops noise before the queue; task-level matching identifies the signal
- **Callback-based API** with `startMonitoring`, `stopMonitoring`, `destroy`
- Cross-chip support — timer resolution detected automatically at runtime

---

# Installation

## Using ESP-IDF Component Manager (Recommended)

```bash
idf.py add-dependency "embedblocks/pulse_decoder^1.0.0"
```

Or in your project's `idf_component.yml`:

```yaml
dependencies:
  embedblocks/pulse_decoder: "^1.0.0"
```

---

# How it works

Each GPIO line is assigned an MCPWM capture channel. On every rising edge the timer timestamp is recorded. On the falling edge the width is computed and compared against your registered pulse widths within the configured tolerance.

```
GPIO line  ______|‾‾‾‾‾‾‾‾‾‾|___________

                 ↑          ↓
           record ts    width = ts_now - ts
                              │
                        ISR tick filter
                        (drop obvious noise)
                              │
                        task: ticks → µs
                        match against widths[]
                              │
                        callback(line, signal_id)
```

Three signals on one line with tolerance ±100 µs:

```
Registered:  500 µs    1000 µs    1500 µs
             [400─600] [900─1100] [1400─1600]

Received 480 µs  → signal 0 ✓
Received 750 µs  → no match (rejected)
Received 1530 µs → signal 2 ✓
```

---

# Callback rule

The library processes all lines from a **single internal FreeRTOS task**. A slow callback blocks every other line. Always post to your own queue and return immediately:

```c
/* CORRECT */
static void on_pulse(pulse_decoder_event_data_t *event, void *context)
{
    xQueueSend(s_event_queue, event, 0);
}

/* WRONG — blocks the library's internal task */
static void on_pulse(pulse_decoder_event_data_t *event, void *context)
{
    ESP_LOGI(TAG, "got pulse");           /* slow */
    vTaskDelay(pdMS_TO_TICKS(10));        /* never do this */
}
```

Do all real processing in your own task that reads from your own queue.

---

# Chip support

| Chip      | Status              | Timer resolution        |
|-----------|---------------------|-------------------------|
| ESP32     | ✅ Tested           | 80 MHz (fixed, APB)     |
| ESP32-S3  | ⚠️ Expected to work | 1 MHz (configurable)    |
| ESP32-C3  | ⚠️ Expected to work | 1 MHz (configurable)    |
| ESP32-H2  | ⚠️ Expected to work | 1 MHz (configurable)    |

The actual timer resolution is queried at runtime using `mcpwm_capture_timer_get_resolution()` so tick↔µs conversions are always correct regardless of chip. No compile-time flags needed.

---

# API

## Create

```c
esp_err_t pulseDecoderCreate(pulse_decoder_config_t    *config,
                              pulse_decoder_interface_t **out_if);
```

**`pulse_decoder_config_t`:**

| Field | Type | Description |
|---|---|---|
| `gpio_num` | `uint8_t` | GPIO to monitor |
| `pulse_widths_us` | `uint32_t *` | Expected signal widths in µs |
| `total_signals` | `uint8_t` | Length of `pulse_widths_us` |
| `tolerance_us` | `uint32_t` | ± matching window in µs |
| `cb` | `pulseDecoderEventCallback` | Called when a signal is identified |
| `context` | `void *` | Passed to `cb`, may be NULL |

## Control

```c
decoder->startMonitoring(decoder);   /* start receiving events  */
decoder->stopMonitoring(decoder);    /* stop receiving events   */
decoder->destroy(decoder);           /* free all resources      */
```

## Event data

```c
typedef struct {
    uint8_t line_number;    /* gpio_num the pulse arrived on         */
    uint8_t source_number;  /* index into pulse_widths_us[] matched  */
} pulse_decoder_event_data_t;
```

---

# Usage example

```c
#include "pulse_decoder.h"

static uint32_t      widths_us[] = { 500, 1000, 1500 };
static QueueHandle_t s_queue;

static void on_pulse(pulse_decoder_event_data_t *event, void *context)
{
    xQueueSend(s_queue, event, 0);   /* post and return — never block here */
}

void app_main(void)
{
    s_queue = xQueueCreate(20, sizeof(pulse_decoder_event_data_t));

    pulse_decoder_config_t cfg = {
        .gpio_num        = 5,
        .pulse_widths_us = widths_us,
        .total_signals   = 3,
        .tolerance_us    = 100,
        .cb              = on_pulse,
        .context         = NULL,
    };

    pulse_decoder_interface_t *decoder;
    ESP_ERROR_CHECK(pulseDecoderCreate(&cfg, &decoder));
    decoder->startMonitoring(decoder);
}
```

---

# Choosing tolerance

Set tolerance to less than half the gap between your two closest signal widths:

```
Widths: 500, 1000, 1500  →  gap = 500 µs  →  tolerance < 250 µs
Widths: 500,  600,  700  →  gap = 100 µs  →  tolerance < 50 µs
```

---

# Limitations

- Maximum **6 decoder instances** per application (hardware: 2 MCPWM groups × 3 capture channels)
- The callback and context are **shared across all instances** — the last-registered value applies to all decoders
- Only the **original ESP32** has been fully validated; other chips are expected to work based on driver compatibility

---

# Examples

| Example | Description |
|---------|-------------|
| `basic` | Single decoder on one GPIO line. Minimal setup showing create, start, and event handling. |
| `multi_line` | Four decoders running simultaneously, each on its own GPIO. Demonstrates the queue-based decoupling pattern. |

### Running an example

```bash
cd examples/basic
idf.py build flash monitor
```

---

# Testing

Tests use Unity with a physical jumper wire between GPIO 4 (TX) and GPIO 5 (RX).

```bash
cd test
idf.py build flash monitor
```

---

# License

MIT