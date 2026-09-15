#include "HardwareControl.h"
#include "Config.h"
#include "AudioDSP.h"
#include "DisplayUI.h"
#include <Wire.h>
#include <RotaryEncoder.h>
#include <Preferences.h>
#include <PCF8574.h>
#include <ArduinoJson.h>
#include "mbedtls/sha256.h"  // ESP32 hardware-accelerated SHA-256

// ---------------------------------------------------------
// HARDWARE INSTANCES
// ---------------------------------------------------------
static RotaryEncoder encoder(PIN_ENC_CLK, PIN_ENC_DT, RotaryEncoder::LatchMode::TWO03);
static PCF8574 pcf(PCF8574_ADDR);
static Preferences preferences;

// Button debounce state
static uint8_t lastPCFState = 0xFF; // All high (released) initially
static unsigned long lastDebounceTime[6] = {0};
static const unsigned long DEBOUNCE_MS = 50;

// Encoder button debounce
static bool lastEncBtnState = true; // INPUT_PULLUP: high = not pressed
static unsigned long lastEncBtnDebounce = 0;
static bool encBtnProcessed = false;

// Encoder long-press tracking (for password reset)
static unsigned long encBtnPressStart = 0;
static bool encBtnHeld = false;
static bool encLongPressTriggered = false;

// Encoder position tracking
static long lastEncoderPos = 0;

// Active preset slot
static int activePresetSlot = 0;

// ---------------------------------------------------------
// AUTHENTICATION SYSTEM
// ---------------------------------------------------------
typedef struct {
    char token[AUTH_TOKEN_LEN + 1]; // Hex token string
    unsigned long createdAt;         // millis() timestamp
    bool active;
} AuthSession_t;

static AuthSession_t sessions[AUTH_MAX_SESSIONS];
static char storedPasswordHash[65]; // SHA-256 hex string (64 chars + null)

// SHA-256 hash helper
static String sha256Hash(const String& input) {
    unsigned char hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); // 0 = SHA-256, 1 = SHA-224
    mbedtls_sha256_update(&ctx, (const unsigned char*)input.c_str(), input.length());
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    char hexStr[65];
    for (int i = 0; i < 32; i++) {
        sprintf(&hexStr[i * 2], "%02x", hash[i]);
    }
    hexStr[64] = 0;
    return String(hexStr);
}

// Generate a random hex token
static String generateToken() {
    char token[AUTH_TOKEN_LEN + 1];
    for (int i = 0; i < AUTH_TOKEN_LEN; i++) {
        token[i] = "0123456789abcdef"[esp_random() % 16];
    }
    token[AUTH_TOKEN_LEN] = 0;
    return String(token);
}

void Auth_Init() {
    // Load stored password hash from NVS
    String hash = preferences.getString("auth_hash", "");
    if (hash.length() != 64) {
        // No password stored yet: hash the default password and store it
        hash = sha256Hash(AUTH_DEFAULT_PASS);
        preferences.putString("auth_hash", hash);
        Serial.println("[AUTH] Default password set");
    }
    strncpy(storedPasswordHash, hash.c_str(), 64);
    storedPasswordHash[64] = 0;

    // Clear all sessions
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        sessions[i].active = false;
        sessions[i].token[0] = 0;
    }
    Serial.println("[AUTH] Authentication initialized");
}

bool Auth_ValidatePassword(const String& password) {
    String hash = sha256Hash(password);
    return (hash == String(storedPasswordHash));
}

bool Auth_ChangePassword(const String& oldPass, const String& newPass) {
    if (!Auth_ValidatePassword(oldPass)) {
        Serial.println("[AUTH] Change password failed: old password incorrect");
        return false;
    }
    if (newPass.length() < 4) {
        Serial.println("[AUTH] Change password failed: new password too short");
        return false;
    }
    String hash = sha256Hash(newPass);
    preferences.putString("auth_hash", hash);
    strncpy(storedPasswordHash, hash.c_str(), 64);
    storedPasswordHash[64] = 0;
    Serial.println("[AUTH] Password changed successfully");
    return true;
}

