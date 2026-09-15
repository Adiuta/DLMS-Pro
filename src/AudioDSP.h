#ifndef AUDIO_DSP_H
#define AUDIO_DSP_H

#include <Arduino.h>
#include "Config.h"

// ---------------------------------------------------------
// DSP DATA STRUCTURES
// ---------------------------------------------------------

// Biquad filter coefficient set (Direct Form II Transposed)
typedef struct {
    float b[3];         // b0, b1, b2 (feedforward)
    float a[2];         // a1, a2 (feedback, negated convention)
    float w[2];         // State memory w[n-1], w[n-2]
} BiquadFilter_t;

// PEQ Band Parameters (user-facing)
typedef enum {
    FILTER_BELL = 0,
    FILTER_LOW_SHELF,
    FILTER_HIGH_SHELF,
    FILTER_NOTCH,
    FILTER_ALLPASS,
    FILTER_BANDPASS
} FilterType_t;

typedef struct {
    bool     enabled;
    FilterType_t type;
    float    freq;      // Center / corner frequency (Hz)
    float    gain;      // Gain in dB (for Bell/Shelf)
    float    q;         // Q factor (bandwidth)
} PEQBand_t;

// Crossover filter type
typedef enum {
    XOVER_BUTTERWORTH = 0,
    XOVER_BESSEL,
    XOVER_LINKWITZ_RILEY
} CrossoverType_t;

// Crossover slope
typedef enum {
    SLOPE_6DB  = 1,     // 1st order
    SLOPE_12DB = 2,     // 2nd order
    SLOPE_18DB = 3,     // 3rd order
    SLOPE_24DB = 4,     // 4th order
    SLOPE_48DB = 8      // 8th order (LR48)
} CrossoverSlope_t;

// Crossover filter (HPF or LPF)
typedef struct {
    bool           enabled;
    float          freq;       // Cutoff frequency (Hz)
    CrossoverType_t type;
    CrossoverSlope_t slope;
    BiquadFilter_t  stages[4]; // Up to 4 cascaded biquads (for 48dB/oct = 8th order)
    int             numStages; // Active number of biquad stages
} CrossoverFilter_t;

// Matrix routing source
typedef enum {
    ROUTE_IN1 = 0,
    ROUTE_IN2,
    ROUTE_SUM        // IN1 + IN2
} RouteSource_t;

// FIR filter structure
typedef struct {
    bool    enabled;
    int     numTaps;                 // Active number of taps
    float   coeffs[FIR_MAX_TAPS];   // FIR coefficients
    float   delayLine[FIR_MAX_TAPS]; // Circular buffer for FIR delay line
    int     delayIndex;              // Current write position in circular buffer
} FIRFilter_t;

// Delay buffer structure
typedef struct {
    float   delaySamples;            // Delay in fractional samples
    float   delayMs;                 // Delay in milliseconds (user-facing)
    float*  buffer;                  // Delay ring buffer (PSRAM allocated)
    int     bufferSize;              // Total buffer size in samples
    int     writeIndex;              // Ring buffer write position
} DelayBuffer_t;

// Peak Limiter structure (feed-forward, log-domain)
typedef struct {
    bool    enabled;
    float   threshold;   // Threshold in dBFS
    float   attackMs;    // Attack time in ms
    float   releaseMs;   // Release time in ms
    // Internal state
    float   attackCoeff;
    float   releaseCoeff;
    float   envelope;    // Current envelope level (linear)
    float   gainReduction; // Current gain reduction in dB
} PeakLimiter_t;

// VU Meter data (thread-safe read from Core 0)
typedef struct {
    float   peakLevel;   // Peak level (0.0 .. 1.0)
    float   rmsLevel;    // RMS level (0.0 .. 1.0)
    bool    clip;        // Clip indicator
    float   peakDb;      // Peak in dBFS
    float   rmsDb;       // RMS in dBFS
} VUMeter_t;

