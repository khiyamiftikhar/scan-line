#ifndef SIGNAL_GENERATOR_H
#define SIGNAL_GENERATOR_H

/*
 * signal_generator.h
 *
 * Utility for generating test PWM pulses on GPIO output pins.
 *
 * THIS FILE IS NOT PART OF THE pulse_decoder LIBRARY.
 * It exists only to drive signals in this example so the decoder has
 * something to measure.  In a real application the signals come from
 * external hardware (keypads, sensors, IR receivers, etc).
 */

#include <stdint.h>

/*
 * Start a FreeRTOS task that cycles through pulse_widths_us[] repeatedly
 * on the given gpio_num, with interval_ms between each pulse.
 *
 * The task runs indefinitely until the application exits.
 */
void signalGeneratorStart(uint8_t         gpio_num,
                           const uint32_t *pulse_widths_us,
                           uint8_t         total_signals,
                           uint32_t        interval_ms);

#endif /* SIGNAL_GENERATOR_H */