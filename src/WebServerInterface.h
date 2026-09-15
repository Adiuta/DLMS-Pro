#ifndef WEB_SERVER_INTERFACE_H
#define WEB_SERVER_INTERFACE_H

#include <Arduino.h>

void WebServerInterface_Init();
void WebServerInterface_Update();

// Push full state to all connected WebSocket clients
void WebServerInterface_BroadcastState();

#endif // WEB_SERVER_INTERFACE_H
