#include "AudioDSP.h"
#include "Config.h"
#include "driver/i2s.h"
#include <math.h>
#include <string.h>

// ---------------------------------------------------------
// GLOBAL DSP STATE
// ---------------------------------------------------------
volatile InputChannel_t  g_inputs[NUM_INPUTS];
volatile OutputChannel_t g_outputs[NUM_OUTPUTS];
volatile bool            g_dspParamsDirty = false;

// I2S port
#define I2S_PORT I2S_NUM_0

// DMA Buffers
static int32_t rx_buf[I2S_BUFFER_SIZE * 2];    // 2ch interleaved 32-bit
static int32_t tx_buf[I2S_BUFFER_SIZE * 4];    // 4ch interleaved 32-bit (TDM)

// Float processing buffers
static float in1_buf[I2S_BUFFER_SIZE];
static float in2_buf[I2S_BUFFER_SIZE];
static float out_buf[NUM_OUTPUTS][I2S_BUFFER_SIZE];

// VU meter accumulators (per block)
static float vu_peak_in[NUM_INPUTS];
static float vu_rms_in[NUM_INPUTS];
static float vu_peak_out[NUM_OUTPUTS];
static float vu_rms_out[NUM_OUTPUTS];

// ---------------------------------------------------------
// UTILITY FUNCTIONS
// ---------------------------------------------------------
float dBtoLinear(float dB) {
    return powf(10.0f, dB / 20.0f);
}

float linearToDB(float lin) {
    if (lin <= 0.0f) return -120.0f;
    return 20.0f * log10f(lin);
}

