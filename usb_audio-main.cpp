/*
 * Workshop System Computer - Audio & MIDI Firmware
 * ================================================
 * 
 * Provides a composite USB device (UAC1 Audio + MIDI) for the Workshop System Computer.
 * 
 * MODES OF OPERATION:
 * 
 * [MODE 1] Normal Operation (Switch: Middle)
 * - Functions as a standard Multi-Channel USB Audio Interface.
 * - All hardware inputs/outputs are mapped 1:1 to USB Audio streams.
 * 
 * [MODE 2] Alt / CV Interface Mode (Switch: Up)
 * - Configurable mixed audio and Midi to CV interface.
 * 
 * [MODE 3] Audio Only Mode (Switch: Down)
 * - Disables all MIDI functionality (USB Descriptor & Tasks).
 * - Pure USB Audio Interface (maximizes bandwidth for audio stability).
 * 
 * CONFIGURATION & BANDWIDTH:
 * 
 * - Defaults: 44.1 kHz, 4 Channels.
 * - Input Mapping:
 *   - Audio 1/2: Audio Stream
 *   - CV 1: Pitch (Ch 1)
 *   - CV 2: CC 4 (Ch 1)
 *   - Pulse 1: Gate (for CV 1)
 * - Output Mapping:
 *   - Audio 1/2: Audio Stream
 *   - CV 1: Pitch (Ch 1)
 *   - CV 2: CC 4 (Ch 1)
 *   - Pulse 1: Gate
 *   - Pulse 2: Clock
 * - Knobs:
 *   - Main: CC 1 (Ch 1)
 *   - X: CC 2 (Ch 1)
 *   - Y: CC 3 (Ch 1)
 * - Settings are adjustable via the Workshop System Web Interface.
 * 
 * [WARNING] USB 1.1 Bandwidth Limits
 * - Bandwidth is extremely tight on USB 1.1 Full Speed.
 * - macOS: Typically stable at 6 Channels @ 48kHz.
 * - Windows: May require reducing content to 6ch@24kHz or 4ch@44.1kHz.
 * - Linux: 2 channel audio works up to 48kHz. 4 channel audio works up to 24kHz
 * 
 * Please test different configurations/ Audio Drivers for stability on your specific system.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "bsp/board_api.h"
#include "tusb.h"
#include "hardware/pwm.h"
#include "usb_descriptors.h"
#include "ring_buffer.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h" 
#include "pico/bootrom.h"

// Include ComputerCard header-only library
#include "ComputerCard/ComputerCard.h"
#include "quantiser.h"

//--------------------------------------------------------------------+
// Configuration
//--------------------------------------------------------------------+
struct GlobalConfig {    // Version 17: Matrix Config
    uint8_t version;
    
    // USB Stream Masks (Bitmasks 0-5)
    uint8_t usbOutMask; // Which physical channels (0-5) consume USB Audio Out streams
    uint8_t usbInMask;  // Which physical channels (0-5) produce USB Audio In streams
    
    // Per-Channel Configuration (Physical Channels 0-5)
    // 0=Audio 1, 1=Audio 2, 2=CV 1, 3=CV 2, 4=Pulse 1, 5=Pulse 2
    
    // Output Config (PC -> Device)
    uint8_t outMode[6];     // 0=Audio, 1=Pitch, 2=CC, 3=Gate, 4=Trigger, 5=Clock, 6=Binary
    uint8_t outChannel[6];  // MIDI Channel (1-16, 0=Omni)
    uint8_t outCC[6];       // CC Number (for Mode 2)
    uint8_t pulsePPQN[2];   // PPQN for Pulse Clock Mode (Index 0 for Pulse 1, 1 for Pulse 2)
    uint8_t pulseOutBinary[2]; // Legacy/Helper (0=PWM, 1=Binary) - kept for compatibility/simplicity
    
    // Input Config (Device -> PC)
    uint8_t inMode[6];      // 0=Audio, 1=Pitch(Note), 2=CC, 3=Gate(future)
    uint8_t inChannel[6];   // MIDI Channel (1-16)
    uint8_t inCC[6];        // CC Number
    
    // Global Control
    uint8_t knobMainCC;
    uint8_t knobXCC;
    uint8_t knobYCC;
    uint8_t sampleRateIdx;
    
    // Constructor with defaults (V18)
    GlobalConfig() :
        version(18),
        usbOutMask(0x0F), // Audio 1/2 + CV 1/2 enabled (4 channels)
        usbInMask(0x0F),  // Audio 1/2 + CV 1/2 enabled (4 channels)
        
        outMode{0, 0, 1, 2, 3, 5}, // A1=Audio, A2=Audio, CV1=Pitch, CV2=CC, P1=Gate, P2=Clock
        outChannel{0, 0, 0, 0, 0, 0}, // A1/A2/CV1/CV2=Ch1, P1=Omni, P2=Ch1 (0=Omni for outputs)
        outCC{0, 0, 0, 4, 0, 0},      // CV2 CC=4
        pulsePPQN{24, 24},
        pulseOutBinary{1, 1},         // Binary mode (not PWM)
        
        inMode{0, 0, 1, 2, 1, 1},     // A1/A2=Audio, CV1=Pitch, CV2=CC, P1=Gate, P2=Gate
        inChannel{0, 0, 0, 0, 0, 1},  // CV1/CV2/P1=Ch1, P2=Ch2 (0 means Ch1 for inputs)
        inCC{0, 0, 0, 4, 0, 0},       // CV2 CC=4
        
        knobMainCC(1),
        knobXCC(2),
        knobYCC(3),
        sampleRateIdx(1)              // 44.1 kHz
    {
    }
};

GlobalConfig config{}; // Brace initialization ensures default member initializers are used
 
uint8_t sysExBuffer[128];
uint8_t sysExPtr = 0;

// Expose Sample Rate and Channel Count to C (usb_descriptors.c)

extern "C" uint8_t g_sampleRateIdx = 0;
extern "C" uint8_t g_channelsOut = 6;
extern "C" uint8_t g_channelsIn = 6;
// Global flag for Audio Only mode (Debug)
extern "C" bool g_audioOnly = false;

// Check Switch position at boot (Mux State 3, ADC Ch 2)
// Returns true if Switch is DOWN
bool CheckDebugSwitch() {
    // 1. Init Mux Pins (24, 25)
    gpio_init(24); gpio_set_dir(24, GPIO_OUT);
    gpio_init(25); gpio_set_dir(25, GPIO_OUT);
    
    // Set Mux to State 3 (Switch) -> Both High
    gpio_put(24, 1);
    gpio_put(25, 1);
    
    // 2. Init ADC
    adc_init();
    adc_gpio_init(28); // ADC Ch 2
    adc_select_input(2);
    
    // Stabilize
    sleep_ms(1);
    
    // 3. Read
    uint16_t val = adc_read();
    
    // Switch Levels: Down < 1000, Middle 1000-3000, Up > 3000
    return (val < 1000);
}

// LED Flash Timer
volatile int32_t configFlashTimer = 0;

// Flash Persistence
#define FLASH_TARGET_OFFSET (PICO_FLASH_SIZE_BYTES - 4096)
const uint8_t *flash_target_contents = (const uint8_t *) (XIP_BASE + FLASH_TARGET_OFFSET);

// Helper: Count Set Bits
int CountBits(uint8_t n) {
    int count = 0;
    while (n > 0) {
        if (n & 1) count++;
        n >>= 1;
    }
    return count;
}

// Helper: Pad to Even (2, 4, 6)
int PadToEven(int c) {
    if (c <= 0) return 2;
    if (c % 2 != 0) return c + 1;
    return c;
}


void LoadConfigFromFlash() {
    uint8_t *flash_target_contents = (uint8_t *) (XIP_BASE + FLASH_TARGET_OFFSET);
    
    // Simple verification - if Version matches, load. Else defaults.

    // Version 18 introduced new defaults and Pulse Binary logic.
    if (flash_target_contents[0] == 18) {
        memcpy(&config, flash_target_contents, sizeof(GlobalConfig));
    } else {
        // Fallback or Migration could go here. For now, defaults.
        config = GlobalConfig();
    }
    
    // Update Globals
    g_sampleRateIdx = config.sampleRateIdx;
    g_channelsOut = PadToEven(CountBits(config.usbOutMask));
    g_channelsIn = PadToEven(CountBits(config.usbInMask));
}

void SaveConfigToFlash() {
    // Determine the interrupt implementation from SDK or locally
    // We need to pause Core 1 while writing flash
    multicore_lockout_start_blocking();
    
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_TARGET_OFFSET, 4096);
    flash_range_program(FLASH_TARGET_OFFSET, (const uint8_t *)&config, sizeof(GlobalConfig));
    restore_interrupts(ints);
    
    multicore_lockout_end_blocking();
}

void ProcessSysEx(uint8_t* data, uint8_t len) {
    // Expect [F0, 7D, CMD, ... F7]
    if (len < 3 || data[0] != 0xF0 || data[1] != 0x7D) return;
    
    uint8_t cmd = data[2];
    
    // Debug: Blink LED 1 on Receive
    gpio_put(25, 1);
    busy_wait_ms(10);
    gpio_put(25, 0);
    
    /* V17 Protocol LAYOUT (CMD 1 & 3):
       0: F0
       1: 7D
       2: CMD
       3: usbOutMask
       4: usbInMask
       5-10: outMode[6]
       11-16: outChannel[6]
       17-22: outCC[6]
       23-28: inMode[6]
       29-34: inChannel[6]
       35-40: inCC[6]
       41: knobMainCC
       42: knobXCC
       43: knobYCC
       44: sampleRateIdx
       45: pulsePPQN[0]
       46: pulsePPQN[1]
       47: pulseOutBinary[0]
       48: pulseOutBinary[1]
       49: F7
       Total Length: 50 bytes
    */
    
    // CMD 1: PREVIEW (Update RAM)
    if (cmd == 1 && len >= 50) {
        config.usbOutMask = data[3];

        config.usbInMask = data[4];
        
        for(int i=0; i<6; i++) config.outMode[i] = data[5+i];
        for(int i=0; i<6; i++) config.outChannel[i] = data[11+i];
        for(int i=0; i<6; i++) config.outCC[i] = data[17+i];
        
        for(int i=0; i<6; i++) config.inMode[i] = data[23+i];
        for(int i=0; i<6; i++) config.inChannel[i] = data[29+i];
        for(int i=0; i<6; i++) config.inCC[i] = data[35+i];

        config.knobMainCC = data[41];
        config.knobXCC = data[42];
        config.knobYCC = data[43];
        config.sampleRateIdx = data[44];
        config.pulsePPQN[0] = data[45];
        config.pulsePPQN[1] = data[46];
        config.pulseOutBinary[0] = data[47];
        config.pulseOutBinary[1] = data[48];
        
        // Update Globals for Descriptor - REMOVED
        // We MUST NOT update these until reboot, otherwise audio_task desyncs with Host
        // g_sampleRateIdx = config.sampleRateIdx;
        // g_channelsOut = PadToEven(CountBits(config.usbOutMask));
        // g_channelsIn = PadToEven(CountBits(config.usbInMask));
        
        configFlashTimer = 500; 
    }
    
    // CMD 2: WRITE FLASH
    if (cmd == 2) {
         SaveConfigToFlash();
         configFlashTimer = 1000; 
    }
    
    // CMD 3: READ (Send Current Config)
    if (cmd == 3) {
        uint8_t response[50];
        response[0] = 0xF0;
        response[1] = 0x7D;
        response[2] = 3; 


        response[3] = config.usbOutMask;
        response[4] = config.usbInMask;
        
        for(int i=0; i<6; i++) response[5+i] = config.outMode[i];
        for(int i=0; i<6; i++) response[11+i] = config.outChannel[i];
        for(int i=0; i<6; i++) response[17+i] = config.outCC[i];
        
        for(int i=0; i<6; i++) response[23+i] = config.inMode[i];
        for(int i=0; i<6; i++) response[29+i] = config.inChannel[i];
        for(int i=0; i<6; i++) response[35+i] = config.inCC[i];

        response[41] = config.knobMainCC;
        response[42] = config.knobXCC;
        response[43] = config.knobYCC;
        response[44] = config.sampleRateIdx;
        response[45] = config.pulsePPQN[0];
        response[46] = config.pulsePPQN[1];
        response[47] = config.pulseOutBinary[0];
        response[48] = config.pulseOutBinary[1];

        response[49] = 0xF7;
        tud_midi_stream_write(0, response, 50);
    }

    // CMD 4: REBOOT
    if (cmd == 4) watchdog_reboot(0, 0, 0);

    // CMD 5: BOOTLOADER
    if (cmd == 5) reset_usb_boot(0, 0);
}

