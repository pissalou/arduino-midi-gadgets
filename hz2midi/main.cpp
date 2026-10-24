#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/sync.h"

// ---------------------------------------------------------------------------
// Pitch Detector based on Joel de Guzman's  BACF (Bipolar Autocorrelation Function) 
// Play a tone 50-1600Hz into the microphone and read the freq detected on Serial  
// ---------------------------------------------------------------------------


// Q DSP library components
#include <q/fx/signal_conditioner.hpp>
#include <q/pitch/pitch_detector.hpp>

namespace q = cycfi::q;
using namespace q::literals;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
// GPIO25, on-board LED
constexpr uint     LED_GPIO        = 25;  

// ADC input N is on GPIO 26+N, so channel 2 = GPIO28
constexpr uint     CAPTURE_CHANNEL = 2;

// Sampling: the ADC is clocked at 48 MHz and takes one sample every
// (clkdiv + 1) cycles, so 48 MHz / 48 kHz = 1000 cycles -> clkdiv = 999.
constexpr uint32_t ADC_CLOCK_HZ    = 48000000;
constexpr uint32_t SAMPLE_RATE_HZ  = 48000;

// 10000 samples @ 48 kHz = ~208 ms per buffer
constexpr int      CAPTURE_DEPTH   = 10000;

#define PRINT_SAMPLES 1   // print detected frequency (Hz) per buffer, 0 = none

// Onset detection: STA/LTA ratio in percent. 500 = short-term level must be
// 5x the long-term background level. Tune for your mic/room.
constexpr int32_t  THRESHOLD   = 500;
constexpr uint64_t LOCKOUT_US  = 1000000;
constexpr uint32_t WARMUP_BUFS = 5;      // let the background estimate settle

constexpr int32_t  ALPHA   = 50;   // short-term average weight (per sample), %
constexpr int32_t  ALPHA_L = 5;    // long-term average weight (per buffer), %

constexpr int      FIXQ      = 8;           // fixed-point fraction bits (Q8)
constexpr int32_t  MIN_LTA_Q = 1 << FIXQ;   // background >= 1 LSB of noise

// Pitch detection range in Hz. Tones outside this range are either missed
// or reported as a wrong (sub)harmonic, so set it to cover your signal.
constexpr auto PITCH_LOWEST  = q::frequency{50.0};
constexpr auto PITCH_HIGHEST = q::frequency{1600.0};
constexpr auto PITCH_HYSTERESIS = -45_dB;

// Noise gate of the signal conditioner. Q's defaults (-33 dB open,
// -55 dB close) need a fairly loud input: about 100 ADC counts of
// amplitude. These open at about 30 counts (~25 mV) instead.
constexpr auto GATE_ONSET   = -50_dB;
constexpr auto GATE_RELEASE = -60_dB;

// ---------------------------------------------------------------------------
// Double buffer + DMA state
// ---------------------------------------------------------------------------
static uint16_t capture_buf[2][CAPTURE_DEPTH];
static uint     dma_chan[2];

static volatile int      ready_idx        = -1;  // buffer waiting to be processed
static volatile uint32_t buffers_captured = 0;
static volatile uint32_t overruns         = 0;

// Onset detector state (only touched from the main loop)
static int32_t  sta = 0;
static int32_t  lta = 0;
static bool     led_state = false;
static uint64_t last_trigger_us = 0;

// Q processors. Created in main() (they allocate on construction).
struct pitch_chain
{
    q::signal_conditioner sc;
    q::pitch_detector     pd;
};
static pitch_chain* pitch = nullptr;
static float        dc_float = 2048.0f;   // slowly tracked DC level for Q

// ---------------------------------------------------------------------------
// DMA IRQ: two channels chained to each other (ping-pong). When one finishes,
// the hardware has already started the other, so there is no gap between
// buffers. Here we just re-arm the finished channel and hand its buffer off.
// ---------------------------------------------------------------------------
static void dma_handler()
{
    for (int i = 0; i < 2; ++i) {
        uint32_t mask = 1u << dma_chan[i];
        if (dma_hw->ints0 & mask) {
            dma_hw->ints0 = mask;  // clear IRQ
            // Reset write address for next time this channel is chained to.
            // Transfer count reloads automatically on trigger.
            dma_channel_set_write_addr(dma_chan[i], capture_buf[i], false);

            if (ready_idx != -1) {
                overruns = overruns + 1;   // previous buffer never taken
            }
            ready_idx = i;
            buffers_captured = buffers_captured + 1;
        }
    }
}

static void setup_dma_channel(uint chan, uint chain_to, uint16_t* buf)
{
    dma_channel_config cfg = dma_channel_get_default_config(chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg, false);   // ADC FIFO
    channel_config_set_write_increment(&cfg, true);   // buffer
    channel_config_set_dreq(&cfg, DREQ_ADC);          // paced by ADC samples
    channel_config_set_chain_to(&cfg, chain_to);      // ping-pong

    dma_channel_configure(chan, &cfg,
                          buf,             // dst
                          &adc_hw->fifo,   // src
                          CAPTURE_DEPTH,   // transfer count
                          false);          // don't start yet
    dma_channel_set_irq0_enabled(chan, true);
}

