/*
 * MTMWS Workshop Computer — Oscilloscope Telemetry Card
 *
 * Streams decimated ADC samples (Audio1/2, CV1/2) plus knob/switch state
 * over USB CDC to a browser-based oscilloscope.
 *
 * Architecture:
 *   Core 0 : ComputerCard audio interrupt → fills TelemetryFrames at ~12kHz
 *            (48kHz / 4) and pushes them to a lock-free queue.
 *   Core 1 : Owns the TinyUSB stack. Pops frames, COBS-encodes them, and
 *            writes them to the host over USB CDC.
 *
 * IMPORTANT: This card OWNS the USB stack directly via TinyUSB.
 *   - stdio_usb MUST be disabled in CMake (it conflicts with manual CDC,
 *     and also interferes with the normalisation probe on GPIO4).
 *   - usb_descriptors.c must be compiled alongside this file.
 */

#include "ComputerCard.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/timer.h"
#include "bsp/board.h"      // TinyUSB board support (board_init)
#include "tusb.h"
#include <stdio.h>
#include <stdint.h>
#include <atomic>

// -----------------------------------------------------------------------------
// Telemetry frame
// -----------------------------------------------------------------------------
//
// Packed so the wire format matches exactly what the JS DataView expects.
// Size: 2 + 2 + 4*(64*2) + 3*2 + 1 + 1 = 524 bytes.
//
// Signed int16 is used for ADC payloads because ComputerCard's AudioIn/CVIn
// functions return signed values in the range -2048..+2047. The browser side
// decodes these with getInt16() and normalises by /2048.0.

struct __attribute__((packed)) TelemetryFrame {
    uint16_t syncWord;        // 0xAA55 magic for frame alignment
    uint16_t sampleCount;     // Number of valid samples in this frame (always 64 here)

    int16_t  audio1[64];      // Signed 12-bit audio, sign-extended into int16
    int16_t  audio2[64];
    int16_t  cv1[64];
    int16_t  cv2[64];

    uint16_t knobs[3];        // Main, X, Y — raw 0..4095 from KnobVal()
    uint8_t  switches;        // 0=Down, 1=Middle, 2=Up (from Switch enum)
    uint8_t  padding;         // Keep struct size aligned and explicit
};

static_assert(sizeof(TelemetryFrame) == 524,
              "TelemetryFrame size must match the JS parser (524 bytes)");

// -----------------------------------------------------------------------------
// Lock-free SPSC queue
// -----------------------------------------------------------------------------
//
// Single-producer (Core 0 ISR) / single-consumer (Core 1 loop). We use
// std::atomic with acquire/release ordering so the producer's writes to
// buffer[head] are visible to the consumer before it sees the head advance,
// and vice versa for tail. `volatile` alone is NOT sufficient for cross-core
// synchronisation — it only prevents the compiler from caching in registers.

template <typename T, size_t Size>
class LocklessQueue {
private:
    T buffer[Size];
    std::atomic<size_t> head{0};   // written by producer, read by consumer
    std::atomic<size_t> tail{0};   // written by consumer, read by producer
public:
    // Producer side (Core 0, called from audio ISR)
    bool push(const T& item) {
        const size_t h = head.load(std::memory_order_relaxed);
        const size_t next_head = (h + 1) % Size;
        if (next_head == tail.load(std::memory_order_acquire)) {
            return false; // Full — drop silently. Better than blocking the ISR.
        }
        buffer[h] = item;
        head.store(next_head, std::memory_order_release);
        return true;
    }

    // Consumer side (Core 1)
    bool pop(T& item) {
        const size_t t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) {
            return false; // Empty
        }
        item = buffer[t];
        tail.store((t + 1) % Size, std::memory_order_release);
        return true;
    }
};

// Queue holds 16 frames (~85ms of buffering at 12kHz / 64-sample frames).
// Deep enough to absorb USB host polling hiccups without dropping data.
LocklessQueue<TelemetryFrame, 16> telemetryQueue;

// -----------------------------------------------------------------------------
// COBS encoder
// -----------------------------------------------------------------------------
//
// Consistent Overhead Byte Stuffing — replaces all zero bytes in the payload
// so we can use 0x00 as an unambiguous frame delimiter on the wire. Overhead
// is at most 1 byte per 254 bytes of payload.

size_t CobsEncode(const uint8_t* ptr, size_t length, uint8_t* dst) {
    size_t read_index  = 0;
    size_t write_index = 1;
    size_t code_index  = 0;
    uint8_t code       = 1;

    while (read_index < length) {
        if (ptr[read_index] == 0) {
            dst[code_index] = code;
            code = 1;
            code_index = write_index++;
            read_index++;
        } else {
            dst[write_index++] = ptr[read_index++];
            code++;
            if (code == 0xFF) {
                dst[code_index] = code;
                code = 1;
                code_index = write_index++;
            }
        }
    }
    dst[code_index] = code;
    return write_index;
}