//--------------------------------------------------------------------+
// Audio Ring Buffers (shared between cores)
//--------------------------------------------------------------------+
audio_ring_buffer_t audioInRB;   // ADC -> USB (Mic)
audio_ring_buffer_t audioOutRB;  // USB -> DAC (Speaker)
audio_ring_buffer_t midiInRB;    // Core 1 (CV/Knobs) -> Core 0 (USB MIDI TX)
audio_ring_buffer_t midiOutRB;   // Core 0 (USB MIDI RX) -> Core 1 (CV Logic)

//--------------------------------------------------------------------+
// TinyUSB Audio Configuration
//--------------------------------------------------------------------+
const uint32_t sample_rates[] = {48000, 44100, 24000};
#define N_SAMPLE_RATES TU_ARRAY_SIZE(sample_rates)

uint32_t current_sample_rate = 48000;

// Audio controls
uint8_t mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];
int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];

// Buffer for speaker data
int16_t spk_buf[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2];
int spk_data_size;

// Current resolution
uint8_t current_resolution = 16;

//--------------------------------------------------------------------+
// ComputerCard Audio Processing (runs on Core 1 @ 48kHz)
//--------------------------------------------------------------------+
// Voice Logic for Note Stacking and Pitch/Gate
struct Voice {
    uint8_t noteStack[8] = {255, 255, 255, 255, 255, 255, 255, 255};
    int noteStackPtr = 0;
    uint8_t activeNote = 255;
    uint8_t lastValidNote = 60; // Default C4
    uint16_t pitchBend = 8192;
    bool gateState = false;
    
