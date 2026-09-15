#include "WebServerInterface.h"
#include "Config.h"
#include "AudioDSP.h"
#include "HardwareControl.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include "WebUI.h"

// ---------------------------------------------------------
// SERVER INSTANCES
// ---------------------------------------------------------
static AsyncWebServer server(80);
static WebSocketsServer webSocket(81);

// VU meter broadcast timer
static unsigned long lastVuBroadcast = 0;
static const unsigned long VU_BROADCAST_MS = 50; // 20Hz update to browser

// Track authenticated WebSocket clients (by client number)
static String wsClientTokens[5] = {""}; // Map client# -> session token

// ---------------------------------------------------------
// WEBSOCKET: Handle incoming commands from browser
// ---------------------------------------------------------
static void handleWSCommand(uint8_t clientNum, const String& payload) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("[WS] JSON parse error: %s\n", err.c_str());
        return;
    }

    const char* cmd = doc["cmd"];
    if (!cmd) return;

    // ---- LOGIN (unauthenticated endpoint) ----
    if (strcmp(cmd, "login") == 0) {
        String password = doc["password"] | "";
        if (Auth_ValidatePassword(password)) {
            String token = Auth_CreateSession();
            if (clientNum < 5) wsClientTokens[clientNum] = token;
            char buf[128];
            snprintf(buf, sizeof(buf),
                "{\"type\":\"authOk\",\"token\":\"%s\"}", token.c_str());
            webSocket.sendTXT(clientNum, buf);
            // Send full DSP state after successful auth
            String json = Preset_ExportJSON();
            webSocket.sendTXT(clientNum, "{\"type\":\"state\",\"data\":" + json + "}");
            Serial.printf("[WS] Client #%u authenticated\n", clientNum);
        } else {
            webSocket.sendTXT(clientNum, "{\"type\":\"authFail\",\"msg\":\"Invalid password\"}");
            Serial.printf("[WS] Client #%u auth FAILED\n", clientNum);
        }
        return;
    }

    // ---- All other commands require a valid session ----
    String token = doc["token"] | "";
    // Also check stored client token
    if (clientNum < 5 && token.length() == 0) {
        token = wsClientTokens[clientNum];
    }
    if (!Auth_ValidateSession(token)) {
        webSocket.sendTXT(clientNum, "{\"type\":\"authRequired\",\"msg\":\"Session expired or invalid\"}");
        return;
    }

    // ---- CHANGE PASSWORD ----
    if (strcmp(cmd, "changePassword") == 0) {
        String oldPass = doc["oldPass"] | "";
        String newPass = doc["newPass"] | "";
        if (Auth_ChangePassword(oldPass, newPass)) {
            webSocket.sendTXT(clientNum, "{\"type\":\"passwordChanged\",\"ok\":true}");
        } else {
            webSocket.sendTXT(clientNum, "{\"type\":\"passwordChanged\",\"ok\":false,\"msg\":\"Old password incorrect or new too short\"}");
        }
        return;
    }

    // ---- LOGOUT ----
    if (strcmp(cmd, "logout") == 0) {
        Auth_DestroySession(token);
        if (clientNum < 5) wsClientTokens[clientNum] = "";
        webSocket.sendTXT(clientNum, "{\"type\":\"loggedOut\"}");
        return;
    }
    // ---- MUTE ----
    if (strcmp(cmd, "mute") == 0) {
        const char* ch = doc["ch"];
        bool state = doc["state"] | false;
        if (strcmp(ch, "in1") == 0)      g_inputs[0].mute = state;
        else if (strcmp(ch, "in2") == 0) g_inputs[1].mute = state;
        else if (strcmp(ch, "outA") == 0) g_outputs[0].mute = state;
        else if (strcmp(ch, "outB") == 0) g_outputs[1].mute = state;
        else if (strcmp(ch, "outC") == 0) g_outputs[2].mute = state;
        else if (strcmp(ch, "outD") == 0) g_outputs[3].mute = state;
    }

    // ---- INPUT GAIN ----
    else if (strcmp(cmd, "inputGain") == 0) {
        int ch = doc["ch"] | 0;
        float gain = doc["val"] | 0.0f;
        if (ch >= 0 && ch < NUM_INPUTS) {
            g_inputs[ch].gain = gain;
            g_inputs[ch].gainLinear = dBtoLinear(gain);
        }
    }

    // ---- OUTPUT GAIN ----
    else if (strcmp(cmd, "outputGain") == 0) {
        int ch = doc["ch"] | 0;
        float gain = doc["val"] | 0.0f;
        if (ch >= 0 && ch < NUM_OUTPUTS) {
            g_outputs[ch].gain = gain;
            g_outputs[ch].gainLinear = dBtoLinear(gain);
        }
    }

    // ---- ROUTING SOURCE ----
    else if (strcmp(cmd, "route") == 0) {
        int ch = doc["ch"] | 0;
        int src = doc["val"] | 2;
        if (ch >= 0 && ch < NUM_OUTPUTS) {
            g_outputs[ch].source = (RouteSource_t)src;
        }
    }

    // ---- CROSSOVER HPF ----
    else if (strcmp(cmd, "hpf") == 0) {
        int ch = doc["ch"] | 0;
        if (ch >= 0 && ch < NUM_OUTPUTS) {
            OutputChannel_t* out = (OutputChannel_t*)&g_outputs[ch];
            if (doc.containsKey("en"))    out->hpf.enabled = doc["en"] | false;
            if (doc.containsKey("freq"))  out->hpf.freq = doc["freq"] | 20.0f;
            if (doc.containsKey("type"))  out->hpf.type = (CrossoverType_t)(doc["type"] | 0);
            if (doc.containsKey("slope")) out->hpf.slope = (CrossoverSlope_t)(doc["slope"] | 4);
            AudioDSP_RecalcCrossover(ch);
        }
    }

    // ---- CROSSOVER LPF ----
    else if (strcmp(cmd, "lpf") == 0) {
        int ch = doc["ch"] | 0;
        if (ch >= 0 && ch < NUM_OUTPUTS) {
            OutputChannel_t* out = (OutputChannel_t*)&g_outputs[ch];
            if (doc.containsKey("en"))    out->lpf.enabled = doc["en"] | false;
            if (doc.containsKey("freq"))  out->lpf.freq = doc["freq"] | 20000.0f;
            if (doc.containsKey("type"))  out->lpf.type = (CrossoverType_t)(doc["type"] | 0);
            if (doc.containsKey("slope")) out->lpf.slope = (CrossoverSlope_t)(doc["slope"] | 4);
            AudioDSP_RecalcCrossover(ch);
        }
    }

    // ---- INPUT PEQ ----
    else if (strcmp(cmd, "inputPEQ") == 0) {
        int ch = doc["ch"] | 0;
        int band = doc["band"] | 0;
        if (ch >= 0 && ch < NUM_INPUTS && band >= 0 && band < INPUT_PEQ_BANDS) {
            PEQBand_t* b = (PEQBand_t*)&g_inputs[ch].peq[band];
            if (doc.containsKey("en"))   b->enabled = doc["en"] | false;
            if (doc.containsKey("type")) b->type = (FilterType_t)(doc["type"] | 0);
            if (doc.containsKey("freq")) b->freq = doc["freq"] | 1000.0f;
            if (doc.containsKey("gain")) b->gain = doc["gain"] | 0.0f;
            if (doc.containsKey("q"))    b->q = doc["q"] | 1.0f;
            AudioDSP_RecalcInputPEQ(ch);
        }
    }

    // ---- OUTPUT PEQ ----
    else if (strcmp(cmd, "outputPEQ") == 0) {
        int ch = doc["ch"] | 0;
        int band = doc["band"] | 0;
        if (ch >= 0 && ch < NUM_OUTPUTS && band >= 0 && band < OUTPUT_PEQ_BANDS) {
            PEQBand_t* b = (PEQBand_t*)&g_outputs[ch].peq[band];
            if (doc.containsKey("en"))   b->enabled = doc["en"] | false;
            if (doc.containsKey("type")) b->type = (FilterType_t)(doc["type"] | 0);
            if (doc.containsKey("freq")) b->freq = doc["freq"] | 1000.0f;
            if (doc.containsKey("gain")) b->gain = doc["gain"] | 0.0f;
            if (doc.containsKey("q"))    b->q = doc["q"] | 1.0f;
            AudioDSP_RecalcOutputPEQ(ch);
        }
    }

    // ---- DELAY ----
    else if (strcmp(cmd, "delay") == 0) {
        int ch = doc["ch"] | 0;
        float ms = doc["val"] | 0.0f;
        if (ch >= 0 && ch < NUM_OUTPUTS) {
            OutputChannel_t* out = (OutputChannel_t*)&g_outputs[ch];
            out->delay.delayMs = ms;
            out->delay.delaySamples = ms * (float)SAMPLE_RATE / 1000.0f;
        }
    }

    // ---- PHASE INVERT ----
    else if (strcmp(cmd, "phase") == 0) {
        const char* ch = doc["ch"];
        bool state = doc["val"] | false;
        if (strcmp(ch, "in1") == 0)       g_inputs[0].phaseInvert = state;
        else if (strcmp(ch, "in2") == 0)  g_inputs[1].phaseInvert = state;
        else if (strcmp(ch, "outA") == 0) g_outputs[0].phaseInvert = state;
        else if (strcmp(ch, "outB") == 0) g_outputs[1].phaseInvert = state;
        else if (strcmp(ch, "outC") == 0) g_outputs[2].phaseInvert = state;
        else if (strcmp(ch, "outD") == 0) g_outputs[3].phaseInvert = state;
    }

    // ---- LIMITER ----
    else if (strcmp(cmd, "limiter") == 0) {
        int ch = doc["ch"] | 0;
        if (ch >= 0 && ch < NUM_OUTPUTS) {
            PeakLimiter_t* l = (PeakLimiter_t*)&g_outputs[ch].limiter;
            if (doc.containsKey("en"))      l->enabled = doc["en"] | false;
            if (doc.containsKey("thresh"))  l->threshold = doc["thresh"] | -2.0f;
            if (doc.containsKey("attack"))  l->attackMs = doc["attack"] | 10.0f;
            if (doc.containsKey("release")) l->releaseMs = doc["release"] | 100.0f;
            AudioDSP_RecalcLimiter(ch);
        }
    }

    // ---- FIR FILTER: Enable/Disable ----
    else if (strcmp(cmd, "fir") == 0) {
        bool isOutput = doc["isOutput"] | true;
        int ch = doc["ch"] | 0;
        if (doc.containsKey("en")) {
            if (isOutput && ch >= 0 && ch < NUM_OUTPUTS) {
                g_outputs[ch].fir.enabled = doc["en"] | false;
            } else if (!isOutput && ch >= 0 && ch < NUM_INPUTS) {
                g_inputs[ch].fir.enabled = doc["en"] | false;
            }
        }
    }

    // ---- FIR FILTER: Upload Coefficients ----
    else if (strcmp(cmd, "firCoeffs") == 0) {
        bool isOutput = doc["isOutput"] | true;
        int ch = doc["ch"] | 0;
        JsonArray coeffArr = doc["coeffs"];
        int numTaps = coeffArr.size();
        if (numTaps > FIR_MAX_TAPS) numTaps = FIR_MAX_TAPS;

        float tempCoeffs[FIR_MAX_TAPS];
        for (int t = 0; t < numTaps; t++) {
            tempCoeffs[t] = coeffArr[t] | 0.0f;
        }

        if (isOutput && ch >= 0 && ch < NUM_OUTPUTS) {
            FIR_SetCoeffs((FIRFilter_t*)&g_outputs[ch].fir, tempCoeffs, numTaps);
            g_outputs[ch].fir.enabled = true;
            Serial.printf("[WS] FIR loaded: OUT%c, %d taps\n", 'A' + ch, numTaps);
        } else if (!isOutput && ch >= 0 && ch < NUM_INPUTS) {
            FIR_SetCoeffs((FIRFilter_t*)&g_inputs[ch].fir, tempCoeffs, numTaps);
            g_inputs[ch].fir.enabled = true;
            Serial.printf("[WS] FIR loaded: IN%d, %d taps\n", ch + 1, numTaps);
        }
    }

    // ---- PRESET SAVE ----
    else if (strcmp(cmd, "presetSave") == 0) {
        int slot = doc["slot"] | 0;
        Preset_Save(slot);
    }

    // ---- PRESET LOAD ----
    else if (strcmp(cmd, "presetLoad") == 0) {
        int slot = doc["slot"] | 0;
        Preset_Load(slot);
        WebServerInterface_BroadcastState();
    }

    // ---- GET FULL STATE ----
    else if (strcmp(cmd, "getState") == 0) {
        // Client requests the current DSP state (on connection)
        String json = Preset_ExportJSON();
        webSocket.sendTXT(clientNum, "{\"type\":\"state\",\"data\":" + json + "}");
    }

    // ---- PRESET EXPORT ----
    else if (strcmp(cmd, "presetExport") == 0) {
        String json = Preset_ExportJSON();
        webSocket.sendTXT(clientNum, "{\"type\":\"presetData\",\"data\":" + json + "}");
    }

    // ---- PRESET IMPORT ----
    else if (strcmp(cmd, "presetImport") == 0) {
        String data;
        serializeJson(doc["data"], data);
        Preset_ImportJSON(data);
        WebServerInterface_BroadcastState();
    }
}

