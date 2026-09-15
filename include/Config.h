#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ---------------------------------------------------------
// PIN MAPPING
// ---------------------------------------------------------

// I2S Audio Bus
#define PIN_I2S_BCK     4
#define PIN_I2S_LRCK    5
#define PIN_I2S_DIN     6
#define PIN_I2S_DOUT    7
#define PIN_I2S_SCK     15 // MCLK

// TFT LCD SPI (ST7735)
#define PIN_TFT_SCK     12
#define PIN_TFT_MOSI    11
#define PIN_TFT_CS      10
#define PIN_TFT_DC      9
#define PIN_TFT_RST     14

// I2C Bus (PCF8574 & NVS)
#define PIN_I2C_SDA     1
#define PIN_I2C_SCL     2

// Rotary Encoder (EC11)
#define PIN_ENC_CLK     16
#define PIN_ENC_DT      17
#define PIN_ENC_SW      18

// ---------------------------------------------------------
// AUDIO DSP CONSTANTS
// ---------------------------------------------------------
#define SAMPLE_RATE         96000
#define I2S_BUFFER_SIZE     128       // Samples per DMA buffer
#define I2S_BUFFER_COUNT    8         // Number of DMA buffers
#define NUM_INPUTS          2
#define NUM_OUTPUTS         4

// ---------------------------------------------------------
// DSP FILTER LIMITS
// ---------------------------------------------------------
#define INPUT_PEQ_BANDS     5         // 5-Band PEQ per input channel
#define OUTPUT_PEQ_BANDS    7         // 7-Band PEQ per output channel
#define BIQUAD_COEFFS       5         // b0, b1, b2, a1, a2
#define BIQUAD_STATE        2         // w[0], w[1] (Direct Form II Transposed)

// FIR Filter
#define FIR_MAX_TAPS        256       // Maximum FIR filter taps per channel
#define FIR_DEFAULT_TAPS    64        // Default FIR taps

// Delay Buffer
#define DELAY_MAX_MS        500.0f    // Maximum delay in milliseconds
#define DELAY_MAX_SAMPLES   ((int)(DELAY_MAX_MS * SAMPLE_RATE / 1000.0f)) // 48000 samples @ 96kHz

// Limiter
#define LIMITER_ATTACK_MIN  0.1f      // ms
#define LIMITER_ATTACK_MAX  100.0f    // ms
#define LIMITER_RELEASE_MIN 10.0f     // ms
#define LIMITER_RELEASE_MAX 2000.0f   // ms

// VU Meter
#define VU_UPDATE_RATE_MS   30        // VU meter update rate

// ---------------------------------------------------------
// PCF8574 EXPANDER BUTTONS
// ---------------------------------------------------------
#define PCF8574_ADDR        0x20      // Default I2C address
#define BTN_IN1             0
#define BTN_IN2             1
#define BTN_OUT_A           2
#define BTN_OUT_B           3
#define BTN_OUT_C           4
#define BTN_OUT_D           5

// ---------------------------------------------------------
// PRESET SYSTEM
// ---------------------------------------------------------
#define MAX_PRESETS          20
#define PRESET_NAME_LEN     32

// ---------------------------------------------------------
// SYSTEM
// ---------------------------------------------------------
#define WIFI_SSID           "DLMS_2.4_Config"
#define WIFI_PASS           "12345678"
#define HOSTNAME            "dlms24"

// ---------------------------------------------------------
// SECURITY & AUTHENTICATION
// ---------------------------------------------------------
#define AUTH_DEFAULT_PASS    "admin123"       // Default admin password
#define AUTH_TOKEN_LEN       32               // Session token length (hex string)
#define AUTH_SESSION_TIMEOUT 3600000          // Session timeout: 1 hour (ms)
#define AUTH_MAX_SESSIONS    4                // Max concurrent sessions
#define AUTH_LONG_PRESS_MS   10000            // 10 second long-press to reset password

#endif // CONFIG_H