    void PushNote(uint8_t note) {
        for (int i=0; i<noteStackPtr; i++) {
            if (noteStack[i] == note) {
                for (int j=i; j<noteStackPtr-1; j++) noteStack[j] = noteStack[j+1];
                noteStackPtr--;
                break;
            }
        }
        if (noteStackPtr < 8) noteStack[noteStackPtr++] = note;
        activeNote = noteStack[noteStackPtr-1];
        lastValidNote = activeNote; // Update S&H
    }
    
    void PopNote(uint8_t note) {
         for (int i=0; i<noteStackPtr; i++) {
            if (noteStack[i] == note) {
                for (int j=i; j<noteStackPtr-1; j++) noteStack[j] = noteStack[j+1];
                noteStackPtr--;
                break;
            }
        }
        if (noteStackPtr > 0) {
            activeNote = noteStack[noteStackPtr-1];
            lastValidNote = activeNote; // Update S&H
        }
        else activeNote = 255;
        
        if (noteStackPtr == 0) gateState = false;
    }
    
    uint8_t GetNote(int historyIndex) {
        // historyIndex 0 = Latest, 1 = Previous
        if (noteStackPtr > historyIndex) return noteStack[noteStackPtr - 1 - historyIndex];
        // If requesting current note but stack empty, return last valid (Sample & Hold)
        if (historyIndex == 0 && noteStackPtr == 0) return lastValidNote;
        return 255;
    }

    int32_t GetPitchMV(int historyIndex = 0) {
        uint8_t note = GetNote(historyIndex);
        if (note == 255) return 0; // Should only happen for historyIndex > 0
        int32_t noteBase_mv = (int32_t)(note - 60) * 1000 * 100 / 12; 
        int32_t bendDelta = (int32_t)pitchBend - 8192; 
        int32_t bend_mv = (bendDelta * 16666) / 8192; 
        return (noteBase_mv + bend_mv) / 100;
    }
};