// ---------------------------------------------------------
// WEBSOCKET: Event handler
// ---------------------------------------------------------
static void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            Serial.printf("[WS] Client #%u disconnected\n", num);
            break;

        case WStype_CONNECTED: {
            IPAddress ip = webSocket.remoteIP(num);
            Serial.printf("[WS] Client #%u connected from %s\n", num, ip.toString().c_str());

            // Do NOT send DSP state yet - client must authenticate first
            webSocket.sendTXT(num, "{\"type\":\"authRequired\",\"msg\":\"Please login\"}");
            break;
        }

        case WStype_TEXT: {
            String payloadStr = String((char*)payload);
            handleWSCommand(num, payloadStr);
            break;
        }

        case WStype_BIN:
            // Binary mode could be used for FIR coefficient bulk upload
            // For now, we use JSON text mode
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------
// BROADCAST: Send VU meter data to all clients
// ---------------------------------------------------------
static void broadcastVUMeters() {
    if (webSocket.connectedClients() == 0) return;

    // Compact JSON for VU data (sent frequently)
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"vu\","
        "\"in1\":%.0f,\"in2\":%.0f,"
        "\"outA\":%.0f,\"outB\":%.0f,\"outC\":%.0f,\"outD\":%.0f,"
        "\"in1dB\":%.1f,\"in2dB\":%.1f,"
        "\"oAdB\":%.1f,\"oBdB\":%.1f,\"oCdB\":%.1f,\"oDdB\":%.1f,"
        "\"clip1\":%d,\"clip2\":%d,"
        "\"gr\":[%.1f,%.1f,%.1f,%.1f]}",
        g_inputs[0].vu.peakLevel * 100.0f,
        g_inputs[1].vu.peakLevel * 100.0f,
        g_outputs[0].vu.peakLevel * 100.0f,
        g_outputs[1].vu.peakLevel * 100.0f,
        g_outputs[2].vu.peakLevel * 100.0f,
        g_outputs[3].vu.peakLevel * 100.0f,
        g_inputs[0].vu.peakDb,
        g_inputs[1].vu.peakDb,
        g_outputs[0].vu.peakDb,
        g_outputs[1].vu.peakDb,
        g_outputs[2].vu.peakDb,
        g_outputs[3].vu.peakDb,
        (int)g_inputs[0].vu.clip,
        (int)g_inputs[1].vu.clip,
        g_outputs[0].limiter.gainReduction,
        g_outputs[1].limiter.gainReduction,
        g_outputs[2].limiter.gainReduction,
        g_outputs[3].limiter.gainReduction
    );

    webSocket.broadcastTXT(buf);
}