void Auth_ResetToDefault() {
    String hash = sha256Hash(AUTH_DEFAULT_PASS);
    preferences.putString("auth_hash", hash);
    strncpy(storedPasswordHash, hash.c_str(), 64);
    storedPasswordHash[64] = 0;
    // Invalidate all active sessions
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        sessions[i].active = false;
    }
    Serial.println("[AUTH] Password reset to default via hardware override");
}

String Auth_CreateSession() {
    // Clean expired sessions first
    Auth_CleanExpiredSessions();

    // Find a free session slot
    int slot = -1;
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (!sessions[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        // Evict oldest session
        unsigned long oldest = ULONG_MAX;
        for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
            if (sessions[i].createdAt < oldest) {
                oldest = sessions[i].createdAt;
                slot = i;
            }
        }
    }

    String token = generateToken();
    strncpy(sessions[slot].token, token.c_str(), AUTH_TOKEN_LEN);
    sessions[slot].token[AUTH_TOKEN_LEN] = 0;
    sessions[slot].createdAt = millis();
    sessions[slot].active = true;

    Serial.printf("[AUTH] Session created in slot %d\n", slot);
    return token;
}

bool Auth_ValidateSession(const String& token) {
    if (token.length() == 0) return false;
    unsigned long now = millis();
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (sessions[i].active &&
            strcmp(sessions[i].token, token.c_str()) == 0) {
            // Check expiry
            if ((now - sessions[i].createdAt) > AUTH_SESSION_TIMEOUT) {
                sessions[i].active = false;
                return false;
            }
            // Refresh session timestamp on valid access
            sessions[i].createdAt = now;
            return true;
        }
    }
    return false;
}

void Auth_DestroySession(const String& token) {
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (sessions[i].active &&
            strcmp(sessions[i].token, token.c_str()) == 0) {
            sessions[i].active = false;
            Serial.printf("[AUTH] Session destroyed in slot %d\n", i);
            return;
        }
    }
}

void Auth_CleanExpiredSessions() {
    unsigned long now = millis();
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (sessions[i].active &&
            (now - sessions[i].createdAt) > AUTH_SESSION_TIMEOUT) {
            sessions[i].active = false;
        }
    }
}

// ---------------------------------------------------------
// PRESET SYSTEM (NVS-based)
// ---------------------------------------------------------

// Serialization helpers
static void serializeInputChannel(const InputChannel_t* inp, JsonObject& obj) {
    obj["gain"] = inp->gain;
    obj["mute"] = (bool)inp->mute;
    obj["phase"] = (bool)inp->phaseInvert;

    JsonArray peqArr = obj["peq"].to<JsonArray>();
    for (int b = 0; b < INPUT_PEQ_BANDS; b++) {
        JsonObject band = peqArr.add<JsonObject>();
        band["en"] = (bool)inp->peq[b].enabled;
        band["type"] = (int)inp->peq[b].type;
        band["freq"] = inp->peq[b].freq;
        band["gain"] = inp->peq[b].gain;
        band["q"] = inp->peq[b].q;
    }

    // FIR state
    JsonObject firObj = obj["fir"].to<JsonObject>();
    firObj["en"] = (bool)inp->fir.enabled;
    firObj["taps"] = inp->fir.numTaps;
    JsonArray coeffArr = firObj["c"].to<JsonArray>();
    for (int t = 0; t < inp->fir.numTaps; t++) {
        coeffArr.add(inp->fir.coeffs[t]);
    }
}

