/*
 * test_pulse_decoder.c
 *
 * Unity tests for pulse_decoder.
 *
 * WIRING REQUIRED
 * ───────────────
 * Connect a jumper wire between TX_GPIO (pin 4) and RX_GPIO (pin 5).
 *
 * TX_GPIO is toggled by the test to generate pulses.
 * RX_GPIO is the MCPWM capture input watched by the decoder.
 * The wire carries the signal electrically between the two.
 *
 * Pulse generation
 * ────────────────
 * generate_pulse(width_us):
 *   1. TX_GPIO → HIGH   (MCPWM capture records rising-edge timestamp)
 *   2. busy-wait width_us  (esp_rom_delay_us, accurate to ±1 µs)
 *   3. TX_GPIO → LOW   (MCPWM capture computes width, enqueues event)
 *
 * Synchronisation
 * ────────────────
 * The decoder runs its own FreeRTOS task that calls the user callback.
 * Each test blocks on a binary semaphore that the callback posts.
 * A 200 ms timeout catches cases where no callback arrives.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "unity.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"    /* esp_rom_delay_us */

#include "pulse_decoder.h"

/* ── Pin assignments — change to suit your board ────────────────────────── */
#define TX_GPIO     4       /* output: drives test pulses        */
#define RX_GPIO     5       /* input:  MCPWM capture             */

/* ── Registered signal widths and tolerance ─────────────────────────────── */
#define SIG_A_US    500u
#define SIG_B_US    1000u
#define SIG_C_US    1500u
#define TOLERANCE   100u

/* ── Maximum wait for a callback before the test fails ──────────────────── */
#define CALLBACK_TIMEOUT_MS  200

/* ── State shared between the callback and the test task ────────────────── */
typedef struct {
    SemaphoreHandle_t sem;          /* callback posts, test blocks on this */
    int               last_id;      /* source_number of last event         */
    uint8_t           last_line;    /* line_number   of last event         */
    uint32_t          call_count;   /* total events received               */
} test_context_t;

static test_context_t             g_ctx   = {0};
static pulse_decoder_interface_t *g_iface = NULL;

/* ── Callback ────────────────────────────────────────────────────────────── */

static void test_callback(pulse_decoder_event_data_t *event, void *context)
{
    test_context_t *ctx = (test_context_t *)context;
    
    ctx->last_id    = event->source_number;
    ctx->last_line  = event->line_number;
    ctx->call_count++;
    xSemaphoreGive(ctx->sem);
}

/* ── GPIO init ───────────────────────────────────────────────────────────── */

static void init_tx_gpio(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << TX_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(TX_GPIO, 0);
}

/* ── Pulse generator ─────────────────────────────────────────────────────── */

static void generate_pulse(uint32_t width_us)
{
    gpio_set_level(TX_GPIO, 0);
    esp_rom_delay_us(50);           /* ensure a clean LOW before rising edge  */

    gpio_set_level(TX_GPIO, 1);     /* rising edge → MCPWM stores timestamp   */
    esp_rom_delay_us(width_us);

    gpio_set_level(TX_GPIO, 0);     /* falling edge → MCPWM computes width    */
    esp_rom_delay_us(50);
}

/* ── Unity setUp / tearDown ──────────────────────────────────────────────── */

void setUp(void)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.last_id = -1;
    g_ctx.sem     = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL_MESSAGE(g_ctx.sem, "Failed to create semaphore");

    init_tx_gpio();

    uint32_t widths[] = { SIG_A_US, SIG_B_US, SIG_C_US };

    pulse_decoder_config_t cfg = {
        .gpio_num        = RX_GPIO,
        .pulse_widths_us = widths,
        .total_signals   = 3,
        .tolerance_us    = TOLERANCE,
        .cb              = test_callback,
        .context         = &g_ctx,
    };

    esp_err_t err = pulseDecoderCreate(&cfg, &g_iface);
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, err, "pulseDecoderCreate failed");
    TEST_ASSERT_NOT_NULL(g_iface);

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK,
        g_iface->startMonitoring(g_iface),
        "startMonitoring failed");

    vTaskDelay(pdMS_TO_TICKS(10));  /* let peripheral settle */
}

void tearDown(void)
{
    if (g_iface) {
        g_iface->stopMonitoring(g_iface);
        g_iface->destroy(g_iface);
        g_iface = NULL;
    }
    if (g_ctx.sem) {
        vSemaphoreDelete(g_ctx.sem);
        g_ctx.sem = NULL;
    }
    gpio_reset_pin(TX_GPIO);
    gpio_reset_pin(RX_GPIO);
}