//--------------------------------------------------------------------+
// ComputerCard Audio Processing (runs on Core 1 @ 48kHz)
//--------------------------------------------------------------------+
class AudioCard : public ComputerCard
{
public:
    // Input State
    int16_t inNote[6] = {-1, -1, -1, -1, -1, -1};
    int16_t inCCVal[6] = {-1, -1, -1, -1, -1, -1};
    bool inGate[6] = {false, false, false, false, false, false};
    
    int32_t lastKnobMain = -1;
    int32_t lastKnobX = -1;
    int32_t lastKnobY = -1;
    int sampleCounter = 0;


    void ProcessKnob(Knob k, int32_t *lastVal, uint8_t ccNum)
    {
        int32_t raw = KnobVal(k); 
        // Hysteresis: Require significant change (>16 raw units) to update
        if (abs(raw - *lastVal) > 16 || *lastVal == -1) 
        {
            int ccVal = (raw * 127) / 4095; // Map 12-bit to 7-bit
            int lastCC = (*lastVal * 127) / 4095;
            
            if (ccVal != lastCC || *lastVal == -1)
            {
                // Format: Status | CC# | Val | 0
                uint32_t evt = 0xB0 | (ccNum << 8) | (ccVal << 16);
                rb_push(&midiInRB, evt);
                *lastVal = raw; 
            }
        }
    }

    void ProcessAltModeInputs()
    {
        for(int i=0; i<6; i++) {
            // Read Input (16-bit signed)
            int16_t val = ReadPhysicalIn(i); 
            
            // MIDI Generation Logic
            // Determine Mode:
            // CV Inputs (2, 3): 1=Pitch, 2=CC
            // Pulse Inputs (4, 5): 1=Gate
            // Audio Inputs (0, 1): Normally 0 (Audio)
            
            int mode = config.inMode[i];
            
            // Channel & CC
            // Note: config stores channel 1-16. MIDI status needs 0-15.
            uint8_t midiCh = (config.inChannel[i] > 0) ? (config.inChannel[i] - 1) : 0;
            
            if (i == 2 || i == 3) { // CV Inputs
                if (mode == 1) { // Pitch
                    int16_t note = quantSample(val >> 4); // 16-bit -> 12-bit
                    
                    if (note != inNote[i]) {
                        // Check Gate from associated Pulse input
                        // CV1 (2) -> Pulse1 (4)
                        // CV2 (3) -> Pulse1 (5)
                        int gateIdx = (i == 2) ? 4 : 5;
                        bool gateActive = inGate[gateIdx]; 
                        
                        if (gateActive) {
                            if (inNote[i] != -1) rb_push(&midiInRB, 0x80 | midiCh | (inNote[i] << 8) | (0 << 16)); // Note Off Old
                            rb_push(&midiInRB, 0x90 | midiCh | (note << 8) | (100 << 16)); // Note On New
                        }
                        inNote[i] = note;
                    }
                }
                else if (mode == 2) { // CC
                    // Map -32768..32767 to 0..4095 then 0..127?
                    // (val >> 4) is -2048..2047. +2048 -> 0..4095
                    int ccVal = (((val >> 4) + 2048) * 127) / 4096;
                    ccVal = (ccVal < 0) ? 0 : (ccVal > 127 ? 127 : ccVal);
                    
                    if (abs(ccVal - inCCVal[i]) > 0) { // Sensitive
                        inCCVal[i] = ccVal;
                        rb_push(&midiInRB, 0xB0 | midiCh | (config.inCC[i] << 8) | (ccVal << 16));
                    }
                }
            }
            else if (i == 4 || i == 5) { // Pulse Inputs
                if (mode == 1) { // Gate
                    bool high = (val > 0); // Active High
                    
                    if (high != inGate[i]) {
                        inGate[i] = high;
                        
                        // Trigger Note On/Off for associated CV
                        int cvIdx = (i == 4) ? 2 : 3;
                        if (config.inMode[cvIdx] == 1) { // If CV is Pitch Mode
                            int16_t note = inNote[cvIdx];
                            uint8_t cvCh = (config.inChannel[cvIdx] > 0) ? (config.inChannel[cvIdx] - 1) : 0;
                            
                            if (high) {
                                if (note != -1) rb_push(&midiInRB, 0x90 | cvCh | (note << 8) | (100 << 16));
                            } else {
                                if (note != -1) rb_push(&midiInRB, 0x80 | cvCh | (note << 8) | (0 << 16));
                            }
                        }
                    }
                }
            }
        }
    }
    
    // Constructor to initialize PWM
    AudioCard() {
        EnableNormalisationProbe(); // Enable Jack Detection
        gpio_set_function(8, GPIO_FUNC_PWM);
        gpio_set_function(9, GPIO_FUNC_PWM);
        uint slice_num = pwm_gpio_to_slice_num(8); 
        pwm_set_wrap(slice_num, 1023); 
        pwm_set_enabled(slice_num, true);
    }
    
    // ===== V17 STATE GLOBALS =====
    Voice voices[6]; 
    uint8_t midiClockCount[6] = {0};
    bool clockPulseActive[6] = {false};
    int clockPulseTimer[6] = {0};
    // Pulse output mode tracking
    bool pulse1_is_gpio = false;
    bool pulse2_is_gpio = false;
    // (Moved to top)