// -----------------------------------------------------------------------------
// DSP module (Core 0)
// -----------------------------------------------------------------------------
//
// ProcessSample() runs inside the ADC DMA completion interrupt at 48kHz.
// It MUST complete in well under ~20μs or the ADC/MUX will desync (knob
// values lock up, channels permute). Everything here is just array writes
// and a queue push — well within budget.

class TelemetryModule : public ComputerCard {
private:
    TelemetryFrame currentFrame;
    uint16_t sampleIndex       = 0;
    uint16_t decimationCounter = 0;

    // Decimation: 48kHz / 4 = 12kHz effective sample rate per channel.
    // At 64 samples per frame, that's ~187 frames/sec to the host.
    static constexpr uint16_t DECIMATION_FACTOR = 4;

public:
    TelemetryModule() : ComputerCard() {
        currentFrame.syncWord = 0xAA55;
        currentFrame.padding  = 0;
    }

protected:
    void ProcessSample() override {
        // Only sample every DECIMATION_FACTOR-th call (12kHz capture).
        if (++decimationCounter < DECIMATION_FACTOR) return;
        decimationCounter = 0;

        // Capture the four analogue channels. These are signed int16 from
        // ComputerCard, range -2048..+2047 (12-bit signed).
        currentFrame.audio1[sampleIndex] = AudioIn1();
        currentFrame.audio2[sampleIndex] = AudioIn2();
        currentFrame.cv1[sampleIndex]    = CVIn1();
        currentFrame.cv2[sampleIndex]    = CVIn2();

        if (++sampleIndex >= 64) {
            // Frame complete — snapshot the slow controls and ship it.
            currentFrame.sampleCount = 64;
            currentFrame.knobs[0]    = static_cast<uint16_t>(KnobVal(Knob::Main));
            currentFrame.knobs[1]    = static_cast<uint16_t>(KnobVal(Knob::X));
            currentFrame.knobs[2]    = static_cast<uint16_t>(KnobVal(Knob::Y));
            currentFrame.switches    = static_cast<uint8_t>(SwitchVal());

            // Non-blocking push. If the host is slow/disconnected we just
            // drop the frame rather than stall the audio interrupt.
            telemetryQueue.push(currentFrame);
            sampleIndex = 0;
        }
    }
};

TelemetryModule* moduleInstance;

// -----------------------------------------------------------------------------
// USB transmission thread (Core 1)
// -----------------------------------------------------------------------------
//
// This core owns the entire USB stack. We init TinyUSB here (NOT via
// stdio_init_all — that path is disabled in CMake). The loop services
// USB events and drains the telemetry queue to the host.

void core1_usb_thread() {
    board_init();
    tusb_init();

    TelemetryFrame outFrame;
    uint8_t cobsBuffer[sizeof(TelemetryFrame) + 5];

    while (true) {
        tud_task();

        if (tud_cdc_connected()) {
            if (telemetryQueue.pop(outFrame)) {
                size_t encodedLength = CobsEncode(
                    reinterpret_cast<const uint8_t*>(&outFrame),
                    sizeof(TelemetryFrame),
                    cobsBuffer);

                cobsBuffer[encodedLength] = 0x00;        // frame delimiter
                const size_t totalLength = encodedLength + 1;

                // The encoded frame (~525 bytes) is larger than the CDC TX FIFO
                // (CFG_TUD_CDC_TX_BUFSIZE = 512). We must stream it out in
                // pieces, draining the FIFO between writes by servicing
                // tud_task() and flushing. The previous "all-or-nothing"
                // check meant we never wrote anything — that was the bug
                // hiding all telemetry.
                size_t written = 0;
                while (written < totalLength) {
                    uint32_t avail = tud_cdc_write_available();
                    if (avail == 0) {
                        // FIFO full — let the host drain it.
                        tud_cdc_write_flush();
                        tud_task();
                        continue;
                    }
                    size_t remaining = totalLength - written;
                    size_t chunk = (avail < remaining) ? avail : remaining;
                    written += tud_cdc_write(cobsBuffer + written, chunk);
                }
                tud_cdc_write_flush();
            }
        } else {
            // No host — drain the queue so reconnect is clean.
            while (telemetryQueue.pop(outFrame)) { /* discard */ }
        }
    }
}

// -----------------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------------

int main() {
    // NOTE: no stdio_init_all() — stdio_usb is disabled in CMake because
    // it conflicts with our manual TinyUSB CDC use and interferes with
    // the normalisation probe on GPIO4.

    moduleInstance = new TelemetryModule();
    moduleInstance->EnableNormalisationProbe();

    // Launch the USB worker on Core 1 before starting the audio loop,
    // so the host can enumerate while audio is initialising.
    multicore_launch_core1(core1_usb_thread);

    // Run() blocks forever, servicing the 48kHz audio interrupt.
    moduleInstance->Run();
    return 0; // unreachable
}