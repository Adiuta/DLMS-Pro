#ifndef HARDWARE_CONTROL_H
#define HARDWARE_CONTROL_H

#include <Arduino.h>

void HardwareControl_Init();
void HardwareControl_Update();

// Preset management
void Preset_Save(int slot);
void Preset_Load(int slot);
void Preset_SetName(int slot, const char* name);
const char* Preset_GetName(int slot);
int Preset_GetActive();

// JSON export/import
String Preset_ExportJSON();
bool Preset_ImportJSON(const String& json);

// ---------------------------------------------------------
// AUTHENTICATION API
// ---------------------------------------------------------
void Auth_Init();
bool Auth_ValidatePassword(const String& password);
bool Auth_ChangePassword(const String& oldPass, const String& newPass);
void Auth_ResetToDefault();                  // Called by hardware long-press
String Auth_CreateSession();                 // Returns new session token
bool Auth_ValidateSession(const String& token);
void Auth_DestroySession(const String& token);
void Auth_CleanExpiredSessions();

#endif // HARDWARE_CONTROL_H