    void WritePhysicalOut(int ch, int16_t val) {
        switch(ch) {
            case 0: AudioOut1(val >> 4); break; // Audio 1
            case 1: AudioOut2(val >> 4); break; // Audio 2
            case 2: CVOut1(val); break; // CV 1
            case 3: CVOut2(val); break; // CV 2
            case 4: // Pulse 1
                if (config.pulseOutBinary[0]) {
                     // Binary Mode: Switch to GPIO and set HIGH/LOW
                     if (!pulse1_is_gpio) {
                         gpio_set_function(8, GPIO_FUNC_SIO);
                         gpio_set_dir(8, GPIO_OUT);
                         pulse1_is_gpio = true;
                     }
                     gpio_put(8, !(val > 0)); // Inverted: true -> LOW (Jack HIGH)
                } else {
                     // PWM Mode: Switch to PWM if currently in GPIO mode
                     if (pulse1_is_gpio) {
                         gpio_set_function(8, GPIO_FUNC_PWM);
                         pulse1_is_gpio = false;
                     }
                     pwm_set_gpio_level(8, 1023 - ((val + 32768) >> 6));
                }
                break;
            case 5: // Pulse 2
                if (config.pulseOutBinary[1]) {
                     // Binary Mode: Switch to GPIO and set HIGH/LOW
                     if (!pulse2_is_gpio) {
                         gpio_set_function(9, GPIO_FUNC_SIO);
                         gpio_set_dir(9, GPIO_OUT);
                         pulse2_is_gpio = true;
                     }
                     gpio_put(9, !(val > 0)); // Inverted: true -> LOW (Jack HIGH)
                } else {
                     // PWM Mode: Switch to PWM if currently in GPIO mode
                     if (pulse2_is_gpio) {
                         gpio_set_function(9, GPIO_FUNC_PWM);
                         pulse2_is_gpio = false;
                     }
                     pwm_set_gpio_level(9, 1023 - ((val + 32768) >> 6));
                }
                break;
        }
    }

    int16_t ReadPhysicalIn(int ch) {
        switch(ch) {
            case 0: return AudioIn1() << 4;
            case 1: return AudioIn2() << 4;
            case 2: return CVIn1() << 4;
            case 3: return CVIn2() << 4;
            case 4: return PulseIn1() ? 32767 : 0; // Unipolar 0..1
            case 5: return PulseIn2() ? 32767 : 0; // Unipolar 0..1
        }
        return 0;
    }

    void ProcessSample() override
    {
        bool altMode = (SwitchVal() == Switch::Up);

        // ===== KNOB & CV POLLING (10ms) =====
        // Throttled to 10ms (480 samples) to prevent MIDI CC flooding
        if (++sampleCounter % 480 == 0) {
            ProcessKnob(Knob::Main, &lastKnobMain, config.knobMainCC); 
            ProcessKnob(Knob::X, &lastKnobX, config.knobXCC);      
            ProcessKnob(Knob::Y, &lastKnobY, config.knobYCC);   

            if (altMode) ProcessAltModeInputs();
        }

        // ===== MIDI RX & STATE UPDATE =====
        // ===== MIDI RX & STATE UPDATE =====
        uint32_t evt;
        int evt_count = 0;
        // Limit to 4 events per sample to absolutely guarantee audio deadline
        // Process in both modes to drain buffer, but only act on events if altMode
        while(evt_count++ < 4 && rb_pop(&midiOutRB, &evt)) {
             if (!altMode) continue; // Drain and discard in normal mode

             uint8_t status = evt & 0xFF;
                 uint8_t d1 = (evt >> 8) & 0xFF;
                 uint8_t d2 = (evt >> 16) & 0xFF;
                 uint8_t cmd = status & 0xF0;
                 uint8_t ch = status & 0x0F;
                 
                 if (status == 0xF8) {
                      for(int i=0; i<6; i++) {
                          if (config.outMode[i] == 5) { // Clock
                              midiClockCount[i]++;
                              if (midiClockCount[i] >= config.pulsePPQN[i%2]) {
                                  midiClockCount[i] = 0;
                                  clockPulseActive[i] = true;
                                  clockPulseTimer[i] = 200; 
                              }
                          }
                      }
                      continue;
                  }

                 for(int i=0; i<6; i++) {
                     bool match = (config.outChannel[i] == 0) || ((config.outChannel[i] - 1) == ch);
                     if (!match) continue;

                     if (cmd == 0x90 && d2 > 0) { // Note On
                         voices[i].PushNote(d1);
                         voices[i].gateState = true;
                     }
                     else if (cmd == 0x80 || (cmd == 0x90 && d2 == 0)) { // Note Off
                         voices[i].PopNote(d1);
                     }
                     else if (cmd == 0xE0) { // Pitch Bend
                         uint16_t pb = d1 | (d2 << 7);
                         voices[i].pitchBend = pb;
                     }
                     else if (cmd == 0xB0) { // CC
                          if (config.outMode[i] == 2 && d1 == config.outCC[i]) {
                              // TODO: Implement CC value persistence/handling
                          }
                     }
                 }
             }

        
        // Update Pulse Timers
        for(int i=0; i<6; i++) {
            if (clockPulseActive[i]) {
                if (clockPulseTimer[i] > 0) clockPulseTimer[i]--;
                else clockPulseActive[i] = false;
            }
        }
        
        static uint32_t led_counter = 0;
        led_counter++;
        
        // ===== OUTPUT LOGIC =====
        int streamIdx = 0;
        int16_t streamData[6] = {0,0,0,0,0,0}; 
        int wordsToPop = 3; // ALWAYS 3 words (6 channels) on internal RB bus
        
        if (rb_count(&audioOutRB) >= wordsToPop) {
            for(int w=0; w<wordsToPop; w++) {
                uint32_t s;
                rb_pop(&audioOutRB, &s);
                if (streamIdx < 6) streamData[streamIdx++] = (int16_t)(s & 0xFFFF);
                if (streamIdx < 6) streamData[streamIdx++] = (int16_t)(s >> 16);
            }
        } else {
             for(int k=0; k<6; k++) streamData[k] = 0;
        }

        int currentStreamPtr = 0;
        for(int i=0; i<6; i++) {
            bool usbEnabled = (config.usbOutMask & (1<<i));
            // Audio 1/2 (channels 0-1) ALWAYS passthrough USB audio
            // CV/Pulse (2-5): If mode is Audio (0), use USB audio. Otherwise use MIDI modes in alt mode.
            bool forceMidi = altMode && (i >= 2) && (config.outMode[i] != 0);
            
            if (usbEnabled && !forceMidi) {
                // streamData contains normalized 6ch audio (padded with 0s if USB is 2/4ch)
                if (currentStreamPtr < 6) {
                    WritePhysicalOut(i, streamData[currentStreamPtr++]);
                }
            } else {
                // Mode Fallback
                int16_t outVal = 0;
                uint8_t mode = config.outMode[i];
                if (mode == 1) { // Pitch
                     outVal = (int16_t)(voices[i].GetPitchMV(0) * 1.0f); // TODO Scale
                } else if (mode == 3) { // Gate
                    outVal = voices[i].gateState ? 32767 : 0;
                } else if (mode == 5) { // Clock
                    outVal = clockPulseActive[i] ? 32767 : 0;
                } else if (mode == 6) { // Binary
                    // TODO: Implement Binary mode setting
                    outVal = voices[i].gateState ? 32767 : 0;
                }
                WritePhysicalOut(i, outVal);
            }
        }
        
        // ===== INPUT LOGIC =====
        streamIdx = 0;
        // int16_t inStream[6]; // Re-use streamData
        
        for(int i=0; i<6; i++) {
            if (config.usbInMask & (1<<i)) {
                if (streamIdx < 6) streamData[streamIdx++] = ReadPhysicalIn(i);
            }
        }
        while(streamIdx < 6) streamData[streamIdx++] = 0; // Pad
        
        // Input Bus is also 6-channel fixed
        if (rb_count(&audioInRB) < (AUDIO_BUFFER_SIZE - 4)) {
            for(int w=0; w<3; w++) {
                uint32_t s = (uint16_t)streamData[w*2] | ((uint32_t)(uint16_t)streamData[w*2+1] << 16);
                rb_push(&audioInRB, s);
            }
        }
        
        // LEDs
        if (led_counter % 1000 == 0) {
            LedOn(0, tud_mounted());
             if (configFlashTimer > 0) {
                 configFlashTimer--;
                 bool f = (configFlashTimer/100)%2;
                 LedOn(1, f);
             } else {
                 LedOn(1, tud_ready());
             }
             
             // LED 2: Speaker Activity
             LedOn(2, rb_count(&audioOutRB) > 0);
             
             // LED 3: Mic Activity
             LedOn(3, rb_count(&audioInRB) > 0);
             
             // LED 4: USB Ready
             LedOn(4, tud_ready());
             
             // LED 5: Sample Rate (On=48k)
             LedOn(5, g_sampleRateIdx == 0);
        }
    }
};

