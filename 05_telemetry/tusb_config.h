#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

// Mode 0 is Device mode for the RP2040
#define CFG_TUSB_RHPORT0_MODE     OPT_MODE_DEVICE

#ifndef CFG_TUSB_OS
  #define CFG_TUSB_OS OPT_OS_NONE
#endif

// Device stack configuration
#define CFG_TUD_ENDPOINT0_SIZE    64

// --- Class Driver Enable ---
// Enable CDC (Serial Port) for Web Serial API. Disable everything else.
#define CFG_TUD_CDC               1
#define CFG_TUD_MSC               0
#define CFG_TUD_HID               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            0

// --- CDC Configuration ---
// Endpoint sizes. 64 is standard for Full Speed USB 2.0
#define CFG_TUD_CDC_EP_BUFSIZE    64

// FIFO Buffer Sizes
// Increased from 64 to 512 to handle the high-speed telemetry packets
// without stalling Core 1 if the PC host operating system lags slightly.
#define CFG_TUD_CDC_RX_BUFSIZE    256
#define CFG_TUD_CDC_TX_BUFSIZE    1024 

#ifdef __cplusplus
 }
#endif

#endif