/* ── Test cases ──────────────────────────────────────────────────────────── */

TEST_CASE("Exact pulse widths are identified correctly", "[pulse_decoder]")
{
    struct { uint32_t width_us; int expected_id; } sigs[] = {
        { SIG_A_US, 0 },
        { SIG_B_US, 1 },
        { SIG_C_US, 2 },
    };

    for (int i = 0; i < 3; i++) {
        g_ctx.last_id = -1;

        generate_pulse(sigs[i].width_us);

        bool got = xSemaphoreTake(g_ctx.sem, pdMS_TO_TICKS(CALLBACK_TIMEOUT_MS));
        TEST_ASSERT_TRUE_MESSAGE(got, "Callback timed out");
        TEST_ASSERT_EQUAL(sigs[i].expected_id, g_ctx.last_id);
        TEST_ASSERT_EQUAL(RX_GPIO,             g_ctx.last_line);
    }
}

TEST_CASE("Pulse within tolerance boundary is accepted", "[pulse_decoder]")
{
    /* SIG_B +/- (TOLERANCE - 1) must still resolve to ID 1 */
    uint32_t just_below = SIG_B_US - (TOLERANCE - 1);
    uint32_t just_above = SIG_B_US + (TOLERANCE - 1);

    generate_pulse(just_below);
    TEST_ASSERT_TRUE_MESSAGE(
        xSemaphoreTake(g_ctx.sem, pdMS_TO_TICKS(CALLBACK_TIMEOUT_MS)),
        "Pulse just below centre timed out");
    TEST_ASSERT_EQUAL(1, g_ctx.last_id);

    g_ctx.last_id = -1;

    generate_pulse(just_above);
    TEST_ASSERT_TRUE_MESSAGE(
        xSemaphoreTake(g_ctx.sem, pdMS_TO_TICKS(CALLBACK_TIMEOUT_MS)),
        "Pulse just above centre timed out");
    TEST_ASSERT_EQUAL(1, g_ctx.last_id);
}

TEST_CASE("Out-of-range pulse fires no callback", "[pulse_decoder]")
{
    /*
     * 750 us sits exactly halfway between SIG_A (500) and SIG_B (1000).
     * Both are 250 us away — well outside the 100 us tolerance.
     */
    generate_pulse((SIG_A_US + SIG_B_US) / 2);

    TEST_ASSERT_FALSE_MESSAGE(
        xSemaphoreTake(g_ctx.sem, pdMS_TO_TICKS(CALLBACK_TIMEOUT_MS)),
        "Callback fired for out-of-range pulse");
}

TEST_CASE("Pulse below filter floor is rejected", "[pulse_decoder]")
{
    /* Below SIG_A - tolerance - margin, caught by the ISR pre-filter */
    generate_pulse(SIG_A_US - TOLERANCE - 50);

    TEST_ASSERT_FALSE_MESSAGE(
        xSemaphoreTake(g_ctx.sem, pdMS_TO_TICKS(CALLBACK_TIMEOUT_MS)),
        "Callback fired for sub-floor pulse");
}

TEST_CASE("Pulse above filter ceiling is rejected", "[pulse_decoder]")
{
    /* Above SIG_C + tolerance + margin, caught by the ISR pre-filter */
    generate_pulse(SIG_C_US + TOLERANCE + 50);

    TEST_ASSERT_FALSE_MESSAGE(
        xSemaphoreTake(g_ctx.sem, pdMS_TO_TICKS(CALLBACK_TIMEOUT_MS)),
        "Callback fired for over-ceiling pulse");
}

TEST_CASE("Multiple sequential pulses all delivered", "[pulse_decoder]")
{
    uint32_t sequence[] = { SIG_A_US, SIG_C_US, SIG_B_US, SIG_A_US };
    int      expected[] = { 0,        2,        1,        0         };

    for (int i = 0; i < 4; i++) {
        g_ctx.last_id = -1;

        generate_pulse(sequence[i]);

        TEST_ASSERT_TRUE_MESSAGE(
            xSemaphoreTake(g_ctx.sem, pdMS_TO_TICKS(CALLBACK_TIMEOUT_MS)),
            "Callback timed out for sequential pulse");
        TEST_ASSERT_EQUAL(expected[i], g_ctx.last_id);
    }

    TEST_ASSERT_EQUAL(4, g_ctx.call_count);
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