AudioCard card;

//--------------------------------------------------------------------+
// Core 1 Entry Point (Audio Processing)
//--------------------------------------------------------------------+
void core1_entry()
{
    // Initialize as victim for flash writes (Core 0 will lock us out)
    multicore_lockout_victim_init();
    
    card.Run();  // Never returns
}

//--------------------------------------------------------------------+
// TinyUSB Audio Callbacks (UAC1 - Full Speed for RP2040)
//--------------------------------------------------------------------+

// Set request for endpoint
bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff)
{
    (void)rhport;
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    
    if (ctrlSel == AUDIO10_EP_CTRL_SAMPLING_FREQ) {
        if (p_request->bRequest == AUDIO10_CS_REQ_SET_CUR) {
            TU_VERIFY(p_request->wLength == 3);
            current_sample_rate = tu_unaligned_read32(pBuff) & 0x00FFFFFF;
            return true;
        }
    }
    return false;
}

// Get request for endpoint
bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    
    if (ctrlSel == AUDIO10_EP_CTRL_SAMPLING_FREQ) {
        if (p_request->bRequest == AUDIO10_CS_REQ_GET_CUR) {
            uint8_t freq[3];
            freq[0] = (uint8_t)(current_sample_rate & 0xFF);
            freq[1] = (uint8_t)((current_sample_rate >> 8) & 0xFF);
            freq[2] = (uint8_t)((current_sample_rate >> 16) & 0xFF);
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, freq, sizeof(freq));
        }
    }
    return false;
}

// Get request for entity (feature unit)
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t entityID = TU_U16_HIGH(p_request->wIndex);
    
    if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT) {
        switch (ctrlSel) {
            case AUDIO10_FU_CTRL_MUTE:
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &mute[channelNum], 1);
            
            case AUDIO10_FU_CTRL_VOLUME: {
                if (p_request->bRequest == AUDIO10_CS_REQ_GET_CUR) {
                    int16_t vol = volume[channelNum] * 256;
                    return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &vol, sizeof(vol));
                } else if (p_request->bRequest == AUDIO10_CS_REQ_GET_MIN) {
                    int16_t min = -90 * 256;
                    return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &min, sizeof(min));
                } else if (p_request->bRequest == AUDIO10_CS_REQ_GET_MAX) {
                    int16_t max = 0;
                    return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &max, sizeof(max));
                } else if (p_request->bRequest == AUDIO10_CS_REQ_GET_RES) {
                    int16_t res = 256;
                    return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &res, sizeof(res));
                }
            }
            break;
        }
    }
    return false;
}

