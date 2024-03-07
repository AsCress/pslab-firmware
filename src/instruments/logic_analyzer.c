/**
 * @file logic_analyzer.c
 * @author Alexander Bessman (alexander.bessman@gmail.com)
 * @brief High-level driver for the PSLab's Logic Analyzer instrument
 * @details
 * # Implementation
 *
 * The logic analyzer uses the following resources:
 *
 * ## Pins LA1-4
 *
 * When the logic level on an active pin changes, a timestamp is stored in the
 * sample buffer. Three types of logic level changes (edges) can be captured:
 * ANY, FALLING, or RISING.
 *
 * If the configured edge type is ANY, a timestamp is stored every time the
 * logic level changes from low to high, or from high to low.
 *
 * If the configured edge type is RISING, a timestamp is stored every time the
 * logic level changes from low to high, but not from high to low. Vice versa
 * for edge type FALLING.
 *
 * Up to 10k timestamps can be captured, across all four channels.
 *
 * ## Input Capture (IC) channels IC1-4
 *
 * Each ICx channel is associated with the corresponding LAx pin. When the
 * configured edge type is detected on LAn, the current value of ICxTMR is
 * copied to ICxBUF.
 *
 * IC interrupt is used to trigger delayed capture, if edge type is FALLING or
 * RISING. If edge type is ANY, delayed capture is instead triggered by CN.
 *
 * ## Input Change Notification (CN)
 *
 * One pin may be designated as the trigger pin, in which case capture begins
 * when the configured edge type is detected on that pin. If no pin is selected
 * as trigger, capture begins immediatedely.
 *
 * If the edge type is ANY, CN interrupt is used to start capture. If the edge
 * type is FALLING or RISING, IC interrupt is used instead.
 *
 * ## Timer TMR5
 *
 * When the trigger condition is met, TMR5 is started. TMR5 is used as trigger
 * source to start the enabled IC channels' ICxTMR, as well as clock source to
 * clock the same.
 *
 * ## Direct Memory Access (DMA) channels DMA0-3
 *
 * ICx drives DMA(x-1). Every time a new value is copied to ICxBUF, DMA(x-1)
 * copies it to the sample buffer.
 *
 * When the requested number of timestamps have been captured on LAx, DMA(x-1)
 * interrupts and resets itself and ICx. If ICx is the last active channel,
 * TMR5 is reset.
 *
 * ## Sample Buffer
 *
 * Captured timestamps are stored in the sample buffer.
 */

#include <stdbool.h>
#include <stdint.h>

#include "../bus/uart/uart.h"
#include "buffer.h"
#include "commands.h"
#include "registers_ng/cn.h"
#include "registers_ng/dma.h"
#include "registers_ng/ic.h"
#include "registers_ng/pins.h"
#include "registers_ng/tmr.h"
#include "types.h"

/*********************/
/* Static Prototypes */
/*********************/

/**
 * @brief Start TMR and DMA
 * @details
 * May be called directly to trigger manually, or by an interrupt callback.
 */
static void trigger(void);

/**
 * @brief Trigger from Input Capture interrupt
 * @details
 * Register with `IC_interrupt_enable`. Disables IC interrupt and then calls
 * `trigger`. Used when triggering on either FALLING or RISING edges, not both.
 *
 * @param channel
 */
static void ic_callback(Channel channel);

/**
 * @brief Trigger from Input Change Notification interrupt
 * @details
 * Register with `CN_interrupt_enable`. Disables CN interrupt and then calls
 * `trigger`. Used when triggering on ANY edge.
 *
 * @param channel
 * Unused.
 */
static void cn_callback(Channel channel);

/**
 * @brief Stop IC, DMA, and TMR when all events have been captured
 *
 * @details Called by DMA interrupts to cleanup after capture is complete.
 *
 * @param channel
 */
static void cleanup_callback(Channel channel);

/**
 * @brief Capture logic level changes on on LA1-4
 *
 * @param num_channels
 * @param events
 * @param edge
 * @param trigger_pin
 */
static void capture(
    uint8_t num_channels,
    uint16_t events,
    Edge edge,
    Channel trigger_pin
);

/**
 * @brief Choose trigger method based on pin and edge type
 * @details
 * Manually triggers immediately if `trigger_pin` is NONE.
 *
 * Uses IC interrupt if `edge` is RISING or FALLING, CN interrupt if edge is
 * ANY.
 *
 * @param edge
 * @param trigger_pin
 */
static void configure_trigger(Edge edge, Channel trigger_pin);

/**
 * @brief Convert TMR to corresponding IC clock source
 *
 * @param timer
 * @return IC_Timer
 */
static IC_Timer timer2ictsel(TMR_Timer timer);