static inline float clampf(float val, float lo, float hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

// ---------------------------------------------------------
// BIQUAD FILTER: Direct Form II Transposed (single sample)
// ---------------------------------------------------------
float Biquad_Process(BiquadFilter_t* f, float input) {
    float output = f->b[0] * input + f->w[0];
    f->w[0] = f->b[1] * input - f->a[0] * output + f->w[1];
    f->w[1] = f->b[2] * input - f->a[1] * output;
    return output;
}

static void Biquad_Reset(BiquadFilter_t* f) {
    f->w[0] = 0.0f;
    f->w[1] = 0.0f;
}

// ---------------------------------------------------------
// BIQUAD COEFFICIENT GENERATORS (Audio EQ Cookbook, Robert Bristow-Johnson)
// ---------------------------------------------------------
void Biquad_CalcBell(BiquadFilter_t* f, float freq, float gainDb, float q, float sampleRate) {
    float A  = powf(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * M_PI * freq / sampleRate;
    float sinw0 = sinf(w0);
    float cosw0 = cosf(w0);
    float alpha = sinw0 / (2.0f * q);

    float b0 =  1.0f + alpha * A;
    float b1 = -2.0f * cosw0;
    float b2 =  1.0f - alpha * A;
    float a0 =  1.0f + alpha / A;
    float a1 = -2.0f * cosw0;
    float a2 =  1.0f - alpha / A;

    f->b[0] = b0 / a0;
    f->b[1] = b1 / a0;
    f->b[2] = b2 / a0;
    f->a[0] = a1 / a0;
    f->a[1] = a2 / a0;
    Biquad_Reset(f);
}

void Biquad_CalcLowShelf(BiquadFilter_t* f, float freq, float gainDb, float q, float sampleRate) {
    float A  = powf(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * M_PI * freq / sampleRate;
    float sinw0 = sinf(w0);
    float cosw0 = cosf(w0);
    float alpha = sinw0 / (2.0f * q);
    float sqrtA = sqrtf(A);

    float b0 =     A * ((A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha);
    float b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0);
    float b2 =     A * ((A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha);
    float a0 =           (A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha;
    float a1 =  -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0);
    float a2 =           (A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha;

    f->b[0] = b0 / a0;
    f->b[1] = b1 / a0;
    f->b[2] = b2 / a0;
    f->a[0] = a1 / a0;
    f->a[1] = a2 / a0;
    Biquad_Reset(f);
}

void Biquad_CalcHighShelf(BiquadFilter_t* f, float freq, float gainDb, float q, float sampleRate) {
    float A  = powf(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * M_PI * freq / sampleRate;
    float sinw0 = sinf(w0);
    float cosw0 = cosf(w0);
    float alpha = sinw0 / (2.0f * q);
    float sqrtA = sqrtf(A);

    float b0 =     A * ((A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha);
    float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0);
    float b2 =     A * ((A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha);
    float a0 =           (A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha;
    float a1 =   2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0);
    float a2 =           (A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha;

    f->b[0] = b0 / a0;
    f->b[1] = b1 / a0;
    f->b[2] = b2 / a0;
    f->a[0] = a1 / a0;
    f->a[1] = a2 / a0;
    Biquad_Reset(f);
}

void Biquad_CalcNotch(BiquadFilter_t* f, float freq, float q, float sampleRate) {
    float w0 = 2.0f * M_PI * freq / sampleRate;
    float sinw0 = sinf(w0);
    float cosw0 = cosf(w0);
    float alpha = sinw0 / (2.0f * q);

    float b0 =  1.0f;
    float b1 = -2.0f * cosw0;
    float b2 =  1.0f;
    float a0 =  1.0f + alpha;
    float a1 = -2.0f * cosw0;
    float a2 =  1.0f - alpha;

    f->b[0] = b0 / a0;
    f->b[1] = b1 / a0;
    f->b[2] = b2 / a0;
    f->a[0] = a1 / a0;
    f->a[1] = a2 / a0;
    Biquad_Reset(f);
}

void Biquad_CalcAllpass(BiquadFilter_t* f, float freq, float q, float sampleRate) {
    float w0 = 2.0f * M_PI * freq / sampleRate;
    float sinw0 = sinf(w0);
    float cosw0 = cosf(w0);
    float alpha = sinw0 / (2.0f * q);

    float b0 =  1.0f - alpha;
    float b1 = -2.0f * cosw0;
    float b2 =  1.0f + alpha;
    float a0 =  1.0f + alpha;
    float a1 = -2.0f * cosw0;
    float a2 =  1.0f - alpha;

    f->b[0] = b0 / a0;
    f->b[1] = b1 / a0;
    f->b[2] = b2 / a0;
    f->a[0] = a1 / a0;
    f->a[1] = a2 / a0;
    Biquad_Reset(f);
}

void Biquad_CalcBandpass(BiquadFilter_t* f, float freq, float q, float sampleRate) {
    float w0 = 2.0f * M_PI * freq / sampleRate;
    float sinw0 = sinf(w0);
    float cosw0 = cosf(w0);
    float alpha = sinw0 / (2.0f * q);

    float b0 =  alpha;
    float b1 =  0.0f;
    float b2 = -alpha;
    float a0 =  1.0f + alpha;
    float a1 = -2.0f * cosw0;
    float a2 =  1.0f - alpha;

    f->b[0] = b0 / a0;
    f->b[1] = b1 / a0;
    f->b[2] = b2 / a0;
    f->a[0] = a1 / a0;
    f->a[1] = a2 / a0;
    Biquad_Reset(f);
}

void Biquad_CalcLPF(BiquadFilter_t* f, float freq, float q, float sampleRate) {
    float w0 = 2.0f * M_PI * freq / sampleRate;
    float sinw0 = sinf(w0);
    float cosw0 = cosf(w0);
    float alpha = sinw0 / (2.0f * q);

    float b0 = (1.0f - cosw0) / 2.0f;
    float b1 =  1.0f - cosw0;
    float b2 = (1.0f - cosw0) / 2.0f;
    float a0 =  1.0f + alpha;
    float a1 = -2.0f * cosw0;
    float a2 =  1.0f - alpha;

    f->b[0] = b0 / a0;
    f->b[1] = b1 / a0;
    f->b[2] = b2 / a0;
    f->a[0] = a1 / a0;
    f->a[1] = a2 / a0;
    Biquad_Reset(f);
}

void Biquad_CalcHPF(BiquadFilter_t* f, float freq, float q, float sampleRate) {
    float w0 = 2.0f * M_PI * freq / sampleRate;
    float sinw0 = sinf(w0);
    float cosw0 = cosf(w0);
    float alpha = sinw0 / (2.0f * q);

    float b0 =  (1.0f + cosw0) / 2.0f;
    float b1 = -(1.0f + cosw0);
    float b2 =  (1.0f + cosw0) / 2.0f;
    float a0 =  1.0f + alpha;
    float a1 = -2.0f * cosw0;
    float a2 =  1.0f - alpha;

    f->b[0] = b0 / a0;
    f->b[1] = b1 / a0;
    f->b[2] = b2 / a0;
    f->a[0] = a1 / a0;
    f->a[1] = a2 / a0;
    Biquad_Reset(f);
}

// ---------------------------------------------------------
// CROSSOVER COEFFICIENT GENERATORS
// ---------------------------------------------------------

// Butterworth Q values for cascaded 2nd-order stages
static const float BW_Q_2ND[] = { 0.7071f };                           // 12dB
static const float BW_Q_3RD[] = { 1.0f, 0.5f };                        // 18dB (2nd + 1st)
static const float BW_Q_4TH[] = { 0.5412f, 1.3065f };                  // 24dB
static const float BW_Q_8TH[] = { 0.5098f, 0.6013f, 0.8999f, 2.5629f }; // 48dB

void Crossover_CalcButterworth(CrossoverFilter_t* xf, bool isHPF, float sampleRate) {
    int order = (int)xf->slope;
    void (*calcFunc)(BiquadFilter_t*, float, float, float);

    if (isHPF) calcFunc = Biquad_CalcHPF;
    else       calcFunc = Biquad_CalcLPF;

    switch (order) {
        case 1: // 6dB - single 1st order approximation via biquad
            xf->numStages = 1;
            calcFunc(&xf->stages[0], xf->freq, 0.5f, sampleRate);
            break;
        case 2: // 12dB
            xf->numStages = 1;
            calcFunc(&xf->stages[0], xf->freq, BW_Q_2ND[0], sampleRate);
            break;
        case 3: // 18dB (2nd order + 1st order)
            xf->numStages = 2;
            calcFunc(&xf->stages[0], xf->freq, BW_Q_3RD[0], sampleRate);
            calcFunc(&xf->stages[1], xf->freq, BW_Q_3RD[1], sampleRate);
            break;
        case 4: // 24dB
            xf->numStages = 2;
            calcFunc(&xf->stages[0], xf->freq, BW_Q_4TH[0], sampleRate);
            calcFunc(&xf->stages[1], xf->freq, BW_Q_4TH[1], sampleRate);
            break;
        case 8: // 48dB
            xf->numStages = 4;
            for (int i = 0; i < 4; i++) {
                calcFunc(&xf->stages[i], xf->freq, BW_Q_8TH[i], sampleRate);
            }
            break;
        default:
            xf->numStages = 1;
            calcFunc(&xf->stages[0], xf->freq, BW_Q_2ND[0], sampleRate);
            break;
    }
}

void Crossover_CalcLinkwitzRiley(CrossoverFilter_t* xf, bool isHPF, float sampleRate) {
    // Linkwitz-Riley = two cascaded Butterworth filters of half the order
    // LR12 = 2x BW6 (Q=0.5), LR24 = 2x BW12 (Q=0.7071), LR48 = 2x BW24
    void (*calcFunc)(BiquadFilter_t*, float, float, float);
    if (isHPF) calcFunc = Biquad_CalcHPF;
    else       calcFunc = Biquad_CalcLPF;

    int order = (int)xf->slope;

    switch (order) {
        case 2: // LR12 = 2x 1st-order Butterworth
            xf->numStages = 2;
            calcFunc(&xf->stages[0], xf->freq, 0.5f, sampleRate);
            calcFunc(&xf->stages[1], xf->freq, 0.5f, sampleRate);
            break;
        case 4: // LR24 = 2x 2nd-order Butterworth (Q=0.7071)
            xf->numStages = 2;
            calcFunc(&xf->stages[0], xf->freq, 0.7071f, sampleRate);
            calcFunc(&xf->stages[1], xf->freq, 0.7071f, sampleRate);
            break;
        case 8: // LR48 = 2x 4th-order Butterworth
            xf->numStages = 4;
            calcFunc(&xf->stages[0], xf->freq, BW_Q_4TH[0], sampleRate);
            calcFunc(&xf->stages[1], xf->freq, BW_Q_4TH[1], sampleRate);
            calcFunc(&xf->stages[2], xf->freq, BW_Q_4TH[0], sampleRate);
            calcFunc(&xf->stages[3], xf->freq, BW_Q_4TH[1], sampleRate);
            break;
        default: // Default to LR24
            xf->numStages = 2;
            calcFunc(&xf->stages[0], xf->freq, 0.7071f, sampleRate);
            calcFunc(&xf->stages[1], xf->freq, 0.7071f, sampleRate);
            break;
    }
}

void Crossover_CalcBessel(CrossoverFilter_t* xf, bool isHPF, float sampleRate) {
    // Bessel Q values (for maximally flat group delay)
    void (*calcFunc)(BiquadFilter_t*, float, float, float);
    if (isHPF) calcFunc = Biquad_CalcHPF;
    else       calcFunc = Biquad_CalcLPF;

    int order = (int)xf->slope;
    switch (order) {
        case 2: // 12dB Bessel
            xf->numStages = 1;
            calcFunc(&xf->stages[0], xf->freq, 0.5773f, sampleRate);
            break;
        case 4: // 24dB Bessel
            xf->numStages = 2;
            calcFunc(&xf->stages[0], xf->freq, 0.8055f, sampleRate);
            calcFunc(&xf->stages[1], xf->freq, 0.5219f, sampleRate);
            break;
        default:
            xf->numStages = 1;
            calcFunc(&xf->stages[0], xf->freq, 0.5773f, sampleRate);
            break;
    }
}

// ---------------------------------------------------------
// FIR FILTER ENGINE
// ---------------------------------------------------------
void FIR_Reset(FIRFilter_t* fir) {
    memset((void*)fir->delayLine, 0, sizeof(float) * FIR_MAX_TAPS);
    fir->delayIndex = 0;
}

void FIR_SetCoeffs(FIRFilter_t* fir, const float* coeffs, int numTaps) {
    if (numTaps > FIR_MAX_TAPS) numTaps = FIR_MAX_TAPS;
    fir->numTaps = numTaps;
    memcpy((void*)fir->coeffs, coeffs, sizeof(float) * numTaps);
    FIR_Reset(fir);
}

float FIR_Process(FIRFilter_t* fir, float input) {
    // Write input to the circular delay line
    fir->delayLine[fir->delayIndex] = input;

    float output = 0.0f;
    int idx = fir->delayIndex;

    // Convolution: sum of coeffs[k] * delayLine[n - k]
    for (int k = 0; k < fir->numTaps; k++) {
        output += fir->coeffs[k] * fir->delayLine[idx];
        idx--;
        if (idx < 0) idx = fir->numTaps - 1; // Wrap circular buffer
    }

    // Advance write pointer
    fir->delayIndex++;
    if (fir->delayIndex >= fir->numTaps) fir->delayIndex = 0;

    return output;
}

// ---------------------------------------------------------
// DELAY BUFFER
// ---------------------------------------------------------
static void Delay_Init(DelayBuffer_t* d) {
    d->bufferSize = DELAY_MAX_SAMPLES;
    // Allocate from PSRAM for large delay buffers
    d->buffer = (float*)ps_malloc(sizeof(float) * d->bufferSize);
    if (d->buffer) {
        memset(d->buffer, 0, sizeof(float) * d->bufferSize);
    }
    d->writeIndex = 0;
    d->delayMs = 0.0f;
    d->delaySamples = 0.0f;
}

float Delay_Process(DelayBuffer_t* d, float input) {
    if (!d->buffer || d->delaySamples < 1.0f) return input; // No delay or no buffer

    // Write current sample
    d->buffer[d->writeIndex] = input;

    // Calculate read position with fractional interpolation
    int delaySamplesInt = (int)d->delaySamples;
    float frac = d->delaySamples - (float)delaySamplesInt;

    int readIdx0 = d->writeIndex - delaySamplesInt;
    if (readIdx0 < 0) readIdx0 += d->bufferSize;

    int readIdx1 = readIdx0 - 1;
    if (readIdx1 < 0) readIdx1 += d->bufferSize;

    // Linear interpolation for sub-sample accuracy
    float output = d->buffer[readIdx0] * (1.0f - frac) + d->buffer[readIdx1] * frac;

    // Advance write pointer
    d->writeIndex++;
    if (d->writeIndex >= d->bufferSize) d->writeIndex = 0;

    return output;
}

static void Delay_SetMs(DelayBuffer_t* d, float ms) {
    d->delayMs = clampf(ms, 0.0f, DELAY_MAX_MS);
    d->delaySamples = d->delayMs * (float)SAMPLE_RATE / 1000.0f;
}

// ---------------------------------------------------------
// PEAK LIMITER (Feed-forward, log-domain envelope)
// ---------------------------------------------------------
static void Limiter_Init(PeakLimiter_t* l) {
    l->enabled = false;
    l->threshold = -2.0f;   // dBFS
    l->attackMs = 10.0f;
    l->releaseMs = 100.0f;
    l->envelope = 0.0f;
    l->gainReduction = 0.0f;
    // Calculate coefficients
    l->attackCoeff  = expf(-1.0f / (l->attackMs  * 0.001f * (float)SAMPLE_RATE));
    l->releaseCoeff = expf(-1.0f / (l->releaseMs * 0.001f * (float)SAMPLE_RATE));
}

float Limiter_Process(PeakLimiter_t* l, float input) {
    if (!l->enabled) return input;

    float absInput = fabsf(input);
    float threshLin = dBtoLinear(l->threshold);

    // Envelope follower (peak detector with attack/release)
    if (absInput > l->envelope) {
        l->envelope = l->attackCoeff * l->envelope + (1.0f - l->attackCoeff) * absInput;
    } else {
        l->envelope = l->releaseCoeff * l->envelope + (1.0f - l->releaseCoeff) * absInput;
    }

    // Compute gain reduction
    float gain = 1.0f;
    if (l->envelope > threshLin && l->envelope > 0.0f) {
        gain = threshLin / l->envelope;
    }
    l->gainReduction = linearToDB(gain);

    return input * gain;
}

// ---------------------------------------------------------
// COEFFICIENT RECALCULATION
// ---------------------------------------------------------
void AudioDSP_RecalcInputPEQ(int ch) {
    InputChannel_t* inp = (InputChannel_t*)&g_inputs[ch];
    inp->gainLinear = dBtoLinear(inp->gain);

    for (int b = 0; b < INPUT_PEQ_BANDS; b++) {
        PEQBand_t* band = &inp->peq[b];
        BiquadFilter_t* filt = &inp->peqFilters[b];
        if (!band->enabled) {
            // Bypass: set to unity pass-through
            filt->b[0] = 1.0f; filt->b[1] = 0.0f; filt->b[2] = 0.0f;
            filt->a[0] = 0.0f; filt->a[1] = 0.0f;
            continue;
        }
        switch (band->type) {
            case FILTER_BELL:       Biquad_CalcBell(filt, band->freq, band->gain, band->q, SAMPLE_RATE); break;
            case FILTER_LOW_SHELF:  Biquad_CalcLowShelf(filt, band->freq, band->gain, band->q, SAMPLE_RATE); break;
            case FILTER_HIGH_SHELF: Biquad_CalcHighShelf(filt, band->freq, band->gain, band->q, SAMPLE_RATE); break;
            case FILTER_NOTCH:      Biquad_CalcNotch(filt, band->freq, band->q, SAMPLE_RATE); break;
            case FILTER_ALLPASS:    Biquad_CalcAllpass(filt, band->freq, band->q, SAMPLE_RATE); break;
            case FILTER_BANDPASS:   Biquad_CalcBandpass(filt, band->freq, band->q, SAMPLE_RATE); break;
        }
    }
}

void AudioDSP_RecalcOutputPEQ(int ch) {
    OutputChannel_t* out = (OutputChannel_t*)&g_outputs[ch];
    out->gainLinear = dBtoLinear(out->gain);

    for (int b = 0; b < OUTPUT_PEQ_BANDS; b++) {
        PEQBand_t* band = &out->peq[b];
        BiquadFilter_t* filt = &out->peqFilters[b];
        if (!band->enabled) {
            filt->b[0] = 1.0f; filt->b[1] = 0.0f; filt->b[2] = 0.0f;
            filt->a[0] = 0.0f; filt->a[1] = 0.0f;
            continue;
        }
        switch (band->type) {
            case FILTER_BELL:       Biquad_CalcBell(filt, band->freq, band->gain, band->q, SAMPLE_RATE); break;
            case FILTER_LOW_SHELF:  Biquad_CalcLowShelf(filt, band->freq, band->gain, band->q, SAMPLE_RATE); break;
            case FILTER_HIGH_SHELF: Biquad_CalcHighShelf(filt, band->freq, band->gain, band->q, SAMPLE_RATE); break;
            case FILTER_NOTCH:      Biquad_CalcNotch(filt, band->freq, band->q, SAMPLE_RATE); break;
            case FILTER_ALLPASS:    Biquad_CalcAllpass(filt, band->freq, band->q, SAMPLE_RATE); break;
            case FILTER_BANDPASS:   Biquad_CalcBandpass(filt, band->freq, band->q, SAMPLE_RATE); break;
        }
    }
}

void AudioDSP_RecalcCrossover(int ch) {
    OutputChannel_t* out = (OutputChannel_t*)&g_outputs[ch];

    // HPF
    if (out->hpf.enabled) {
        switch (out->hpf.type) {
            case XOVER_BUTTERWORTH:   Crossover_CalcButterworth(&out->hpf, true, SAMPLE_RATE); break;
            case XOVER_LINKWITZ_RILEY: Crossover_CalcLinkwitzRiley(&out->hpf, true, SAMPLE_RATE); break;
            case XOVER_BESSEL:        Crossover_CalcBessel(&out->hpf, true, SAMPLE_RATE); break;
        }
    }

    // LPF
    if (out->lpf.enabled) {
        switch (out->lpf.type) {
            case XOVER_BUTTERWORTH:   Crossover_CalcButterworth(&out->lpf, false, SAMPLE_RATE); break;
            case XOVER_LINKWITZ_RILEY: Crossover_CalcLinkwitzRiley(&out->lpf, false, SAMPLE_RATE); break;
            case XOVER_BESSEL:        Crossover_CalcBessel(&out->lpf, false, SAMPLE_RATE); break;
        }
    }
}

void AudioDSP_RecalcLimiter(int ch) {
    OutputChannel_t* out = (OutputChannel_t*)&g_outputs[ch];
    PeakLimiter_t* l = (PeakLimiter_t*)&out->limiter;
    l->attackCoeff  = expf(-1.0f / (l->attackMs  * 0.001f * (float)SAMPLE_RATE));
    l->releaseCoeff = expf(-1.0f / (l->releaseMs * 0.001f * (float)SAMPLE_RATE));
}

void AudioDSP_RecalcAllCoeffs() {
    for (int i = 0; i < NUM_INPUTS; i++) {
        AudioDSP_RecalcInputPEQ(i);
    }
    for (int o = 0; o < NUM_OUTPUTS; o++) {
        AudioDSP_RecalcOutputPEQ(o);
        AudioDSP_RecalcCrossover(o);
        AudioDSP_RecalcLimiter(o);
    }
}

// ---------------------------------------------------------
// DSP DEFAULT PARAMETERS
// ---------------------------------------------------------
static void DSP_SetDefaults() {
    // Input channels
    for (int i = 0; i < NUM_INPUTS; i++) {
        InputChannel_t* inp = (InputChannel_t*)&g_inputs[i];
        inp->gain = 0.0f;
        inp->gainLinear = 1.0f;
        inp->mute = false;
        inp->phaseInvert = false;

        // PEQ defaults: all bands disabled
        for (int b = 0; b < INPUT_PEQ_BANDS; b++) {
            inp->peq[b].enabled = false;
            inp->peq[b].type = FILTER_BELL;
            inp->peq[b].freq = 100.0f * powf(4.0f, (float)b); // 100, 400, 1.6k, 6.4k, 25.6k
            inp->peq[b].gain = 0.0f;
            inp->peq[b].q = 1.0f;
        }

        // FIR defaults: disabled, unity passthrough
        inp->fir.enabled = false;
        inp->fir.numTaps = 1;
        memset((void*)inp->fir.coeffs, 0, sizeof(float) * FIR_MAX_TAPS);
        inp->fir.coeffs[0] = 1.0f; // Unity impulse
        FIR_Reset((FIRFilter_t*)&inp->fir);

        // VU
        inp->vu.peakLevel = 0.0f;
        inp->vu.rmsLevel = 0.0f;
        inp->vu.clip = false;
        inp->vu.peakDb = -120.0f;
        inp->vu.rmsDb = -120.0f;
    }

    // Output channels
    for (int o = 0; o < NUM_OUTPUTS; o++) {
        OutputChannel_t* out = (OutputChannel_t*)&g_outputs[o];
        out->gain = 0.0f;
        out->gainLinear = 1.0f;
        out->mute = false;
        out->phaseInvert = false;
        out->source = ROUTE_SUM;

        // HPF defaults
        out->hpf.enabled = false;
        out->hpf.freq = 20.0f;
        out->hpf.type = XOVER_LINKWITZ_RILEY;
        out->hpf.slope = SLOPE_24DB;
        out->hpf.numStages = 0;

        // LPF defaults
        out->lpf.enabled = false;
        out->lpf.freq = 20000.0f;
        out->lpf.type = XOVER_LINKWITZ_RILEY;
        out->lpf.slope = SLOPE_24DB;
        out->lpf.numStages = 0;

        // PEQ defaults
        for (int b = 0; b < OUTPUT_PEQ_BANDS; b++) {
            out->peq[b].enabled = false;
            out->peq[b].type = FILTER_BELL;
            out->peq[b].freq = 63.0f * powf(2.0f, (float)b); // 63, 125, 250, 500, 1k, 2k, 4k
            out->peq[b].gain = 0.0f;
            out->peq[b].q = 1.0f;
        }

        // FIR defaults
        out->fir.enabled = false;
        out->fir.numTaps = 1;
        memset((void*)out->fir.coeffs, 0, sizeof(float) * FIR_MAX_TAPS);
        out->fir.coeffs[0] = 1.0f;
        FIR_Reset((FIRFilter_t*)&out->fir);

        // Delay defaults
        Delay_Init((DelayBuffer_t*)&out->delay);

        // Limiter defaults
        Limiter_Init((PeakLimiter_t*)&out->limiter);

        // VU
        out->vu.peakLevel = 0.0f;
        out->vu.rmsLevel = 0.0f;
        out->vu.clip = false;
        out->vu.peakDb = -120.0f;
        out->vu.rmsDb = -120.0f;
    }
}

// ---------------------------------------------------------
// I2S INITIALIZATION
// ---------------------------------------------------------
static void I2S_Init() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = I2S_BUFFER_COUNT,
        .dma_buf_len = I2S_BUFFER_SIZE,
        .use_apll = true,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = PIN_I2S_BCK,
        .ws_io_num = PIN_I2S_LRCK,
        .data_out_num = PIN_I2S_DOUT,
        .data_in_num = PIN_I2S_DIN
    };

    ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pin_config));

    Serial.println("[DSP] I2S driver installed");
}

// ---------------------------------------------------------
// PUBLIC API: INIT
// ---------------------------------------------------------
void AudioDSP_Init() {
    Serial.println("[DSP] Initializing Audio DSP Engine...");

    // Set default parameters
    DSP_SetDefaults();

    // Initialize I2S
    I2S_Init();

    // Calculate all coefficients from defaults
    AudioDSP_RecalcAllCoeffs();

    Serial.println("[DSP] Audio DSP Engine Ready");
}

// ---------------------------------------------------------
// PUBLIC API: PROCESS (called in tight loop on Core 1)
// ---------------------------------------------------------
void AudioDSP_Process() {
    size_t bytes_read = 0;
    size_t bytes_written = 0;

    // --- 1. Read I2S DMA buffer (blocks until data available) ---
    esp_err_t err = i2s_read(I2S_PORT, rx_buf, sizeof(rx_buf), &bytes_read, portMAX_DELAY);
    if (err != ESP_OK || bytes_read == 0) return;

    int numSamples = bytes_read / (sizeof(int32_t) * 2); // Stereo frames read

    // --- 2. Convert int32 to float32 (normalize to -1.0 .. 1.0) ---
    for (int n = 0; n < numSamples; n++) {
        in1_buf[n] = (float)rx_buf[n * 2 + 0] / 2147483648.0f;     // Left = IN1
        in2_buf[n] = (float)rx_buf[n * 2 + 1] / 2147483648.0f;     // Right = IN2
    }

    // --- 3. INPUT PROCESSING: Gain -> PEQ -> FIR ---
    // Reset VU accumulators
    for (int i = 0; i < NUM_INPUTS; i++) {
        vu_peak_in[i] = 0.0f;
        vu_rms_in[i] = 0.0f;
    }

    float* in_bufs[NUM_INPUTS] = { in1_buf, in2_buf };

    for (int ch = 0; ch < NUM_INPUTS; ch++) {
        InputChannel_t* inp = (InputChannel_t*)&g_inputs[ch];
        float* buf = in_bufs[ch];

        for (int n = 0; n < numSamples; n++) {
            float sample = buf[n];

            // Apply input gain
            sample *= inp->gainLinear;

            // Apply phase invert
            if (inp->phaseInvert) sample = -sample;

            // Apply mute
            if (inp->mute) sample = 0.0f;

            // Apply 5-Band PEQ (cascaded biquads)
            for (int b = 0; b < INPUT_PEQ_BANDS; b++) {
                if (inp->peq[b].enabled) {
                    sample = Biquad_Process((BiquadFilter_t*)&inp->peqFilters[b], sample);
                }
            }

            // Apply FIR filter
            if (inp->fir.enabled && inp->fir.numTaps > 1) {
                sample = FIR_Process((FIRFilter_t*)&inp->fir, sample);
            }

            buf[n] = sample;

            // VU meter accumulation
            float absSample = fabsf(sample);
            if (absSample > vu_peak_in[ch]) vu_peak_in[ch] = absSample;
            vu_rms_in[ch] += sample * sample;
        }

        // Finalize VU
        inp->vu.peakLevel = vu_peak_in[ch];
        inp->vu.rmsLevel  = sqrtf(vu_rms_in[ch] / (float)numSamples);
        inp->vu.clip      = (vu_peak_in[ch] >= 0.99f);
        inp->vu.peakDb    = linearToDB(inp->vu.peakLevel);
        inp->vu.rmsDb     = linearToDB(inp->vu.rmsLevel);
    }

    // --- 4. OUTPUT PROCESSING: Route -> Delay -> Crossover -> PEQ -> FIR -> Phase -> Limiter -> Gain ---
    for (int i = 0; i < NUM_OUTPUTS; i++) {
        vu_peak_out[i] = 0.0f;
        vu_rms_out[i] = 0.0f;
    }

    for (int ch = 0; ch < NUM_OUTPUTS; ch++) {
        OutputChannel_t* out = (OutputChannel_t*)&g_outputs[ch];

        for (int n = 0; n < numSamples; n++) {
            float sample;

            // 4a. Matrix Routing / Source Select
            switch (out->source) {
                case ROUTE_IN1: sample = in1_buf[n]; break;
                case ROUTE_IN2: sample = in2_buf[n]; break;
                case ROUTE_SUM:
                default:        sample = (in1_buf[n] + in2_buf[n]) * 0.5f; break;
            }

            // 4b. Delay Buffer (Time Alignment)
            sample = Delay_Process((DelayBuffer_t*)&out->delay, sample);

            // 4c. Crossover HPF (cascaded biquad stages)
            if (out->hpf.enabled) {
                for (int s = 0; s < out->hpf.numStages; s++) {
                    sample = Biquad_Process((BiquadFilter_t*)&out->hpf.stages[s], sample);
                }
            }

            // 4d. Crossover LPF (cascaded biquad stages)
            if (out->lpf.enabled) {
                for (int s = 0; s < out->lpf.numStages; s++) {
                    sample = Biquad_Process((BiquadFilter_t*)&out->lpf.stages[s], sample);
                }
            }

            // 4e. 7-Band Parametric EQ (cascaded biquads)
            for (int b = 0; b < OUTPUT_PEQ_BANDS; b++) {
                if (out->peq[b].enabled) {
                    sample = Biquad_Process((BiquadFilter_t*)&out->peqFilters[b], sample);
                }
            }

            // 4f. FIR Filter
            if (out->fir.enabled && out->fir.numTaps > 1) {
                sample = FIR_Process((FIRFilter_t*)&out->fir, sample);
            }

            // 4g. Phase Invert
            if (out->phaseInvert) sample = -sample;

            // 4h. Peak Limiter
            sample = Limiter_Process((PeakLimiter_t*)&out->limiter, sample);

            // 4i. Output Gain & Mute
            if (out->mute) {
                sample = 0.0f;
            } else {
                sample *= out->gainLinear;
            }

            // Soft clamp to prevent DAC overflow
            sample = clampf(sample, -1.0f, 1.0f);

            out_buf[ch][n] = sample;

            // VU accumulation
            float absSample = fabsf(sample);
            if (absSample > vu_peak_out[ch]) vu_peak_out[ch] = absSample;
            vu_rms_out[ch] += sample * sample;
        }

        // Finalize output VU
        out->vu.peakLevel = vu_peak_out[ch];
        out->vu.rmsLevel  = sqrtf(vu_rms_out[ch] / (float)numSamples);
        out->vu.clip      = (vu_peak_out[ch] >= 0.99f);
        out->vu.peakDb    = linearToDB(out->vu.peakLevel);
        out->vu.rmsDb     = linearToDB(out->vu.rmsLevel);
    }

    // --- 5. Convert float32 back to int32 and interleave for output ---
    // Output: 2x PCM5102A (each stereo) = 4 channels total
    // DAC0 = OUT_A (L) + OUT_B (R), DAC1 = OUT_C (L) + OUT_D (R)
    // We write to a single I2S port using TDM or two separate I2S ports.
    // For single bus: interleave as [A, B, C, D] in TDM mode.
    // For simplicity here, we write first DAC pair (A+B) as stereo.
    for (int n = 0; n < numSamples; n++) {
        // DAC 0 (first PCM5102A): OUT A (left) + OUT B (right)
        tx_buf[n * 2 + 0] = (int32_t)(out_buf[0][n] * 2147483647.0f);
        tx_buf[n * 2 + 1] = (int32_t)(out_buf[1][n] * 2147483647.0f);
    }

    i2s_write(I2S_PORT, tx_buf, numSamples * sizeof(int32_t) * 2, &bytes_written, portMAX_DELAY);

    // For DAC 1 (second PCM5102A on a second I2S port or using another method):
    // This would require I2S_NUM_1 or TDM mode. The architecture here assumes
    // a second I2S write to I2S_NUM_1 for OUT_C + OUT_D.
    // TODO: Configure I2S_NUM_1 for the second DAC if hardware uses separate data lines.

    // --- 6. Check if parameter recalculation is needed ---
    if (g_dspParamsDirty) {
        AudioDSP_RecalcAllCoeffs();
        g_dspParamsDirty = false;
    }
}
