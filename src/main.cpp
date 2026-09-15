#include <Arduino.h>
#include "Config.h"
#include "AudioDSP.h"
#include "DisplayUI.h"
#include "HardwareControl.h"
#include "WebServerInterface.h"

// Task Handles
TaskHandle_t TaskDSPHandle;
TaskHandle_t TaskUIHandle;

// Task Core 1: Real-Time Audio DSP
void TaskDSP(void *pvParameters) {
    Serial.println("Task DSP running on Core 1");
    AudioDSP_Init();
    
    for (;;) {
        // This loop should ideally be driven by I2S DMA interrupts
        // or block on I2S read/write.
        AudioDSP_Process();
        
        // Yield to let watchdog and other critical things run if necessary
        // but normally I2S block will yield automatically.
    }
}

// Task Core 0: Web Server, UI, and Hardware Inputs
void TaskUI(void *pvParameters) {
    Serial.println("Task UI/Web running on Core 0");
    
    DisplayUI_Init();
    HardwareControl_Init();
    WebServerInterface_Init();
    
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(30); // ~33Hz refresh rate
    
    for (;;) {
        HardwareControl_Update();
        DisplayUI_Update();
        WebServerInterface_Update();
        
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("DLMS 2.4 Booting...");

    // Create DSP Task on Core 1 (High Priority)
    xTaskCreatePinnedToCore(
        TaskDSP,
        "TaskDSP",
        16384, // Stack: FIR convolution + delay buffer ops
        NULL,
        configMAX_PRIORITIES - 1, // Priority (Highest)
        &TaskDSPHandle,
        1 // Core 1
    );

    // Create UI/Web Task on Core 0 (Normal Priority)
    xTaskCreatePinnedToCore(
        TaskUI,
        "TaskUI",
        32768, // Stack: JSON serialization + WebSocket + TFT rendering
        NULL,
        1, // Priority
        &TaskUIHandle,
        0 // Core 0
    );
}

void loop() {
    // Empty loop, FreeRTOS tasks handle everything
    vTaskDelete(NULL);
}