static void serializeOutputChannel(const OutputChannel_t* out, JsonObject& obj) {
    obj["gain"] = out->gain;
    obj["mute"] = (bool)out->mute;
    obj["phase"] = (bool)out->phaseInvert;
    obj["source"] = (int)out->source;

    // Crossover HPF
    JsonObject hpf = obj["hpf"].to<JsonObject>();
    hpf["en"] = (bool)out->hpf.enabled;
    hpf["freq"] = out->hpf.freq;
    hpf["type"] = (int)out->hpf.type;
    hpf["slope"] = (int)out->hpf.slope;

    // Crossover LPF
    JsonObject lpf = obj["lpf"].to<JsonObject>();
    lpf["en"] = (bool)out->lpf.enabled;
    lpf["freq"] = out->lpf.freq;
    lpf["type"] = (int)out->lpf.type;
    lpf["slope"] = (int)out->lpf.slope;

    // PEQ
    JsonArray peqArr = obj["peq"].to<JsonArray>();
    for (int b = 0; b < OUTPUT_PEQ_BANDS; b++) {
        JsonObject band = peqArr.add<JsonObject>();
        band["en"] = (bool)out->peq[b].enabled;
        band["type"] = (int)out->peq[b].type;
        band["freq"] = out->peq[b].freq;
        band["gain"] = out->peq[b].gain;
        band["q"] = out->peq[b].q;
    }

    // Delay
    obj["delay_ms"] = out->delay.delayMs;

    // Limiter
    JsonObject lim = obj["limiter"].to<JsonObject>();
    lim["en"] = (bool)out->limiter.enabled;
    lim["thresh"] = out->limiter.threshold;
    lim["attack"] = out->limiter.attackMs;
    lim["release"] = out->limiter.releaseMs;

    // FIR
    JsonObject firObj = obj["fir"].to<JsonObject>();
    firObj["en"] = (bool)out->fir.enabled;
    firObj["taps"] = out->fir.numTaps;
    JsonArray coeffArr = firObj["c"].to<JsonArray>();
    for (int t = 0; t < out->fir.numTaps; t++) {
        coeffArr.add(out->fir.coeffs[t]);
    }
}

static void deserializeInputChannel(InputChannel_t* inp, JsonObject& obj) {
    inp->gain = obj["gain"] | 0.0f;
    inp->mute = obj["mute"] | false;
    inp->phaseInvert = obj["phase"] | false;
    inp->gainLinear = dBtoLinear(inp->gain);

    JsonArray peqArr = obj["peq"];
    for (int b = 0; b < INPUT_PEQ_BANDS && b < (int)peqArr.size(); b++) {
        JsonObject band = peqArr[b];
        inp->peq[b].enabled = band["en"] | false;
        inp->peq[b].type = (FilterType_t)(band["type"] | 0);
        inp->peq[b].freq = band["freq"] | 1000.0f;
        inp->peq[b].gain = band["gain"] | 0.0f;
        inp->peq[b].q = band["q"] | 1.0f;
    }

    // FIR
    JsonObject firObj = obj["fir"];
    inp->fir.enabled = firObj["en"] | false;
    inp->fir.numTaps = firObj["taps"] | 1;
    if (inp->fir.numTaps > FIR_MAX_TAPS) inp->fir.numTaps = FIR_MAX_TAPS;
    JsonArray coeffArr = firObj["c"];
    for (int t = 0; t < inp->fir.numTaps && t < (int)coeffArr.size(); t++) {
        inp->fir.coeffs[t] = coeffArr[t] | 0.0f;
    }
}

static void deserializeOutputChannel(OutputChannel_t* out, JsonObject& obj) {
    out->gain = obj["gain"] | 0.0f;
    out->mute = obj["mute"] | false;
    out->phaseInvert = obj["phase"] | false;
    out->source = (RouteSource_t)(obj["source"] | 2);
    out->gainLinear = dBtoLinear(out->gain);

    // HPF
    JsonObject hpf = obj["hpf"];
    out->hpf.enabled = hpf["en"] | false;
    out->hpf.freq = hpf["freq"] | 20.0f;
    out->hpf.type = (CrossoverType_t)(hpf["type"] | 2);
    out->hpf.slope = (CrossoverSlope_t)(hpf["slope"] | 4);

    // LPF
    JsonObject lpf = obj["lpf"];
    out->lpf.enabled = lpf["en"] | false;
    out->lpf.freq = lpf["freq"] | 20000.0f;
    out->lpf.type = (CrossoverType_t)(lpf["type"] | 2);
    out->lpf.slope = (CrossoverSlope_t)(lpf["slope"] | 4);

    // PEQ
    JsonArray peqArr = obj["peq"];
    for (int b = 0; b < OUTPUT_PEQ_BANDS && b < (int)peqArr.size(); b++) {
        JsonObject band = peqArr[b];
        out->peq[b].enabled = band["en"] | false;
        out->peq[b].type = (FilterType_t)(band["type"] | 0);
        out->peq[b].freq = band["freq"] | 1000.0f;
        out->peq[b].gain = band["gain"] | 0.0f;
        out->peq[b].q = band["q"] | 1.0f;
    }

    // Delay
    out->delay.delayMs = obj["delay_ms"] | 0.0f;
    out->delay.delaySamples = out->delay.delayMs * (float)SAMPLE_RATE / 1000.0f;

    // Limiter
    JsonObject lim = obj["limiter"];
    out->limiter.enabled = lim["en"] | false;
    out->limiter.threshold = lim["thresh"] | -2.0f;
    out->limiter.attackMs = lim["attack"] | 10.0f;
    out->limiter.releaseMs = lim["release"] | 100.0f;

    // FIR
    JsonObject firObj = obj["fir"];
    out->fir.enabled = firObj["en"] | false;
    out->fir.numTaps = firObj["taps"] | 1;
    if (out->fir.numTaps > FIR_MAX_TAPS) out->fir.numTaps = FIR_MAX_TAPS;
    JsonArray coeffArr = firObj["c"];
    for (int t = 0; t < out->fir.numTaps && t < (int)coeffArr.size(); t++) {
        out->fir.coeffs[t] = coeffArr[t] | 0.0f;
    }
}

