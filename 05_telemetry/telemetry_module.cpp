#include "ComputerCard.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/timer.h"
#include "tusb.h"
#include <stdio.h>
#include <stdint.h>

// --- Data Structures ---

// 32-bit aligned structure for efficient memory transfer
struct TelemetryFrame {
    uint16_t syncWord;        // Magic number (0xAA55) to verify frame integrity
    uint16_t sampleCount;     // Variable defining active payload size
    
    // Arrays containing decimated post-compensation ADC values
    uint16_t audio1[64];      
    uint16_t audio2[64];
    uint16_t cv1[64];
    uint16_t cv2[64];
    
    // Hardware interface state
    uint16_t knobs[3];        // Values representing Main, X, and Y potentiometers
    uint8_t  switches;        // State of the 3-position toggle (Down, Middle, Up)
    uint8_t  padding;         // Empty byte appended to ensure 32-bit boundary alignment
};

// --- Lockless SPSC Queue ---

template <typename T, size_t Size>
class LocklessQueue {
private:
    T buffer[Size];
    volatile size_t head = 0;
    volatile size_t tail = 0;
public:
    // Executed entirely on Core 0
    bool push(const T& item) {
        size_t next_head = (head + 1) % Size;
        if (next_head == tail) return false; // Buffer Full - Fail silently
        buffer[head] = item;
        head = next_head;
        return true;
    }

    // Executed entirely on Core 1
    bool pop(T& item) {
        if (head == tail) return false; // Buffer Empty
        item = buffer[tail];
        tail = (tail + 1) % Size;
        return true;
    }
};

// Instantiate queue deep enough to absorb USB host polling latency
LocklessQueue<TelemetryFrame, 16> telemetryQueue;

// --- COBS Encoder ---

size_t CobsEncode(const uint8_t* ptr, size_t length, uint8_t* dst) {
    size_t read_index = 0;
    size_t write_index = 1;
    size_t code_index = 0;
    uint8_t code = 1;

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

// --- Main DSP Module (Core 0) ---

class TelemetryModule : public ComputerCard {
private:
    TelemetryFrame currentFrame;
    uint16_t sampleIndex = 0;
    uint16_t decimationCounter = 0;
    const uint16_t DECIMATION_FACTOR = 4; // Effective 12kHz sample rate

public:
    TelemetryModule() : ComputerCard() {
        currentFrame.syncWord = 0xAA55;
    }

protected:
    void ProcessSample() override {
        // Increment decimation counter
        decimationCounter++;
        if (decimationCounter >= DECIMATION_FACTOR) {
            decimationCounter = 0;
            
            // Populate arrays using the specific ComputerCard API functions
            currentFrame.audio1[sampleIndex] = AudioIn1();
            currentFrame.audio2[sampleIndex] = AudioIn2();
            currentFrame.cv1[sampleIndex]    = CVIn1();
            currentFrame.cv2[sampleIndex]    = CVIn2();
            
            sampleIndex++;
            
            if (sampleIndex >= 64) {
                currentFrame.sampleCount = 64;
                currentFrame.knobs[0] = KnobVal(Knob::Main); 
                currentFrame.knobs[1] = KnobVal(Knob::X);    
                currentFrame.knobs[2] = KnobVal(Knob::Y);    
                currentFrame.switches = static_cast<uint8_t>(SwitchVal()); 
                
                // Transmit to Core 1
                telemetryQueue.push(currentFrame);
                sampleIndex = 0;
            }
        }
    }
};

TelemetryModule* moduleInstance;

// --- USB Transmission Thread (Core 1) ---

void core1_usb_thread() {
    // REMOVE tusb_init(); - The SDK handles this now
    
    TelemetryFrame outFrame;
    uint8_t cobsBuffer[sizeof(TelemetryFrame) + 5]; 
    
    while (true) {
        // tud_task() is still required to process the USB stack
        tud_task(); 
        
        if (tud_cdc_connected()) {
            if (telemetryQueue.pop(outFrame)) {
                size_t encodedLength = CobsEncode((uint8_t*)&outFrame, sizeof(TelemetryFrame), cobsBuffer);
                cobsBuffer[encodedLength] = 0x00; 
                
                if (tud_cdc_write_available() >= (encodedLength + 1)) {
                    tud_cdc_write(cobsBuffer, encodedLength + 1);
                    tud_cdc_write_flush(); 
                }
            }
        } else {
            while (telemetryQueue.pop(outFrame)); // Drain queue
        }
        
        // Minor sleep to yield
        sleep_us(100); 
    }
}

// --- Entry Point ---

int main() {
    stdio_init_all(); 
    
    moduleInstance = new TelemetryModule();
    moduleInstance->EnableNormalisationProbe(); 
    
    // Launch Core 1 context 
    multicore_launch_core1(core1_usb_thread);
    
    // Launch Core 0 audio loop (Blocks indefinitely)
    moduleInstance->Run(); 
    
    return 0;
}