// ---------------------------------------------------------
// INPUT CHANNEL
// ---------------------------------------------------------
typedef struct {
    float       gain;                       // Input gain in dB
    float       gainLinear;                 // Precomputed linear gain
    bool        mute;
    bool        phaseInvert;                // 0° or 180°
    PEQBand_t   peq[INPUT_PEQ_BANDS];      // 5-Band Parametric EQ
    BiquadFilter_t peqFilters[INPUT_PEQ_BANDS]; // Biquad instances for PEQ
    FIRFilter_t fir;                        // FIR filter for input channel
    VUMeter_t   vu;                         // VU meter data
} InputChannel_t;

// ---------------------------------------------------------
// OUTPUT CHANNEL
// ---------------------------------------------------------
typedef struct {
    float       gain;                       // Output gain in dB
    float       gainLinear;                 // Precomputed linear gain
    bool        mute;
    bool        phaseInvert;                // 0° or 180°
    RouteSource_t source;                   // Routing source

    CrossoverFilter_t hpf;                  // High-Pass crossover
    CrossoverFilter_t lpf;                  // Low-Pass crossover

    PEQBand_t   peq[OUTPUT_PEQ_BANDS];      // 7-Band Parametric EQ
    BiquadFilter_t peqFilters[OUTPUT_PEQ_BANDS]; // Biquad instances for PEQ

    FIRFilter_t fir;                        // FIR filter for output channel

    DelayBuffer_t delay;                    // Time alignment delay
    PeakLimiter_t limiter;                  // Peak limiter / dynamics

    VUMeter_t   vu;                         // VU meter data
} OutputChannel_t;

// ---------------------------------------------------------
// GLOBAL DSP STATE (shared between cores with volatile)
// ---------------------------------------------------------
extern volatile InputChannel_t  g_inputs[NUM_INPUTS];
extern volatile OutputChannel_t g_outputs[NUM_OUTPUTS];
extern volatile bool            g_dspParamsDirty;  // Flag to recalculate coefficients

// ---------------------------------------------------------
// PUBLIC API
// ---------------------------------------------------------

// Lifecycle
void AudioDSP_Init();
void AudioDSP_Process();

// Coefficient recalculation (called when parameters change)
void AudioDSP_RecalcInputPEQ(int ch);
void AudioDSP_RecalcOutputPEQ(int ch);
void AudioDSP_RecalcCrossover(int ch);
void AudioDSP_RecalcLimiter(int ch);
void AudioDSP_RecalcAllCoeffs();

// Biquad coefficient generators
void Biquad_CalcBell(BiquadFilter_t* f, float freq, float gainDb, float q, float sampleRate);
void Biquad_CalcLowShelf(BiquadFilter_t* f, float freq, float gainDb, float q, float sampleRate);
void Biquad_CalcHighShelf(BiquadFilter_t* f, float freq, float gainDb, float q, float sampleRate);
void Biquad_CalcNotch(BiquadFilter_t* f, float freq, float q, float sampleRate);
void Biquad_CalcAllpass(BiquadFilter_t* f, float freq, float q, float sampleRate);
void Biquad_CalcBandpass(BiquadFilter_t* f, float freq, float q, float sampleRate);
void Biquad_CalcLPF(BiquadFilter_t* f, float freq, float q, float sampleRate);
void Biquad_CalcHPF(BiquadFilter_t* f, float freq, float q, float sampleRate);

// Crossover coefficient generators
void Crossover_CalcButterworth(CrossoverFilter_t* xf, bool isHPF, float sampleRate);
void Crossover_CalcLinkwitzRiley(CrossoverFilter_t* xf, bool isHPF, float sampleRate);
void Crossover_CalcBessel(CrossoverFilter_t* xf, bool isHPF, float sampleRate);

// FIR filter operations
void FIR_Reset(FIRFilter_t* fir);
void FIR_SetCoeffs(FIRFilter_t* fir, const float* coeffs, int numTaps);
float FIR_Process(FIRFilter_t* fir, float input);

// Inline DSP processing primitives
float Biquad_Process(BiquadFilter_t* f, float input);
float Delay_Process(DelayBuffer_t* d, float input);
float Limiter_Process(PeakLimiter_t* l, float input);

// Utility
float dBtoLinear(float dB);
float linearToDB(float lin);

#endif // AUDIO_DSP_H
