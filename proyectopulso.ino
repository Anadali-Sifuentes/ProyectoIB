#include <Wire.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "MAX30100_PulseOximeter.h"

// ============ CONFIGURACIÓN WiFi ============
const char* ssid = "TecNM-ITT";
const char* password = "";

// ============ CONFIGURACIÓN WebSocket ============
const char* websocket_server = "172.16.65.222";
const int websocket_port = 3000;
const char* websocket_path = "/ws";

// ============ CONFIGURACIÓN SENSOR ============
#define REPORTING_PERIOD_MS 1000
#define SEND_PERIOD_MS 2000

PulseOximeter pox;
WebSocketsClient webSocket;

// Variables compartidas entre cores (con protección)
volatile float sharedBPM = 0;
volatile float sharedSpO2 = 0;
volatile bool dataReady = false;
SemaphoreHandle_t dataMutex;

bool wsConnected = false;
uint32_t tsLastReport = 0;

void onBeatDetected() {
  Serial.println("💓 Latido!");
}

// ============ EVENTO WEBSOCKET ============
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.println("❌ WebSocket Desconectado");
      wsConnected = false;
      break;
      
    case WStype_CONNECTED:
      Serial.printf("✅ WebSocket Conectado a: %s\n", payload);
      wsConnected = true;
      
      {
        StaticJsonDocument<200> doc;
        doc["type"] = "device";
        doc["deviceType"] = "pulso";
        doc["deviceId"] = "MAX30100-001";
        
        String output;
        serializeJson(doc, output);
        webSocket.sendTXT(output);
        Serial.println("📡 Identificación enviada");
      }
      break;
      
    case WStype_TEXT:
      Serial.printf("📨 Mensaje: %s\n", payload);
      break;
  }
}

// ============ TAREA CORE 0: SOLO SENSOR ============
void sensorTask(void * parameter) {
  Serial.println("🔧 [CORE 0] Iniciando sensor...");
  
  // Inicializar I2C en este core
  Wire.begin(21, 22);
  Wire.setClock(100000);
  delay(100);
  
  if (!pox.begin()) {
    Serial.println("❌ [CORE 0] Error al iniciar MAX30100");
    vTaskDelete(NULL);
    return;
  }
  
  Serial.println("✅ [CORE 0] MAX30100 listo");
  pox.setIRLedCurrent(MAX30100_LED_CURR_20_8MA);
  pox.setOnBeatDetectedCallback(onBeatDetected);
  
  // Loop infinito del sensor
  while(true) {
    pox.update();
    
    if (millis() - tsLastReport > REPORTING_PERIOD_MS) {
      tsLastReport = millis();
      
      float bpm = pox.getHeartRate();
      float spo2 = pox.getSpO2();
      
      // Actualizar variables compartidas de forma segura
      if (xSemaphoreTake(dataMutex, portMAX_DELAY)) {
        sharedBPM = bpm;
        sharedSpO2 = spo2;
        dataReady = true;
        xSemaphoreGive(dataMutex);
      }
      
      // Mostrar en serial
      Serial.print("[SENSOR] BPM: ");
      Serial.print(bpm);
      Serial.print(" | SpO2: ");
      Serial.print(spo2);
      Serial.println("%");
    }
    
    vTaskDelay(10 / portTICK_PERIOD_MS); // Delay mínimo
  }
}

// ============ TAREA CORE 1: WiFi y WebSocket ============
void networkTask(void * parameter) {
  delay(2000); // Esperar que el sensor se inicie
  
  Serial.println("🌐 [CORE 1] Conectando WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ [CORE 1] WiFi conectado!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    
    // Iniciar WebSocket
    webSocket.begin(websocket_server, websocket_port, websocket_path);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
    webSocket.enableHeartbeat(15000, 3000, 2);
  } else {
    Serial.println("\n❌ [CORE 1] Sin WiFi");
    vTaskDelete(NULL);
    return;
  }
  
  unsigned long lastSend = 0;
  
  // Loop infinito de red
  while(true) {
    webSocket.loop();
    
    // Enviar datos cada 2 segundos
    if (millis() - lastSend > SEND_PERIOD_MS && wsConnected) {
      lastSend = millis();
      
      // Leer datos compartidos
      float bpm, spo2;
      if (xSemaphoreTake(dataMutex, 100 / portTICK_PERIOD_MS)) {
        if (dataReady) {
          bpm = sharedBPM;
          spo2 = sharedSpO2;
          dataReady = false;
          xSemaphoreGive(dataMutex);
          
          // Enviar solo si hay datos válidos
          if (bpm > 0 || spo2 > 0) {
            StaticJsonDocument<300> doc;
            doc["type"] = "sensor-data";
            doc["pulso"] = bpm;
            doc["spo2"] = spo2;
            doc["timestamp"] = millis();
            doc["deviceId"] = "MAX30100-001";
            
            String output;
            serializeJson(doc, output);
            webSocket.sendTXT(output);
            
            Serial.print("📤 [ENVIADO] BPM: ");
            Serial.print(bpm);
            Serial.print(" | SpO2: ");
            Serial.print(spo2);
            Serial.println("%");
          }
        } else {
          xSemaphoreGive(dataMutex);
        }
      }
    }
    
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n╔═══════════════════════════════════════╗");
  Serial.println("║   MAX30100 Dual Core + WebSocket     ║");
  Serial.println("╚═══════════════════════════════════════╝\n");
  
  // Crear mutex para proteger datos compartidos
  dataMutex = xSemaphoreCreateMutex();
  
  // CORE 0: Sensor (prioridad alta)
  xTaskCreatePinnedToCore(
    sensorTask,      // Función
    "SensorTask",    // Nombre
    4096,            // Stack
    NULL,            // Parámetro
    2,               // Prioridad alta
    NULL,            // Handle
    0                // CORE 0
  );
  
  // CORE 1: Red (prioridad normal)
  xTaskCreatePinnedToCore(
    networkTask,     // Función
    "NetworkTask",   // Nombre
    8192,            // Stack
    NULL,            // Parámetro
    1,               // Prioridad normal
    NULL,            // Handle
    1                // CORE 1
  );
  
  Serial.println("✅ Sistema iniciado en modo dual-core");
}

// ============ LOOP ============
void loop() {
  // Loop vacío - todo lo manejan las tareas
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}