/***********/
/* Globals */
/***********/

static uint8_t g_num_channels = 0;

static uint8_t g_initial_states = 0;

static TMR_Timer const g_TIMER = TMR_TIMER_5;

/********************/
/* Static Functions */
/********************/

static void trigger(void)
{
    // Set timer period to a small value to assert sync when timer starts.
    TMR_set_period(g_TIMER, 1);
    TMR_start(g_TIMER);
    g_initial_states = PINS_get_la_states();

    /* When DMA starts, every time a value is copied to ICxBUF it will be
     * further copied to sample buffer.
     * DMA channels cannot be started simultaneously. It is possible we might
     * miss edges between timer start and DMA start. The alternative is to
     * start DMA first, which risks copying spurious zeros to sample buffer.
     */
    /* Unroll loop; saving even a single clock cycle between DMA channel
     * starts is meaningful. */
    switch (g_num_channels) {
    case 4: // NOLINT(readability-magic-numbers)
        DMA_start(CHANNEL_4);
    case 3: // NOLINT(readability-magic-numbers)
        DMA_start(CHANNEL_3);
    case 2:
        DMA_start(CHANNEL_2);
    case 1:
        DMA_start(CHANNEL_1);
    default:
        break;
    }

    // Timer sync output is only needed once; disable it after trigger is done.
    TMR_set_period(g_TIMER, 0);
}

static void ic_callback(Channel const channel)
{
    IC_interrupt_disable(channel);
    trigger();
}

static void cn_callback(__attribute__((unused)) Channel const channel)
{
    CN_reset();
    trigger();
}

static void cleanup_callback(Channel const channel)
{
    DMA_reset(channel);
    IC_reset(channel);
    --g_num_channels;

    // Reset the clock if this was the last active channel.
    if (!g_num_channels) {
        TMR_reset(g_TIMER);
    }
}

static void capture(
    uint8_t const num_channels,
    uint16_t const events,
    Edge const edge,
    Channel const trigger_pin
)
{
    g_num_channels = num_channels;

    for (uint8_t i = 0; i < num_channels; ++i) {
        size_t const address =
            (size_t)(BUFFER + i * BUFFER_SIZE / num_channels);
        DMA_setup(i, events, address, DMA_SOURCE_IC);
        /* DMA interrupt is enabled here, but the DMA transfer itself is
         * started in trigger callback. */
        DMA_interrupt_enable(i, cleanup_callback);
        /* IC is started here. IC will now begin copying the value of ICxTMR to
         * ICxBUF whenever an event occurs. Until the trigger event starts the
         * clock source, ICxTMR will be held at zero. This is not a problem,
         * because although zeros will be copied to ICxBUF, they won't be
         * copied to the sample buffer until DMA is started by the trigger
         * callback. */
        IC_start(i, edge, timer2ictsel(g_TIMER));
    }

    configure_trigger(edge, trigger_pin);
}

static void configure_trigger(Edge const edge, Channel const trigger_pin)
{
    if (trigger_pin == CHANNEL_NONE) {
        // Start immediately.
        trigger();
        return;
    }

    if (edge == EDGE_ANY) {
        /* Input capture cannot interrupt on both falling and rising edge, only
         * one or the other. Must use Change Notification instead. */
        CN_interrupt_enable(trigger_pin, cn_callback);
        return;
    }

    IC_interrupt_enable(trigger_pin, ic_callback);
}

static IC_Timer timer2ictsel(TMR_Timer const timer)
{
    switch (timer) {
    case TMR_TIMER_1:
        return IC_TIMER_TMR1;
    default:
        return IC_TIMER_PERIPHERAL;
    }
}

/********************/
/* Public Functions */
/********************/

response_t LA_capture(void)
{
    uint8_t const num_channels = UART1_Read();
    uint16_t const events = UART1_ReadInt();
    Edge const edge = UART1_Read();
    Channel const trigger = UART1_Read();

    if (num_channels == 0) {
        return ARGUMENT_ERROR;
    }

    if (num_channels > CHANNEL_NUMEL) {
        return ARGUMENT_ERROR;
    }

    if (edge == EDGE_NONE) {
        return ARGUMENT_ERROR;
    }

    capture(num_channels, events, edge, trigger);
    return SUCCESS;
}

response_t LA_stop(void)
{
    CN_reset();
    TMR_reset(g_TIMER);

    for (size_t i = 0; i < CHANNEL_NUMEL; ++i) {
        IC_reset(i);
        DMA_reset(i);
    }

    return SUCCESS;
}

response_t LA_get_initial_states(void)
{
    UART1_Write(g_initial_states);
    return SUCCESS;
}