// Set request for entity (feature unit)
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff)
{
    (void)rhport;
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t entityID = TU_U16_HIGH(p_request->wIndex);
    
    if (entityID == UAC1_ENTITY_SPK_FEATURE_UNIT) {
        switch (ctrlSel) {
            case AUDIO10_FU_CTRL_MUTE:
                if (p_request->bRequest == AUDIO10_CS_REQ_SET_CUR) {
                    TU_VERIFY(p_request->wLength == 1);
                    mute[channelNum] = pBuff[0];
                    return true;
                }
                break;
            
            case AUDIO10_FU_CTRL_VOLUME:
                if (p_request->bRequest == AUDIO10_CS_REQ_SET_CUR) {
                    TU_VERIFY(p_request->wLength == 2);
                    volume[channelNum] = (int16_t)tu_unaligned_read16(pBuff) / 256;
                    return true;
                }
                break;
        }
    }
    return false;
}

// Interface callbacks
bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport; (void)p_request;
    return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));
    
    if (alt != 0) {
        // Streaming enabled
        if (itf == ITF_NUM_AUDIO_STREAMING_SPK) {
            // Speaker streaming starting - pre-fill buffer
            rb_clear(&audioOutRB);
            // Must be multiple of 3 (atomic sample size) to preserve channel alignment
            // Balanced buffer (200 samples * 3 words = ~12.5ms @ 48kHz)
            for (int i = 0; i < 600; i++) {
                rb_push(&audioOutRB, 0);
            }
        }
        else if (itf == ITF_NUM_AUDIO_STREAMING_MIC) {
            // Mic streaming starting - clear buffer
            rb_clear(&audioInRB);
        }
    }
    
    spk_data_size = 0;
    return true;
}

// TX done callback - just return true, actual TX is done in audio_task
bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting)
{
    (void)rhport; (void)itf; (void)ep_in; (void)cur_alt_setting;
    return true;
}

//--------------------------------------------------------------------+
// USB Audio Task (Core 0) - runs every 1ms
//--------------------------------------------------------------------+
static int16_t mic_buf[600];  // Buffer for 6-channel mic data to send

void audio_task(void)
{
    static uint32_t start_ms = 0;
    uint32_t curr_ms = board_millis();
    if (start_ms == curr_ms) return;  // Run once per ms
    start_ms = curr_ms;
    
    // Determine RX (Spk) and TX (Mic) channel counts based on Config
    uint8_t rx_channels = g_channelsOut; // Explicitly configured
    uint8_t tx_channels = g_channelsIn;  // Explicitly configured
    
    // Safety clamp to allowed values (2, 4, 6)
    if (rx_channels != 2 && rx_channels != 4 && rx_channels != 6) rx_channels = 6;
    if (tx_channels != 2 && tx_channels != 4 && tx_channels != 6) tx_channels = 6;
    
    uint8_t rx_bytes_per_sample = rx_channels * 2;
    uint8_t tx_bytes_per_sample = tx_channels * 2;
    
    // ===== SPEAKER RX (USB -> DAC) =====
    // Format: [Ch1 Ch2 ...] per sample, each 16-bit
    uint32_t bytes_available = tud_audio_available();
    
    // Fix: Only read full frames to avoid dropping partial bytes and shifting channels
    // If we read partials (e.g. 2 leftover bytes of a 4-byte frame), we lose them and desync.
    uint32_t aligned_bytes = (bytes_available / rx_bytes_per_sample) * rx_bytes_per_sample;
    
    if (aligned_bytes > 0) {
        uint32_t bytes_read = tud_audio_read(spk_buf, aligned_bytes);
        
        uint32_t samples_read = bytes_read / rx_bytes_per_sample;
        for (uint32_t i = 0; i < samples_read; i++) {
            // Pack channels 1-2 (always present)
            uint32_t val_12 = (uint16_t)spk_buf[rx_channels*i] | ((uint32_t)(uint16_t)spk_buf[rx_channels*i+1] << 16);
            rb_push(&audioOutRB, val_12);
            
            // Pack channels 3-4 (present for 4ch and 6ch)
            if (rx_channels >= 4) {
                uint32_t val_34 = (uint16_t)spk_buf[rx_channels*i+2] | ((uint32_t)(uint16_t)spk_buf[rx_channels*i+3] << 16);
                rb_push(&audioOutRB, val_34);
            } else {
                rb_push(&audioOutRB, 0); // Silence
            }
            
            // Pack channels 5-6 (present only for 6ch)
            if (rx_channels >= 6) {
                uint32_t val_56 = (uint16_t)spk_buf[rx_channels*i+4] | ((uint32_t)(uint16_t)spk_buf[rx_channels*i+5] << 16);
                rb_push(&audioOutRB, val_56);
            } else {
                rb_push(&audioOutRB, 0); // Silence
            }
        }
    }
    
    // ===== MIC TX (ADC -> USB) =====
    uint32_t count = rb_count(&audioInRB);
    
    // Adaptive Packet Size logic
    uint32_t samples_to_send = 48; // Default for 48k

    if (g_sampleRateIdx == 1) { // 44.1k
         static uint32_t phase_acc = 0;
         uint32_t target_mhz = 44100;
         if (count > 3000) target_mhz = 44150;       
         else if (count > 2200) target_mhz = 44105;  
         else if (count < 1000) target_mhz = 44000;  
         else if (count < 1900) target_mhz = 44095;
         phase_acc += target_mhz;
         samples_to_send = phase_acc / 1000;
         phase_acc %= 1000;
    } else if (g_sampleRateIdx == 2) { // 24k
         samples_to_send = 24;
         if (count > 3000) samples_to_send = 25;
         else if (count < 1000) samples_to_send = 23;
    } else { // 48k
         samples_to_send = 48;
         if (count > 3000) samples_to_send = 49;
         else if (count < 1000) samples_to_send = 47;
    }
    
    for (uint32_t i = 0; i < samples_to_send; i++) {
        uint32_t val_12, val_34, val_56;
        
        // Pop 3 words (Always 6ch internally)
        if (rb_count(&audioInRB) >= 3) {
            rb_pop(&audioInRB, &val_12);
            rb_pop(&audioInRB, &val_34);
            rb_pop(&audioInRB, &val_56);
            
            int16_t ch1 = (int16_t)(val_12 & 0xFFFF);
            int16_t ch2 = (int16_t)((val_12 >> 16) & 0xFFFF);
            int16_t ch3 = (int16_t)(val_34 & 0xFFFF);
            int16_t ch4 = (int16_t)((val_34 >> 16) & 0xFFFF);
            int16_t ch5 = (int16_t)(val_56 & 0xFFFF);
            int16_t ch6 = (int16_t)((val_56 >> 16) & 0xFFFF);
            
            // Pack TX buffer based on tx_channels
            mic_buf[tx_channels*i] = ch1;
            mic_buf[tx_channels*i+1] = ch2;
            if (tx_channels >= 4) {
                mic_buf[tx_channels*i+2] = ch3;
                mic_buf[tx_channels*i+3] = ch4;
            }
            if (tx_channels >= 6) {
                mic_buf[tx_channels*i+4] = ch5;
                mic_buf[tx_channels*i+5] = ch6;
            }
        } else {
            // Buffer underrun
            for(int j=0; j<tx_channels; j++) mic_buf[tx_channels*i+j] = 0;
        }
    }
    
    // Write using TX byte count
    tud_audio_write((uint8_t*)mic_buf, samples_to_send * tx_bytes_per_sample);
}

