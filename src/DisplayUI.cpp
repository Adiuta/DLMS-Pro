#include "DisplayUI.h"
#include "AudioDSP.h"
#include "Config.h"
#include <TFT_eSPI.h>

// Local helper (same as AudioDSP, defined static there)
static inline float clampf(float val, float lo, float hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

// ---------------------------------------------------------
// COLOUR PALETTE (Professional Dark Mode)
// ---------------------------------------------------------
#define COL_BG          0x0000  // Black
#define COL_PANEL       0x18E3  // Dark grey (24,28,24)
#define COL_ACCENT      0x067F  // Cyan accent (#00D2FF approx)
#define COL_TEXT         0xFFFF  // White
#define COL_TEXT_DIM     0xB5B6  // Light grey
#define COL_VU_GREEN     0x07E0  // Green
#define COL_VU_YELLOW    0xFFE0  // Yellow
#define COL_VU_RED       0xF800  // Red
#define COL_MUTE_RED     0xF800
#define COL_SELECTED     0x067F
#define COL_BORDER       0x4208  // Dark border

// ---------------------------------------------------------
// DISPLAY GEOMETRY
// ---------------------------------------------------------
// ST7735: 160x128 in landscape (rotation=1)
#define SCR_W           160
#define SCR_H           128

// VU meter bar dimensions
#define VU_BAR_W        8
#define VU_BAR_H        60
#define VU_BAR_GAP      4
#define VU_INPUT_Y      30
#define VU_OUTPUT_Y     30

// ---------------------------------------------------------
// GLOBALS
// ---------------------------------------------------------
TFT_eSPI tft = TFT_eSPI();
UIState_t g_uiState;

// Menu items
static const char* mainMenuItems[] = {
    "1. INPUT CONFIG",
    "2. OUTPUT CONFIG",
    "3. PRESETS",
    "4. NETWORK & WIFI",
    "5. SYSTEM"
};
#define MAIN_MENU_COUNT 5

// Channel labels
static const char* inputLabels[]  = { "IN1", "IN2" };
static const char* outputLabels[] = { "OUT A", "OUT B", "OUT C", "OUT D" };

// Active preset name
static char presetName[PRESET_NAME_LEN] = "[P01] Default";

// ---------------------------------------------------------
// HELPER: Draw a single VU meter bar
// ---------------------------------------------------------
static void drawVUBar(int x, int y, int w, int h, float level, bool clip) {
    int filledH = (int)(level * h);
    if (filledH > h) filledH = h;
    if (filledH < 0) filledH = 0;

    // Background (empty part)
    tft.fillRect(x, y, w, h - filledH, COL_BG);

    // Filled part with gradient zones
    int greenZone  = (int)(h * 0.6f);
    int yellowZone = (int)(h * 0.85f);

    for (int py = h - filledH; py < h; py++) {
        int fromBottom = h - py;
        uint16_t color;
        if (fromBottom <= greenZone)       color = COL_VU_GREEN;
        else if (fromBottom <= yellowZone) color = COL_VU_YELLOW;
        else                               color = COL_VU_RED;
        tft.drawFastHLine(x, y + py, w, color);
    }

    // Clip indicator dot
    if (clip) {
        tft.fillCircle(x + w / 2, y - 4, 2, COL_VU_RED);
    } else {
        tft.fillCircle(x + w / 2, y - 4, 2, COL_BG);
    }
}

// ---------------------------------------------------------
// HELPER: Draw horizontal separator
// ---------------------------------------------------------
static void drawSeparator(int y) {
    tft.drawFastHLine(0, y, SCR_W, COL_BORDER);
}

// ---------------------------------------------------------
// HELPER: Draw header bar
// ---------------------------------------------------------
static void drawHeader(const char* title) {
    tft.fillRect(0, 0, SCR_W, 16, COL_PANEL);
    tft.setTextColor(COL_ACCENT, COL_PANEL);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(title, 4, 8, 1);
    drawSeparator(16);
}

// ---------------------------------------------------------
// HELPER: Draw menu list
// ---------------------------------------------------------
static void drawMenuList(const char** items, int count, int selected, int yStart) {
    for (int i = 0; i < count; i++) {
        int y = yStart + i * 16;
        if (y + 16 > SCR_H) break; // Don't draw beyond screen

        if (i == selected) {
            tft.fillRect(0, y, SCR_W, 16, COL_ACCENT);
            tft.setTextColor(COL_BG, COL_ACCENT);
        } else {
            tft.fillRect(0, y, SCR_W, 16, COL_BG);
            tft.setTextColor(COL_TEXT_DIM, COL_BG);
        }
        tft.setTextDatum(ML_DATUM);
        tft.drawString(items[i], 8, y + 8, 1);
    }
}

// ---------------------------------------------------------
// SCREEN: HOME (Default)
// ---------------------------------------------------------
void DisplayUI_DrawHome() {
    tft.fillScreen(COL_BG);

    // Header: Preset Name
    tft.fillRect(0, 0, SCR_W, 14, COL_PANEL);
    tft.setTextColor(COL_ACCENT, COL_PANEL);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(presetName, SCR_W / 2, 7, 1);
    drawSeparator(14);

    // Labels for inputs
    tft.setTextColor(COL_TEXT_DIM, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("IN1", 16, 22, 1);
    tft.drawString("IN2", 36, 22, 1);

    // Labels for outputs
    tft.drawString("A", 70, 22, 1);
    tft.drawString("B", 90, 22, 1);
    tft.drawString("C", 110, 22, 1);
    tft.drawString("D", 130, 22, 1);

    // Separator between inputs and outputs
    tft.drawFastVLine(52, 18, VU_BAR_H + 20, COL_BORDER);

    // Draw VU bars (initial)
    // Input VU bars
    drawVUBar(12, VU_INPUT_Y, VU_BAR_W, VU_BAR_H,
              g_inputs[0].vu.peakLevel, g_inputs[0].vu.clip);
    drawVUBar(32, VU_INPUT_Y, VU_BAR_W, VU_BAR_H,
              g_inputs[1].vu.peakLevel, g_inputs[1].vu.clip);

    // Output VU bars
    for (int o = 0; o < NUM_OUTPUTS; o++) {
        drawVUBar(66 + o * 20, VU_OUTPUT_Y, VU_BAR_W, VU_BAR_H,
                  g_outputs[o].vu.peakLevel, g_outputs[o].vu.clip);
    }

    // dB readout under VU bars
    int dbY = VU_INPUT_Y + VU_BAR_H + 4;
    tft.setTextColor(COL_TEXT_DIM, COL_BG);
    tft.setTextDatum(MC_DATUM);

    char dbStr[8];
    snprintf(dbStr, sizeof(dbStr), "%.0f", g_inputs[0].vu.peakDb);
    tft.drawString(dbStr, 16, dbY, 1);
    snprintf(dbStr, sizeof(dbStr), "%.0f", g_inputs[1].vu.peakDb);
    tft.drawString(dbStr, 36, dbY, 1);

    for (int o = 0; o < NUM_OUTPUTS; o++) {
        snprintf(dbStr, sizeof(dbStr), "%.0f", g_outputs[o].vu.peakDb);
        tft.drawString(dbStr, 66 + o * 20, dbY, 1);
    }

    // Mute indicators
    int muteY = dbY + 12;
    for (int i = 0; i < NUM_INPUTS; i++) {
        if (g_inputs[i].mute) {
            tft.fillRect(8 + i * 20, muteY, 16, 8, COL_MUTE_RED);
            tft.setTextColor(COL_TEXT, COL_MUTE_RED);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("M", 16 + i * 20, muteY + 4, 1);
        }
    }
    for (int o = 0; o < NUM_OUTPUTS; o++) {
        if (g_outputs[o].mute) {
            tft.fillRect(62 + o * 20, muteY, 16, 8, COL_MUTE_RED);
            tft.setTextColor(COL_TEXT, COL_MUTE_RED);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("M", 70 + o * 20, muteY + 4, 1);
        }
    }

    // Bottom status bar: WiFi
    int statusY = SCR_H - 12;
    drawSeparator(statusY - 2);
    tft.fillRect(0, statusY - 1, SCR_W, 13, COL_PANEL);
    tft.setTextColor(COL_TEXT_DIM, COL_PANEL);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("WiFi: " WIFI_SSID, 4, statusY + 5, 1);

    tft.setTextDatum(MR_DATUM);
    tft.drawString("192.168.4.1", SCR_W - 4, statusY + 5, 1);
}

// ---------------------------------------------------------
// SCREEN: MAIN MENU
// ---------------------------------------------------------
void DisplayUI_DrawMainMenu() {
    tft.fillScreen(COL_BG);
    drawHeader("MAIN MENU");
    drawMenuList(mainMenuItems, MAIN_MENU_COUNT, g_uiState.menuIndex, 20);
}

// ---------------------------------------------------------
// SCREEN: INPUT CHANNEL DETAIL
// ---------------------------------------------------------
void DisplayUI_DrawInputChannel(int ch) {
    InputChannel_t* inp = (InputChannel_t*)&g_inputs[ch];
    tft.fillScreen(COL_BG);

    char title[24];
    snprintf(title, sizeof(title), "INPUT %d", ch + 1);
    drawHeader(title);

    int y = 20;
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.setTextDatum(ML_DATUM);

    // Gain
    char line[40];
    snprintf(line, sizeof(line), "Gain:  %.1f dB", inp->gain);
    tft.drawString(line, 4, y, 1); y += 14;

    // Phase
    snprintf(line, sizeof(line), "Phase: %s", inp->phaseInvert ? "180" : "0  ");
    tft.drawString(line, 4, y, 1); y += 14;

    // Mute
    snprintf(line, sizeof(line), "Mute:  %s", inp->mute ? "ON " : "OFF");
    tft.setTextColor(inp->mute ? COL_VU_RED : COL_VU_GREEN, COL_BG);
    tft.drawString(line, 4, y, 1); y += 14;
    tft.setTextColor(COL_TEXT, COL_BG);

    drawSeparator(y); y += 4;

    // PEQ summary (mini bar graph)
    tft.setTextColor(COL_ACCENT, COL_BG);
    tft.drawString("PEQ (5-Band)", 4, y, 1); y += 12;
    for (int b = 0; b < INPUT_PEQ_BANDS; b++) {
        PEQBand_t* band = (PEQBand_t*)&inp->peq[b];
        uint16_t col = band->enabled ? COL_VU_GREEN : COL_TEXT_DIM;
        snprintf(line, sizeof(line), "%d:%.0fHz %+.1fdB", b + 1, band->freq, band->gain);
        tft.setTextColor(col, COL_BG);
        tft.drawString(line, 8, y, 1); y += 10;
    }

    // FIR status
    y += 2;
    drawSeparator(y); y += 4;
    tft.setTextColor(COL_ACCENT, COL_BG);
    snprintf(line, sizeof(line), "FIR: %s (%d taps)",
             inp->fir.enabled ? "ON" : "OFF", inp->fir.numTaps);
    tft.drawString(line, 4, y, 1);
}

// ---------------------------------------------------------
// SCREEN: OUTPUT CHANNEL DETAIL
// ---------------------------------------------------------
void DisplayUI_DrawOutputChannel(int ch) {
    OutputChannel_t* out = (OutputChannel_t*)&g_outputs[ch];
    tft.fillScreen(COL_BG);

    char title[24];
    snprintf(title, sizeof(title), "OUTPUT %c", 'A' + ch);
    drawHeader(title);

    int y = 20;
    char line[48];

    tft.setTextColor(COL_TEXT, COL_BG);
    tft.setTextDatum(ML_DATUM);

    // Source
    const char* srcNames[] = {"IN1", "IN2", "SUM"};
    snprintf(line, sizeof(line), "Source: %s", srcNames[out->source]);
    tft.drawString(line, 4, y, 1); y += 12;

    // Gain
    snprintf(line, sizeof(line), "Gain: %.1f dB", out->gain);
    tft.drawString(line, 4, y, 1); y += 12;

    // Mute
    tft.setTextColor(out->mute ? COL_VU_RED : COL_VU_GREEN, COL_BG);
    snprintf(line, sizeof(line), "Mute: %s", out->mute ? "ON" : "OFF");
    tft.drawString(line, 4, y, 1); y += 12;
    tft.setTextColor(COL_TEXT, COL_BG);

    drawSeparator(y); y += 3;

    // Crossover HPF
    tft.setTextColor(COL_ACCENT, COL_BG);
    tft.drawString("CROSSOVER", 4, y, 1); y += 10;
    tft.setTextColor(COL_TEXT, COL_BG);
    snprintf(line, sizeof(line), "HPF: %.1f Hz %s",
             out->hpf.freq, out->hpf.enabled ? "ON " : "OFF");
    tft.drawString(line, 8, y, 1); y += 10;

    snprintf(line, sizeof(line), "LPF: %.1f Hz %s",
             out->lpf.freq, out->lpf.enabled ? "ON " : "OFF");
    tft.drawString(line, 8, y, 1); y += 12;

    drawSeparator(y); y += 3;

    // Delay
    snprintf(line, sizeof(line), "Delay: %.1f ms", out->delay.delayMs);
    tft.drawString(line, 4, y, 1); y += 12;

    // Phase
    snprintf(line, sizeof(line), "Phase: %s", out->phaseInvert ? "180" : "0");
    tft.drawString(line, 4, y, 1); y += 12;

    // Limiter
    tft.setTextColor(out->limiter.enabled ? COL_VU_YELLOW : COL_TEXT_DIM, COL_BG);
    snprintf(line, sizeof(line), "Lim: %s Th:%.1f GR:%.1f",
             out->limiter.enabled ? "ON " : "OFF",
             out->limiter.threshold, out->limiter.gainReduction);
    tft.drawString(line, 4, y, 1); y += 12;

    // FIR status
    tft.setTextColor(COL_ACCENT, COL_BG);
    snprintf(line, sizeof(line), "FIR: %s (%d taps)",
             out->fir.enabled ? "ON" : "OFF", out->fir.numTaps);
    tft.drawString(line, 4, y, 1);
}

// ---------------------------------------------------------
// SCREEN: PEQ BAND EDITOR
// ---------------------------------------------------------
void DisplayUI_DrawPEQEdit(bool isOutput, int ch, int band) {
    tft.fillScreen(COL_BG);

    PEQBand_t* b;
    if (isOutput) {
        b = (PEQBand_t*)&g_outputs[ch].peq[band];
        char title[24];
        snprintf(title, sizeof(title), "OUT%c PEQ Band %d", 'A' + ch, band + 1);
        drawHeader(title);
    } else {
        b = (PEQBand_t*)&g_inputs[ch].peq[band];
        char title[24];
        snprintf(title, sizeof(title), "IN%d PEQ Band %d", ch + 1, band + 1);
        drawHeader(title);
    }

    int y = 22;
    char line[40];
    const char* typeNames[] = {"Bell", "LowShelf", "HiShelf", "Notch", "AllPass", "BandPass"};

    tft.setTextColor(COL_TEXT, COL_BG);
    tft.setTextDatum(ML_DATUM);

    snprintf(line, sizeof(line), "Enable: %s", b->enabled ? "ON" : "OFF");
    tft.setTextColor(b->enabled ? COL_VU_GREEN : COL_VU_RED, COL_BG);
    tft.drawString(line, 4, y, 1); y += 14;
    tft.setTextColor(COL_TEXT, COL_BG);

    snprintf(line, sizeof(line), "Type:   %s", typeNames[b->type]);
    tft.drawString(line, 4, y, 1); y += 14;

    snprintf(line, sizeof(line), "Freq:   %.1f Hz", b->freq);
    tft.drawString(line, 4, y, 1); y += 14;

    snprintf(line, sizeof(line), "Gain:   %+.1f dB", b->gain);
    tft.drawString(line, 4, y, 1); y += 14;

    snprintf(line, sizeof(line), "Q:      %.2f", b->q);
    tft.drawString(line, 4, y, 1); y += 14;

    // Mini frequency response curve hint
    drawSeparator(y + 2);
    y += 6;
    tft.setTextColor(COL_TEXT_DIM, COL_BG);
    tft.drawString("Rotate to change, Press to select", 4, y, 1);
}

// ---------------------------------------------------------
// SCREEN: FIR VIEW
// ---------------------------------------------------------
void DisplayUI_DrawFIRView(bool isOutput, int ch) {
    tft.fillScreen(COL_BG);

    FIRFilter_t* fir;
    char title[24];
    if (isOutput) {
        fir = (FIRFilter_t*)&g_outputs[ch].fir;
        snprintf(title, sizeof(title), "OUT%c FIR Filter", 'A' + ch);
    } else {
        fir = (FIRFilter_t*)&g_inputs[ch].fir;
        snprintf(title, sizeof(title), "IN%d FIR Filter", ch + 1);
    }
    drawHeader(title);

    int y = 22;
    char line[40];
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.setTextDatum(ML_DATUM);

    snprintf(line, sizeof(line), "Status: %s", fir->enabled ? "ENABLED" : "DISABLED");
    tft.setTextColor(fir->enabled ? COL_VU_GREEN : COL_TEXT_DIM, COL_BG);
    tft.drawString(line, 4, y, 1); y += 14;
    tft.setTextColor(COL_TEXT, COL_BG);

    snprintf(line, sizeof(line), "Taps:   %d / %d", fir->numTaps, FIR_MAX_TAPS);
    tft.drawString(line, 4, y, 1); y += 14;

    // Draw impulse response visualization (first 64 taps)
    drawSeparator(y + 2); y += 6;
    tft.setTextColor(COL_ACCENT, COL_BG);
    tft.drawString("Impulse Response:", 4, y, 1); y += 12;

    int graphY = y;
    int graphH = 40;
    int graphW = SCR_W - 8;
    int midY = graphY + graphH / 2;

    // Draw center line
    tft.drawFastHLine(4, midY, graphW, COL_BORDER);

    // Draw taps as vertical lines from center
    int maxDraw = min(fir->numTaps, graphW);
    float maxCoeff = 0.001f;
    for (int t = 0; t < maxDraw; t++) {
        float absC = fabsf(fir->coeffs[t]);
        if (absC > maxCoeff) maxCoeff = absC;
    }

    for (int t = 0; t < maxDraw; t++) {
        float norm = fir->coeffs[t] / maxCoeff;
        int barH = (int)(norm * (graphH / 2));
        uint16_t col = (fir->coeffs[t] >= 0) ? COL_VU_GREEN : COL_VU_RED;
        if (barH > 0) {
            tft.drawFastVLine(4 + t, midY - barH, barH, col);
        } else if (barH < 0) {
            tft.drawFastVLine(4 + t, midY, -barH, col);
        }
    }
}

// ---------------------------------------------------------
// SCREEN: CROSSOVER EDIT
// ---------------------------------------------------------
void DisplayUI_DrawCrossoverEdit(int ch) {
    OutputChannel_t* out = (OutputChannel_t*)&g_outputs[ch];
    tft.fillScreen(COL_BG);

    char title[24];
    snprintf(title, sizeof(title), "OUT%c CROSSOVER", 'A' + ch);
    drawHeader(title);

    int y = 22;
    char line[48];
    const char* typeNames[] = {"Butterworth", "Bessel", "Linkwitz-Riley"};
    const char* slopeNames[] = {"6dB", "12dB", "18dB", "24dB", "", "", "", "48dB"};

    tft.setTextColor(COL_ACCENT, COL_BG);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("HIGH-PASS FILTER", 4, y, 1); y += 12;
    tft.setTextColor(COL_TEXT, COL_BG);

    snprintf(line, sizeof(line), " Enable: %s", out->hpf.enabled ? "ON" : "OFF");
    tft.drawString(line, 4, y, 1); y += 12;
    snprintf(line, sizeof(line), " Freq:   %.1f Hz", out->hpf.freq);
    tft.drawString(line, 4, y, 1); y += 12;
    snprintf(line, sizeof(line), " Type:   %s", typeNames[out->hpf.type]);
    tft.drawString(line, 4, y, 1); y += 12;
    int slopeIdx = (int)out->hpf.slope - 1;
    if (slopeIdx >= 0 && slopeIdx < 8) {
        snprintf(line, sizeof(line), " Slope:  %s/Oct", slopeNames[slopeIdx]);
        tft.drawString(line, 4, y, 1);
    }
    y += 14;

    drawSeparator(y); y += 4;

    tft.setTextColor(COL_ACCENT, COL_BG);
    tft.drawString("LOW-PASS FILTER", 4, y, 1); y += 12;
    tft.setTextColor(COL_TEXT, COL_BG);

    snprintf(line, sizeof(line), " Enable: %s", out->lpf.enabled ? "ON" : "OFF");
    tft.drawString(line, 4, y, 1); y += 12;
    snprintf(line, sizeof(line), " Freq:   %.1f Hz", out->lpf.freq);
    tft.drawString(line, 4, y, 1); y += 12;
    snprintf(line, sizeof(line), " Type:   %s", typeNames[out->lpf.type]);
    tft.drawString(line, 4, y, 1); y += 12;
    slopeIdx = (int)out->lpf.slope - 1;
    if (slopeIdx >= 0 && slopeIdx < 8) {
        snprintf(line, sizeof(line), " Slope:  %s/Oct", slopeNames[slopeIdx]);
        tft.drawString(line, 4, y, 1);
    }
}

// ---------------------------------------------------------
// SCREEN: DELAY EDIT
// ---------------------------------------------------------
void DisplayUI_DrawDelayEdit(int ch) {
    OutputChannel_t* out = (OutputChannel_t*)&g_outputs[ch];
    tft.fillScreen(COL_BG);

    char title[24];
    snprintf(title, sizeof(title), "OUT%c DELAY", 'A' + ch);
    drawHeader(title);

    int y = 24;
    char line[48];
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.setTextDatum(ML_DATUM);

    snprintf(line, sizeof(line), "Delay: %.1f ms", out->delay.delayMs);
    tft.drawString(line, 4, y, 2); y += 20;

    // Show distance equivalent
    float distCm = out->delay.delayMs * 34.3f; // Speed of sound ~343 m/s
    float distInch = distCm / 2.54f;
    snprintf(line, sizeof(line), "= %.1f cm", distCm);
    tft.setTextColor(COL_TEXT_DIM, COL_BG);
    tft.drawString(line, 4, y, 1); y += 14;
    snprintf(line, sizeof(line), "= %.1f inch", distInch);
    tft.drawString(line, 4, y, 1); y += 14;

    // Samples
    snprintf(line, sizeof(line), "= %.0f samples", out->delay.delaySamples);
    tft.drawString(line, 4, y, 1); y += 20;

    drawSeparator(y); y += 6;
    tft.setTextColor(COL_TEXT_DIM, COL_BG);
    tft.drawString("Rotate encoder to adjust", 4, y, 1);
    y += 12;
    snprintf(line, sizeof(line), "Range: 0 - %.0f ms", DELAY_MAX_MS);
    tft.drawString(line, 4, y, 1);
}

// ---------------------------------------------------------
// SCREEN: LIMITER EDIT
// ---------------------------------------------------------
void DisplayUI_DrawLimiterEdit(int ch) {
    OutputChannel_t* out = (OutputChannel_t*)&g_outputs[ch];
    PeakLimiter_t* l = (PeakLimiter_t*)&out->limiter;
    tft.fillScreen(COL_BG);

    char title[24];
    snprintf(title, sizeof(title), "OUT%c LIMITER", 'A' + ch);
    drawHeader(title);

    int y = 22;
    char line[48];
    tft.setTextDatum(ML_DATUM);

    // Enable
    tft.setTextColor(l->enabled ? COL_VU_GREEN : COL_VU_RED, COL_BG);
    snprintf(line, sizeof(line), "Status:  %s", l->enabled ? "ENABLED" : "DISABLED");
    tft.drawString(line, 4, y, 1); y += 14;
    tft.setTextColor(COL_TEXT, COL_BG);

    snprintf(line, sizeof(line), "Thresh:  %.1f dBFS", l->threshold);
    tft.drawString(line, 4, y, 1); y += 14;

    snprintf(line, sizeof(line), "Attack:  %.1f ms", l->attackMs);
    tft.drawString(line, 4, y, 1); y += 14;

    snprintf(line, sizeof(line), "Release: %.1f ms", l->releaseMs);
    tft.drawString(line, 4, y, 1); y += 14;

    drawSeparator(y + 2); y += 6;

    // Gain Reduction meter (horizontal bar)
    tft.setTextColor(COL_ACCENT, COL_BG);
    tft.drawString("Gain Reduction:", 4, y, 1); y += 12;

    float grNorm = clampf(-l->gainReduction / 20.0f, 0.0f, 1.0f); // 0..1 for 0..-20dB
    int barW = (int)(grNorm * (SCR_W - 16));
    tft.fillRect(8, y, SCR_W - 16, 10, COL_PANEL);
    if (barW > 0) {
        tft.fillRect(8, y, barW, 10, COL_VU_YELLOW);
    }
    y += 14;

    snprintf(line, sizeof(line), "GR: %.1f dB", l->gainReduction);
    tft.setTextColor(COL_TEXT_DIM, COL_BG);
    tft.drawString(line, 4, y, 1);
}

// ---------------------------------------------------------
// SCREEN: PRESETS
// ---------------------------------------------------------
void DisplayUI_DrawPresets() {
    tft.fillScreen(COL_BG);
    drawHeader("PRESETS");

    static const char* presetMenuItems[] = {
        "Load Preset",
        "Save Preset",
        "Rename Preset",
        "Export to JSON",
        "Import from JSON"
    };
    drawMenuList(presetMenuItems, 5, g_uiState.menuIndex, 20);
}

// ---------------------------------------------------------
// SCREEN: NETWORK
// ---------------------------------------------------------
void DisplayUI_DrawNetwork() {
    tft.fillScreen(COL_BG);
    drawHeader("NETWORK & WI-FI");

    int y = 24;
    char line[40];
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.setTextDatum(ML_DATUM);

    snprintf(line, sizeof(line), "Mode: Access Point");
    tft.drawString(line, 4, y, 1); y += 14;

    snprintf(line, sizeof(line), "SSID: %s", WIFI_SSID);
    tft.drawString(line, 4, y, 1); y += 14;

    snprintf(line, sizeof(line), "Pass: %s", WIFI_PASS);
    tft.drawString(line, 4, y, 1); y += 14;

    snprintf(line, sizeof(line), "IP:   192.168.4.1");
    tft.setTextColor(COL_ACCENT, COL_BG);
    tft.drawString(line, 4, y, 1); y += 14;

    tft.setTextColor(COL_TEXT_DIM, COL_BG);
    snprintf(line, sizeof(line), "Port: 80 (HTTP) / 81 (WS)");
    tft.drawString(line, 4, y, 1);
}

// ---------------------------------------------------------
// SCREEN: SYSTEM
// ---------------------------------------------------------
void DisplayUI_DrawSystem() {
    tft.fillScreen(COL_BG);
    drawHeader("SYSTEM SETTINGS");

    static const char* sysMenuItems[] = {
        "LCD Backlight",
        "Factory Reset",
        "Calibration",
        "Firmware Info"
    };
    drawMenuList(sysMenuItems, 4, g_uiState.menuIndex, 20);
}

// ---------------------------------------------------------
// NAVIGATION HANDLERS
// ---------------------------------------------------------
void DisplayUI_NavigateTo(UIScreen_t screen) {
    g_uiState.previousScreen = g_uiState.currentScreen;
    g_uiState.currentScreen = screen;
    g_uiState.menuIndex = 0;
    g_uiState.needsRedraw = true;
}

void DisplayUI_OnEncoderRotate(int direction) {
    switch (g_uiState.currentScreen) {
        case SCREEN_HOME:
            // On home screen, rotate scrolls through output channels briefly
            break;

        case SCREEN_MAIN_MENU:
            g_uiState.menuIndex += direction;
            if (g_uiState.menuIndex < 0) g_uiState.menuIndex = MAIN_MENU_COUNT - 1;
            if (g_uiState.menuIndex >= MAIN_MENU_COUNT) g_uiState.menuIndex = 0;
            g_uiState.needsRedraw = true;
            break;

        case SCREEN_PRESETS:
            g_uiState.menuIndex += direction;
            if (g_uiState.menuIndex < 0) g_uiState.menuIndex = 4;
            if (g_uiState.menuIndex > 4) g_uiState.menuIndex = 0;
            g_uiState.needsRedraw = true;
            break;

        case SCREEN_SYSTEM:
            g_uiState.menuIndex += direction;
            if (g_uiState.menuIndex < 0) g_uiState.menuIndex = 3;
            if (g_uiState.menuIndex > 3) g_uiState.menuIndex = 0;
            g_uiState.needsRedraw = true;
            break;

        case SCREEN_DELAY_EDIT: {
            // Adjust delay with encoder
            OutputChannel_t* out = (OutputChannel_t*)&g_outputs[g_uiState.selectedChannel];
            float newDelay = out->delay.delayMs + (float)direction * 0.1f;
            if (newDelay < 0.0f) newDelay = 0.0f;
            if (newDelay > DELAY_MAX_MS) newDelay = DELAY_MAX_MS;
            out->delay.delayMs = newDelay;
            out->delay.delaySamples = newDelay * (float)SAMPLE_RATE / 1000.0f;
            g_uiState.needsRedraw = true;
            break;
        }

        default:
            break;
    }
}

void DisplayUI_OnEncoderPress() {
    switch (g_uiState.currentScreen) {
        case SCREEN_HOME:
            DisplayUI_NavigateTo(SCREEN_MAIN_MENU);
            break;

        case SCREEN_MAIN_MENU:
            switch (g_uiState.menuIndex) {
                case 0: DisplayUI_NavigateTo(SCREEN_INPUT_CONFIG); break;
                case 1: DisplayUI_NavigateTo(SCREEN_OUTPUT_CONFIG); break;
                case 2: DisplayUI_NavigateTo(SCREEN_PRESETS); break;
                case 3: DisplayUI_NavigateTo(SCREEN_NETWORK); break;
                case 4: DisplayUI_NavigateTo(SCREEN_SYSTEM); break;
            }
            break;

        case SCREEN_INPUT_CH:
        case SCREEN_OUTPUT_CH:
        case SCREEN_PEQ_EDIT:
        case SCREEN_FIR_VIEW:
        case SCREEN_XOVER_EDIT:
        case SCREEN_DELAY_EDIT:
        case SCREEN_LIMITER_EDIT:
        case SCREEN_PRESETS:
        case SCREEN_NETWORK:
        case SCREEN_SYSTEM:
        case SCREEN_INPUT_CONFIG:
        case SCREEN_OUTPUT_CONFIG:
            // Go back to previous screen
            DisplayUI_NavigateTo(g_uiState.previousScreen);
            break;

        default:
            DisplayUI_NavigateTo(SCREEN_HOME);
            break;
    }
}

void DisplayUI_OnButtonPress(int buttonId) {
    switch (buttonId) {
        case BTN_IN1:
            g_uiState.selectedChannel = 0;
            g_uiState.isOutputChannel = false;
            DisplayUI_NavigateTo(SCREEN_INPUT_CH);
            break;
        case BTN_IN2:
            g_uiState.selectedChannel = 1;
            g_uiState.isOutputChannel = false;
            DisplayUI_NavigateTo(SCREEN_INPUT_CH);
            break;
        case BTN_OUT_A:
            g_uiState.selectedChannel = 0;
            g_uiState.isOutputChannel = true;
            DisplayUI_NavigateTo(SCREEN_OUTPUT_CH);
            break;
        case BTN_OUT_B:
            g_uiState.selectedChannel = 1;
            g_uiState.isOutputChannel = true;
            DisplayUI_NavigateTo(SCREEN_OUTPUT_CH);
            break;
        case BTN_OUT_C:
            g_uiState.selectedChannel = 2;
            g_uiState.isOutputChannel = true;
            DisplayUI_NavigateTo(SCREEN_OUTPUT_CH);
            break;
        case BTN_OUT_D:
            g_uiState.selectedChannel = 3;
            g_uiState.isOutputChannel = true;
            DisplayUI_NavigateTo(SCREEN_OUTPUT_CH);
            break;
    }
}

// ---------------------------------------------------------
// INIT & UPDATE
// ---------------------------------------------------------
void DisplayUI_Init() {
    tft.init();
    tft.setRotation(1); // Landscape: 160x128
    tft.fillScreen(COL_BG);

    // Show boot splash
    tft.setTextColor(COL_ACCENT, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("DLMS 2.4", SCR_W / 2, SCR_H / 2 - 10, 2);
    tft.setTextColor(COL_TEXT_DIM, COL_BG);
    tft.drawString("Digital Loudspeaker", SCR_W / 2, SCR_H / 2 + 8, 1);
    tft.drawString("Management System", SCR_W / 2, SCR_H / 2 + 20, 1);
    delay(1500);

    // Init UI state
    g_uiState.currentScreen = SCREEN_HOME;
    g_uiState.previousScreen = SCREEN_HOME;
    g_uiState.selectedChannel = 0;
    g_uiState.isOutputChannel = false;
    g_uiState.menuIndex = 0;
    g_uiState.menuScrollOffset = 0;
    g_uiState.peqBandIndex = 0;
    g_uiState.editing = false;
    g_uiState.needsRedraw = true;
    g_uiState.vuNeedsUpdate = true;
    g_uiState.lastVuUpdate = 0;

    Serial.println("[UI] Display initialized");
}

void DisplayUI_Update() {
    unsigned long now = millis();

    // Full redraw if needed
    if (g_uiState.needsRedraw) {
        g_uiState.needsRedraw = false;

        switch (g_uiState.currentScreen) {
            case SCREEN_HOME:         DisplayUI_DrawHome(); break;
            case SCREEN_MAIN_MENU:    DisplayUI_DrawMainMenu(); break;
            case SCREEN_INPUT_CH:
            case SCREEN_INPUT_CONFIG:
                DisplayUI_DrawInputChannel(g_uiState.selectedChannel); break;
            case SCREEN_OUTPUT_CH:
            case SCREEN_OUTPUT_CONFIG:
                DisplayUI_DrawOutputChannel(g_uiState.selectedChannel); break;
            case SCREEN_PEQ_EDIT:
                DisplayUI_DrawPEQEdit(g_uiState.isOutputChannel,
                                      g_uiState.selectedChannel,
                                      g_uiState.peqBandIndex); break;
            case SCREEN_FIR_VIEW:
                DisplayUI_DrawFIRView(g_uiState.isOutputChannel,
                                      g_uiState.selectedChannel); break;
            case SCREEN_XOVER_EDIT:
                DisplayUI_DrawCrossoverEdit(g_uiState.selectedChannel); break;
            case SCREEN_DELAY_EDIT:
                DisplayUI_DrawDelayEdit(g_uiState.selectedChannel); break;
            case SCREEN_LIMITER_EDIT:
                DisplayUI_DrawLimiterEdit(g_uiState.selectedChannel); break;
            case SCREEN_PRESETS:      DisplayUI_DrawPresets(); break;
            case SCREEN_NETWORK:      DisplayUI_DrawNetwork(); break;
            case SCREEN_SYSTEM:       DisplayUI_DrawSystem(); break;
        }
    }

    // Periodic VU meter update on home screen (only update VU bars, not full redraw)
    if (g_uiState.currentScreen == SCREEN_HOME &&
        (now - g_uiState.lastVuUpdate) >= VU_UPDATE_RATE_MS) {
        g_uiState.lastVuUpdate = now;

        // Update input VU bars
        drawVUBar(12, VU_INPUT_Y, VU_BAR_W, VU_BAR_H,
                  g_inputs[0].vu.peakLevel, g_inputs[0].vu.clip);
        drawVUBar(32, VU_INPUT_Y, VU_BAR_W, VU_BAR_H,
                  g_inputs[1].vu.peakLevel, g_inputs[1].vu.clip);

        // Update output VU bars
        for (int o = 0; o < NUM_OUTPUTS; o++) {
            drawVUBar(66 + o * 20, VU_OUTPUT_Y, VU_BAR_W, VU_BAR_H,
                      g_outputs[o].vu.peakLevel, g_outputs[o].vu.clip);
        }

        // Update dB readout
        int dbY = VU_INPUT_Y + VU_BAR_H + 4;
        char dbStr[8];
        tft.setTextColor(COL_TEXT_DIM, COL_BG);
        tft.setTextDatum(MC_DATUM);

        snprintf(dbStr, sizeof(dbStr), "%3.0f", g_inputs[0].vu.peakDb);
        tft.drawString(dbStr, 16, dbY, 1);
        snprintf(dbStr, sizeof(dbStr), "%3.0f", g_inputs[1].vu.peakDb);
        tft.drawString(dbStr, 36, dbY, 1);

        for (int o = 0; o < NUM_OUTPUTS; o++) {
            snprintf(dbStr, sizeof(dbStr), "%3.0f", g_outputs[o].vu.peakDb);
            tft.drawString(dbStr, 66 + o * 20, dbY, 1);
        }
    }
}