// ---------------------------------------------------------------------------
// Signal processing (runs in main loop, not in the ISR)
// ---------------------------------------------------------------------------
static void process_buffer(const uint16_t* buf)
{
    // ---- Onset detector (Q8 fixed point) ----
    int32_t  sum = 0;
    for (int i = 0; i < CAPTURE_DEPTH; ++i) {
        sum += buf[i];
    }
    int32_t dc_q = (int32_t)(((int64_t)sum << FIXQ) / CAPTURE_DEPTH);

    int32_t lta_safe = lta > MIN_LTA_Q ? lta : MIN_LTA_Q;
    int32_t pwr = 0;
    int64_t level_sum = 0;

    for (int i = 0; i < CAPTURE_DEPTH; ++i) {
        int32_t x = std::abs(((int32_t)buf[i] << FIXQ) - dc_q);
        level_sum += x;
        sta += (ALPHA * (x - sta)) / 100;
        int32_t ratio = (100 * sta) / lta_safe;
        if (ratio > pwr) {
            pwr = ratio;
        }
    }

    // Update background level AFTER detection so an onset doesn't
    // raise the baseline it's being compared against.
    int32_t level = (int32_t)(level_sum / CAPTURE_DEPTH);
    lta += (ALPHA_L * (level - lta)) / 100;

    // ---- Q signal conditioner + pitch detector (float, per sample) ----
    // Smooth the DC estimate across buffers to avoid steps at boundaries;
    // the conditioner's highpass removes whatever is left.
    dc_float += 0.1f * (float(sum) / CAPTURE_DEPTH - dc_float);
    constexpr float scale = 1.0f / 2048.0f;   // 12-bit -> roughly [-1, 1]

    bool gate_open = false;
    for (int i = 0; i < CAPTURE_DEPTH; ++i) {
        float s = (float(buf[i]) - dc_float) * scale;
        s = pitch->sc(s);
        pitch->pd(s);
        gate_open |= pitch->sc.gate();
    }

    // Report pitch only while the conditioner's gate says there's signal
    float freq = gate_open ? pitch->pd.get_frequency() : 0.0f;
    if (!gate_open) {
        pitch->pd.reset();
    }

#if PRINT_SAMPLES
    // One value per buffer: detected frequency in Hz, or 0 if none
    printf("%.1f\n", (double)freq);
#endif

    if (buffers_captured <= WARMUP_BUFS) {
        lta = level;  // fast init during warmup
        return;
    }

    if (pwr > THRESHOLD) {
        uint64_t now = to_us_since_boot(get_absolute_time());
        if ((now - last_trigger_us) > LOCKOUT_US) {
            led_state = !led_state;
            gpio_put(LED_GPIO, led_state);
            last_trigger_us = now;
        }
    }
}

int main()
{
    // Arduino-Pico does not support stdio_init_all(); use Serial.begin()/Serial.printf()
    // or the board's debug port instead when you need console output.

    // LED first, so it's ready before any processing happens
    gpio_init(LED_GPIO);
    gpio_set_dir(LED_GPIO, GPIO_OUT);
    gpio_put(LED_GPIO, 0);

    // Q processors
    auto sc_conf = q::signal_conditioner::config{};
    sc_conf.gate_onset_threshold   = GATE_ONSET;
    sc_conf.gate_release_threshold = GATE_RELEASE;
    pitch = new pitch_chain{
        q::signal_conditioner{sc_conf, PITCH_LOWEST, PITCH_HIGHEST, SAMPLE_RATE_HZ},
        q::pitch_detector{PITCH_LOWEST, PITCH_HIGHEST, SAMPLE_RATE_HZ, PITCH_HYSTERESIS}
    };

    adc_init();
    adc_gpio_init(26 + CAPTURE_CHANNEL);
    adc_select_input(CAPTURE_CHANNEL);

    adc_fifo_setup(
        true,    // write each conversion to the FIFO
        true,    // enable DMA DREQ
        1,       // DREQ when at least 1 sample present
        false,   // no ERR bit in samples
        false    // keep full 12-bit samples
    );

    // 48 kHz pacing
    adc_set_clkdiv(float(ADC_CLOCK_HZ / SAMPLE_RATE_HZ) - 1.0f);

    sleep_ms(1000);

    // Two DMA channels, each chained to the other
    dma_chan[0] = dma_claim_unused_channel(true);
    dma_chan[1] = dma_claim_unused_channel(true);
    setup_dma_channel(dma_chan[0], dma_chan[1], capture_buf[0]);
    setup_dma_channel(dma_chan[1], dma_chan[0], capture_buf[1]);

    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    // Start clean: drain stale samples, start channel 0, then the ADC
    adc_fifo_drain();
    dma_channel_start(dma_chan[0]);
    adc_run(true);

    while (true) {
        // Wait for a filled buffer
        while (ready_idx < 0) {
            __wfi();
        }

        // Grab it atomically
        uint32_t irq_state = save_and_disable_interrupts();
        int idx = ready_idx;
        ready_idx = -1;
        uint32_t seq = buffers_captured;
        restore_interrupts(irq_state);

        process_buffer(capture_buf[idx]);

        // If another buffer completed while we were processing, DMA has
        // already started overwriting the one we just used.
        if (buffers_captured != seq) {
            overruns = overruns + 1;
        }
    }
}