void midi_task(void)
{
    if (g_audioOnly) return;

    // 1. Process Connection (RX from USB)
    // Limit to 32 packets per task call to prevent audio dropouts during MIDI floods
    if (tud_midi_available()) {
        uint8_t packet[4];
        int packet_count = 0;
        while(tud_midi_packet_read(packet) && packet_count++ < 32) {
             
             uint8_t cin = packet[0] & 0x0F;
             
             // SysEx
             if (cin == 0x4) { // SysEx Start or Continue
                 if (sysExPtr < 127) {
                     sysExBuffer[sysExPtr++] = packet[1];
                     sysExBuffer[sysExPtr++] = packet[2];
                     sysExBuffer[sysExPtr++] = packet[3];
                 }
             }
             else if (cin >= 0x5 && cin <= 0x7) { // SysEx End
                  int count = (cin == 0x5) ? 1 : (cin == 0x6 ? 2 : 3);
                  for(int k=0; k<count; k++) {
                      if (sysExPtr < 127) sysExBuffer[sysExPtr++] = packet[1+k];
                  }
                  ProcessSysEx(sysExBuffer, sysExPtr);
                  sysExPtr = 0;
             }
             // Standard Channel Messages
             else {
                 uint8_t status = packet[1];
                 if (status >= 0x80) { // Valid Status
                     uint32_t evt = status | (packet[2] << 8) | (packet[3] << 16);
                     rb_push(&midiOutRB, evt);
                 }
             }
        }
    }

    // 2. Send Events from Core 1
    // Limit to 10 events per task call to balance load
    for(int i=0; i<10; i++) {
        uint32_t evt;
        if (rb_pop(&midiInRB, &evt)) {
            uint8_t msg[3];
            msg[0] = (evt) & 0xFF;        // Status (0xB0)
            msg[1] = (evt >> 8) & 0xFF;   // CC Number
            msg[2] = (evt >> 16) & 0xFF;  // Value
            tud_midi_stream_write(0, msg, 3);
        } else {
            break;
        }
    }
}

//--------------------------------------------------------------------+
// Main (Core 0)
//--------------------------------------------------------------------+
int main(void)
{
    // Check Debug Switch (Switch Down = Audio Only)
    // Must be done before board_init() might touch things, but after basic hardware init
    // Actually board_init() usually inits USB, so do this first.
    // However, RP2040 SDK inits basic hardware implicitly.
    // We'll trust the explicit inits in CheckDebugSwitch.
    
    // Set system clock to 200MHz
    set_sys_clock_khz(200000, true);
    
    // Check switch *after* sys clock for stable ADC?
    g_audioOnly = CheckDebugSwitch();
    
    // Initialize ring buffers
    rb_init(&audioInRB);
    rb_init(&audioOutRB);
    rb_init(&midiInRB);
    rb_init(&midiOutRB); // Init new buffer
    
    board_init();

    LoadConfigFromFlash();
    
    // Start Core 1 for audio processing
    multicore_launch_core1(core1_entry);
    
    // Initialize TinyUSB
    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);
    
    if (board_init_after_tusb) {
        board_init_after_tusb();
    }
    
    // Main loop
    while (1) {
        tud_task();
        audio_task(); // Service Audio
        midi_task();  // Service MIDI (now limited)
        audio_task(); // Service Audio AGAIN to ensure priority
    }
    
    return 0;
}