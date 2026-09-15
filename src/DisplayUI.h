#ifndef DISPLAY_UI_H
#define DISPLAY_UI_H

#include <Arduino.h>
#include "Config.h"

// UI Screen / Page identifiers
typedef enum {
    SCREEN_HOME = 0,      // Default: VU meters + preset name + Wi-Fi status
    SCREEN_INPUT_CH,      // Input channel detail (gain, PEQ, FIR)
    SCREEN_OUTPUT_CH,     // Output channel detail (crossover, PEQ, FIR, delay, limiter)
    SCREEN_MAIN_MENU,     // Main menu navigation
    SCREEN_INPUT_CONFIG,  // Input configuration sub-menu
    SCREEN_OUTPUT_CONFIG, // Output configuration sub-menu
    SCREEN_PRESETS,       // Preset management
    SCREEN_NETWORK,       // Wi-Fi / network info
    SCREEN_SYSTEM,        // System settings
    SCREEN_PEQ_EDIT,      // PEQ band editor
    SCREEN_FIR_VIEW,      // FIR filter view/status
    SCREEN_XOVER_EDIT,    // Crossover parameter editor
    SCREEN_DELAY_EDIT,    // Delay editor
    SCREEN_LIMITER_EDIT   // Limiter editor
} UIScreen_t;

// UI state machine
typedef struct {
    UIScreen_t  currentScreen;
    UIScreen_t  previousScreen;
    int         selectedChannel;    // 0-1 for Input, 0-3 for Output
    bool        isOutputChannel;    // true = Output, false = Input
    int         menuIndex;          // Current menu item selection
    int         menuScrollOffset;   // Scroll offset for long menus
    int         peqBandIndex;       // Currently selected PEQ band
    bool        editing;            // true = editing parameter value
    bool        needsRedraw;        // Dirty flag for full redraw
    bool        vuNeedsUpdate;      // Dirty flag for VU meter update only
    unsigned long lastVuUpdate;     // Timestamp of last VU draw
} UIState_t;

extern UIState_t g_uiState;

// Lifecycle
void DisplayUI_Init();
void DisplayUI_Update();

// Screen renderers
void DisplayUI_DrawHome();
void DisplayUI_DrawMainMenu();
void DisplayUI_DrawInputChannel(int ch);
void DisplayUI_DrawOutputChannel(int ch);
void DisplayUI_DrawPEQEdit(bool isOutput, int ch, int band);
void DisplayUI_DrawFIRView(bool isOutput, int ch);
void DisplayUI_DrawCrossoverEdit(int ch);
void DisplayUI_DrawDelayEdit(int ch);
void DisplayUI_DrawLimiterEdit(int ch);
void DisplayUI_DrawPresets();
void DisplayUI_DrawNetwork();
void DisplayUI_DrawSystem();

// Navigation handlers (called by HardwareControl)
void DisplayUI_OnEncoderRotate(int direction);  // +1 or -1
void DisplayUI_OnEncoderPress();
void DisplayUI_OnButtonPress(int buttonId);     // BTN_IN1..BTN_OUT_D
void DisplayUI_NavigateTo(UIScreen_t screen);

#endif // DISPLAY_UI_H