// ---------------------------------------------------------
// PUBLIC PRESET API
// ---------------------------------------------------------
String Preset_ExportJSON() {
    // Use PSRAM for large JSON document
    JsonDocument doc;

    doc["version"] = "DLMS2.4";
    doc["preset"] = activePresetSlot;

    JsonArray inputs = doc["inputs"].to<JsonArray>();
    for (int i = 0; i < NUM_INPUTS; i++) {
        JsonObject obj = inputs.add<JsonObject>();
        serializeInputChannel((const InputChannel_t*)&g_inputs[i], obj);
    }

    JsonArray outputs = doc["outputs"].to<JsonArray>();
    for (int o = 0; o < NUM_OUTPUTS; o++) {
        JsonObject obj = outputs.add<JsonObject>();
        serializeOutputChannel((const OutputChannel_t*)&g_outputs[o], obj);
    }

    String result;
    serializeJsonPretty(doc, result);
    return result;
}

bool Preset_ImportJSON(const String& json) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);
    if (error) {
        Serial.printf("[PRESET] JSON parse error: %s\n", error.c_str());
        return false;
    }

    // Verify version
    String ver = doc["version"] | "";
    if (ver != "DLMS2.4") {
        Serial.println("[PRESET] Invalid preset version");
        return false;
    }

    JsonArray inputs = doc["inputs"];
    for (int i = 0; i < NUM_INPUTS && i < (int)inputs.size(); i++) {
        JsonObject obj = inputs[i];
        deserializeInputChannel((InputChannel_t*)&g_inputs[i], obj);
    }

    JsonArray outputs = doc["outputs"];
    for (int o = 0; o < NUM_OUTPUTS && o < (int)outputs.size(); o++) {
        JsonObject obj = outputs[o];
        deserializeOutputChannel((OutputChannel_t*)&g_outputs[o], obj);
    }

    // Recalculate all DSP coefficients
    g_dspParamsDirty = true;

    Serial.println("[PRESET] Import successful");
    return true;
}

void Preset_Save(int slot) {
    if (slot < 0 || slot >= MAX_PRESETS) return;

    String json = Preset_ExportJSON();
    char key[16];
    snprintf(key, sizeof(key), "preset_%02d", slot);

    preferences.putString(key, json);
    activePresetSlot = slot;
    Serial.printf("[PRESET] Saved to slot %d (%d bytes)\n", slot, json.length());
}

void Preset_Load(int slot) {
    if (slot < 0 || slot >= MAX_PRESETS) return;

    char key[16];
    snprintf(key, sizeof(key), "preset_%02d", slot);

    String json = preferences.getString(key, "");
    if (json.length() == 0) {
        Serial.printf("[PRESET] Slot %d is empty\n", slot);
        return;
    }

    if (Preset_ImportJSON(json)) {
        activePresetSlot = slot;
        Serial.printf("[PRESET] Loaded from slot %d\n", slot);
    }
}

void Preset_SetName(int slot, const char* name) {
    char key[16];
    snprintf(key, sizeof(key), "pname_%02d", slot);
    preferences.putString(key, name);
}

