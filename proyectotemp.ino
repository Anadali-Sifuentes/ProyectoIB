#include <Wire.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Adafruit_MLX90614.h>

// ============ CONFIGURACIÓN WiFi ============
const char* ssid = "TecNM-ITT";
const char* password = "";

// ============ CONFIGURACIÓN WebSocket ============
const char* websocket_server = "172.16.65.222";
const int websocket_port = 3000;
const char* websocket_path = "/ws";

// ============ CONFIGURACIÓN I2C ============
#define I2C_SDA 21  // Cambia si usas otros pines
#define I2C_SCL 22  // Cambia si usas otros pines
#define I2C_FREQ 100000  // 100kHz - más estable

// ============ CONFIGURACIÓN SENSOR ============
#define REPORTING_PERIOD_MS 2000
#define SEND_PERIOD_MS 2000

Adafruit_MLX90614 mlx = Adafruit_MLX90614();
WebSocketsClient webSocket;

// Variables compartidas entre cores
volatile float sharedTempObjeto = 0;
volatile float sharedTempAmbiente = 0;
volatile bool dataReady = false;
volatile bool sensorConnected = false;
SemaphoreHandle_t dataMutex;

bool wsConnected = false;
uint32_t tsLastReport = 0;
uint32_t sensorRetryTime = 0;

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
        doc["deviceType"] = "temperatura";
        doc["deviceId"] = "MLX90614-001";
        
        String output;
        serializeJson(doc, output);
        webSocket.sendTXT(output);
        Serial.println("📡 Identificación enviada");
      }
      break;
      
    case WStype_TEXT:
      Serial.printf("📨 Mensaje: %s\n", payload);
      break;
      
    case WStype_ERROR:
      Serial.println("⚠️ Error WebSocket");
      break;
  }
}

// ============ FUNCIÓN: ESCANEAR I2C ============
void scanI2C() {
  Serial.println("\n🔍 Escaneando bus I2C...");
  byte error, address;
  int nDevices = 0;

  for(address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("✅ Dispositivo I2C encontrado en 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
      nDevices++;
    }
  }
  
  if (nDevices == 0) {
    Serial.println("❌ No se encontraron dispositivos I2C");
    Serial.println("⚠️  Verifica las conexiones:");
    Serial.println("   - SDA → Pin 21");
    Serial.println("   - SCL → Pin 22");
    Serial.println("   - VCC → 3.3V");
    Serial.println("   - GND → GND");
  } else {
    Serial.printf("✅ Total: %d dispositivo(s) encontrado(s)\n", nDevices);
  }
  Serial.println();
}

// ============ FUNCIÓN: REINTENTAR CONEXIÓN SENSOR ============
bool tryConnectSensor() {
  Serial.println("🔄 Intentando conectar con MLX90614...");
  
  // Reiniciar I2C
  Wire.end();
  delay(100);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_FREQ);
  delay(200);
  
  // Intentar inicializar sensor
  if (mlx.begin()) {
    Serial.println("✅ MLX90614 conectado exitosamente!");
    return true;
  } else {
    Serial.println("❌ MLX90614 no responde");
    return false;
  }
}

// ============ TAREA CORE 0: SOLO SENSOR ============
void sensorTask(void * parameter) {
  Serial.println("🔧 [CORE 0] Iniciando sensor...");
  
  // Inicializar I2C en este core
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_FREQ);
  delay(200);
  
  // Escanear bus I2C
  scanI2C();
  
  // Intentar conectar sensor
  sensorConnected = tryConnectSensor();
  
  if (!sensorConnected) {
    Serial.println("⚠️ [CORE 0] Iniciando sin sensor - se reintentará cada 10s");
  }
  
  // Loop infinito del sensor
  while(true) {
    // Si el sensor no está conectado, reintentar cada 10 segundos
    if (!sensorConnected && (millis() - sensorRetryTime > 10000)) {
      sensorRetryTime = millis();
      sensorConnected = tryConnectSensor();
    }
    
    // Si el sensor está conectado, leer datos
    if (sensorConnected && (millis() - tsLastReport > REPORTING_PERIOD_MS)) {
      tsLastReport = millis();
      
      float tempObjeto = mlx.readObjectTempC();
      float tempAmbiente = mlx.readAmbientTempC();
      
      // Verificar si las lecturas son válidas
      if (!isnan(tempObjeto) && !isnan(tempAmbiente)) {
        // Actualizar variables compartidas de forma segura
        if (xSemaphoreTake(dataMutex, portMAX_DELAY)) {
          sharedTempObjeto = tempObjeto;
          sharedTempAmbiente = tempAmbiente;
          dataReady = true;
          xSemaphoreGive(dataMutex);
        }
        
        // Mostrar en serial
        Serial.print("[SENSOR] Objeto: ");
        Serial.print(tempObjeto, 1);
        Serial.print("°C | Ambiente: ");
        Serial.print(tempAmbiente, 1);
        Serial.println("°C");
      } else {
        Serial.println("⚠️ [SENSOR] Lectura inválida (NaN) - posible desconexión");
        sensorConnected = false;
      }
    }
    
    vTaskDelay(10 / portTICK_PERIOD_MS);
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
  
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("✅ [CORE 1] WiFi conectado!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    
    // Iniciar WebSocket
    Serial.println("🔌 [CORE 1] Iniciando WebSocket...");
    webSocket.begin(websocket_server, websocket_port, websocket_path);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
    webSocket.enableHeartbeat(15000, 3000, 2);
  } else {
    Serial.println("❌ [CORE 1] Sin WiFi");
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
      float tempObj, tempAmb;
      bool hasSensorData = false;
      
      if (xSemaphoreTake(dataMutex, 100 / portTICK_PERIOD_MS)) {
        if (dataReady) {
          tempObj = sharedTempObjeto;
          tempAmb = sharedTempAmbiente;
          dataReady = false;
          hasSensorData = true;
        }
        xSemaphoreGive(dataMutex);
      }
      
      // Enviar estado aunque no haya datos del sensor
      StaticJsonDocument<300> doc;
      doc["type"] = "sensor-data";
      doc["deviceId"] = "MLX90614-001";
      doc["timestamp"] = millis();
      doc["sensorConnected"] = sensorConnected;
      
      if (hasSensorData && sensorConnected) {
        doc["temperatura"] = tempObj;
        doc["ambiente"] = tempAmb;
        
        String output;
        serializeJson(doc, output);
        webSocket.sendTXT(output);
        
        Serial.print("📤 [ENVIADO] Temp: ");
        Serial.print(tempObj, 1);
        Serial.print("°C | Ambiente: ");
        Serial.print(tempAmb, 1);
        Serial.println("°C");
      } else {
        doc["temperatura"] = nullptr;
        doc["ambiente"] = nullptr;
        doc["error"] = "Sensor desconectado";
        
        String output;
        serializeJson(doc, output);
        webSocket.sendTXT(output);
        
        Serial.println("📤 [ENVIADO] Estado: Sensor desconectado");
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
  Serial.println("║   MLX90614 Dual Core + WebSocket     ║");
  Serial.println("║        Con detección de errores      ║");
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