// ---------------------------------------------------------
// BROADCAST: Full state push to all clients
// ---------------------------------------------------------
void WebServerInterface_BroadcastState() {
    if (webSocket.connectedClients() == 0) return;
    String json = Preset_ExportJSON();
    webSocket.broadcastTXT("{\"type\":\"state\",\"data\":" + json + "}");
}

// ---------------------------------------------------------
// INIT
// ---------------------------------------------------------
void WebServerInterface_Init() {
    // Start Wi-Fi Access Point
    WiFi.softAP(WIFI_SSID, WIFI_PASS);
    WiFi.setHostname(HOSTNAME);
    Serial.print("[WEB] AP IP address: ");
    Serial.println(WiFi.softAPIP());

    // Serve the embedded GZIP Web Dashboard from PROGMEM
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse_P(
            200, "text/html", index_html_gz, index_html_gz_len);
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=86400");
        request->send(response);
    });

    // REST API: Login endpoint (returns session token)
    server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest* request) {},
        NULL,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            String body = String((char*)data).substring(0, len);
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, body);
            if (err) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            String password = doc["password"] | "";
            if (Auth_ValidatePassword(password)) {
                String token = Auth_CreateSession();
                char buf[128];
                snprintf(buf, sizeof(buf), "{\"token\":\"%s\"}", token.c_str());
                request->send(200, "application/json", buf);
            } else {
                request->send(401, "application/json", "{\"error\":\"Invalid password\"}");
            }
        }
    );

    // REST API: Export preset as JSON download (requires auth token header)
    server.on("/api/preset/export", HTTP_GET, [](AsyncWebServerRequest* request) {
        // Check auth via query param or header
        String token = "";
        if (request->hasHeader("X-Auth-Token")) {
            token = request->header("X-Auth-Token");
        } else if (request->hasParam("token")) {
            token = request->getParam("token")->value();
        }
        if (!Auth_ValidateSession(token)) {
            request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
            return;
        }
        String json = Preset_ExportJSON();
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        response->addHeader("Content-Disposition", "attachment; filename=\"dlms_preset.json\"");
        request->send(response);
    });

    // REST API: Get system info
    server.on("/api/system", HTTP_GET, [](AsyncWebServerRequest* request) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"firmware\":\"DLMS 2.4\","
            "\"sampleRate\":%d,"
            "\"bufferSize\":%d,"
            "\"freeHeap\":%u,"
            "\"freePSRAM\":%u,"
            "\"uptime\":%lu,"
            "\"firMaxTaps\":%d,"
            "\"inputPEQ\":%d,"
            "\"outputPEQ\":%d}",
            SAMPLE_RATE, I2S_BUFFER_SIZE,
            ESP.getFreeHeap(), ESP.getFreePsram(),
            millis() / 1000,
            FIR_MAX_TAPS, INPUT_PEQ_BANDS, OUTPUT_PEQ_BANDS);
        request->send(200, "application/json", buf);
    });

    server.begin();
    Serial.println("[WEB] HTTP server started on port 80");

    // Start WebSocket server
    webSocket.begin();
    webSocket.onEvent(onWebSocketEvent);
    Serial.println("[WEB] WebSocket server started on port 81");
}

// ---------------------------------------------------------
// UPDATE (called at ~33Hz from Core 0 task)
// ---------------------------------------------------------
void WebServerInterface_Update() {
    webSocket.loop();

    // Periodic VU meter broadcast
    unsigned long now = millis();
    if ((now - lastVuBroadcast) >= VU_BROADCAST_MS) {
        lastVuBroadcast = now;
        broadcastVUMeters();
    }
}