const char* Preset_GetName(int slot) {
    static char nameBuf[PRESET_NAME_LEN];
    char key[16];
    snprintf(key, sizeof(key), "pname_%02d", slot);
    String name = preferences.getString(key, "");
    if (name.length() == 0) {
        snprintf(nameBuf, sizeof(nameBuf), "Preset %02d", slot + 1);
    } else {
        strncpy(nameBuf, name.c_str(), PRESET_NAME_LEN - 1);
        nameBuf[PRESET_NAME_LEN - 1] = 0;
    }
    return nameBuf;
}

int Preset_GetActive() {
    return activePresetSlot;
}

// ---------------------------------------------------------
// INIT
// ---------------------------------------------------------
void HardwareControl_Init() {
    // I2C bus
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000); // 400kHz Fast Mode

    // PCF8574 I/O Expander (6 tactile buttons, active-low)
    if (pcf.begin()) {
        Serial.println("[HW] PCF8574 initialized");
        // Set all 6 button pins as inputs (high impedance = pulled up)
        for (int i = 0; i < 6; i++) {
            pcf.write(i, HIGH);
        }
    } else {
        Serial.println("[HW] PCF8574 NOT found!");
    }

    // Rotary encoder button
    pinMode(PIN_ENC_SW, INPUT_PULLUP);

    // NVS Preferences
    preferences.begin("dlms", false);

    // Initialize authentication system
    Auth_Init();

    // Load last active preset
    activePresetSlot = preferences.getInt("activePreset", 0);
    Preset_Load(activePresetSlot);

    Serial.println("[HW] Hardware Control initialized");
}

// ---------------------------------------------------------
// UPDATE (called at ~33Hz from Core 0 task)
// ---------------------------------------------------------
void HardwareControl_Update() {
    unsigned long now = millis();

    // --- Rotary Encoder ---
    encoder.tick();
    long newPos = encoder.getPosition();
    if (newPos != lastEncoderPos) {
        int direction = (newPos > lastEncoderPos) ? 1 : -1;
        lastEncoderPos = newPos;
        DisplayUI_OnEncoderRotate(direction);
    }

    // --- Encoder Button (with debounce + long-press detection) ---
    bool encBtn = digitalRead(PIN_ENC_SW);
    if (encBtn != lastEncBtnState) {
        lastEncBtnDebounce = now;
        encBtnProcessed = false;
        if (!encBtn) {
            // Button just pressed down
            encBtnPressStart = now;
            encBtnHeld = true;
            encLongPressTriggered = false;
        } else {
            // Button just released
            if (encBtnHeld && !encLongPressTriggered) {
                // Short press (released before long-press threshold)
                DisplayUI_OnEncoderPress();
            }
            encBtnHeld = false;
        }
    }

    // Long-press detection: if held for AUTH_LONG_PRESS_MS on SYSTEM screen
    if (encBtnHeld && !encLongPressTriggered && !encBtn) {
        unsigned long holdDuration = now - encBtnPressStart;
        if (holdDuration >= AUTH_LONG_PRESS_MS) {
            encLongPressTriggered = true;
            // Only reset password if currently on the System Settings screen
            if (g_uiState.currentScreen == SCREEN_SYSTEM) {
                Auth_ResetToDefault();
                Serial.println("[HW] LONG PRESS: Admin password reset to default!");
                // Visual feedback on LCD
                DisplayUI_NavigateTo(SCREEN_SYSTEM); // Force redraw with status message
            }
        }
    }
    lastEncBtnState = encBtn;

    // --- PCF8574 Buttons (with debounce) ---
    uint8_t pcfState = pcf.read8();
    for (int i = 0; i < 6; i++) {
        bool currentState = !(pcfState & (1 << i)); // Active LOW
        bool lastState    = !(lastPCFState & (1 << i));

        if (currentState && !lastState) {
            // Rising edge (button just pressed)
            if ((now - lastDebounceTime[i]) > DEBOUNCE_MS) {
                lastDebounceTime[i] = now;
                DisplayUI_OnButtonPress(i);
                Serial.printf("[HW] Button %d pressed\n", i);
            }
        }
    }
    lastPCFState = pcfState;
}
