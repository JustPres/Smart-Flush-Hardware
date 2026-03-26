// ============================================================
// Smart Flush System — Full State Machine Firmware
// Justine M. Lopez | BSIT 3A | SDCA Capstone 2026
// UPDATE WIFI AND MQTT CREDENTIALS BEFORE FLASHING
// ============================================================

// ── 1. Includes + Defines ────────────────────────────────────
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

// ── 2. Credentials ───────────────────────────────────────────
#define WIFI_SSID        
#define WIFI_PASSWORD    
#define MQTT_BROKER     
#define MQTT_PORT       
#define MQTT_USER      
#define MQTT_PASS        

// ── 3. Pin Definitions ───────────────────────────────────────
#define TRIG_PIN        13
#define ECHO_PIN        12
#define PUMP_PIN        14
#define UV_PIN          27
#define SERVO1_PIN      25
#define SERVO2_PIN      26
#define FLOW_PIN        32
#define LED_PIN         2

// ── 4. Config Parameters ─────────────────────────────────────
int DETECTION_THRESHOLD_CM = 30;
int PUMP_DURATION_MS       = 3000;
int UV_DURATION_MS         = 5000;
int PERSON_GONE_CONFIRM_MS = 3000;

// ── 5. State Enum ─────────────────────────────────────────────
enum State {
  STANDBY,
  PERSON_DETECTED,
  LID_OPEN,
  WAITING_FOR_DEPARTURE,
  LID_CLOSING,
  FLUSHING,
  UV_ACTIVE
};

State currentState = STANDBY;

// ── 6. Global Variables ───────────────────────────────────────
Servo servo1;
Servo servo2;

volatile int pulseCount  = 0;
float totalVolume        = 0;
float flushDuration      = 0;

unsigned long lastUltrasonicPublish = 0;
unsigned long lastDistanceTrigger   = 0;
unsigned long lastReconnectAttempt  = 0;
unsigned long lastLedBlink          = 0;
unsigned long personGoneTimer       = 0;
unsigned long pumpStartTime         = 0;
unsigned long uvStartTime           = 0;
unsigned long flushStartTime        = 0;

float distanceBuffer[5] = {0, 0, 0, 0, 0};
int distanceIndex       = 0;
bool ledState           = false;

WiFiClientSecure espClient;
PubSubClient client(espClient);

// ── 7. Flow Sensor ISR ────────────────────────────────────────
void IRAM_ATTR pulseCounter() {
  pulseCount++;
}

// ── 8. WiFi ───────────────────────────────────────────────────
void connectWiFi() {
  Serial.printf("[%lu] [WIFI] Connecting to %s...\n", millis(), WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start >= 20000) {
      Serial.printf("[%lu] [WIFI] Timeout!\n", millis());
      return;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[%lu] [WIFI] Connected! IP: %s\n", millis(),
    WiFi.localIP().toString().c_str());
}

// ── 9. MQTT Callback ──────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) message += (char)payload[i];
  Serial.printf("[%lu] [MQTT] Received [%s]: %s\n", millis(), topic, message.c_str());

  if (String(topic) == "toilet/commands/pump") {
    digitalWrite(PUMP_PIN, message == "ON" ? LOW : HIGH);
    Serial.printf("[%lu] [PUMP] Manual %s\n", millis(), message.c_str());
  }

  if (String(topic) == "toilet/commands/uv") {
    digitalWrite(UV_PIN, message == "ON" ? LOW : HIGH);
    Serial.printf("[%lu] [UV] Manual %s\n", millis(), message.c_str());
  }

  if (String(topic) == "toilet/commands/lid") {
    if (message == "OPEN")  openLid();
    if (message == "CLOSE") closeLid();
  }

  if (String(topic) == "toilet/commands/config") {
    StaticJsonDocument<200> doc;
    deserializeJson(doc, message);
    if (doc.containsKey("pumpDuration"))
      PUMP_DURATION_MS = (int)doc["pumpDuration"] * 1000;
    if (doc.containsKey("uvDuration"))
      UV_DURATION_MS = (int)doc["uvDuration"] * 1000;
    if (doc.containsKey("threshold"))
      DETECTION_THRESHOLD_CM = (int)doc["threshold"];
    Serial.printf("[%lu] [CONFIG] pump:%dms uv:%dms threshold:%dcm\n",
      millis(), PUMP_DURATION_MS, UV_DURATION_MS, DETECTION_THRESHOLD_CM);
  }
}

// ── 10. MQTT Connect ──────────────────────────────────────────
bool connectMQTT() {
  Serial.printf("[%lu] [MQTT] Connecting...\n", millis());
  if (client.connect("ESP32SmartFlush", MQTT_USER, MQTT_PASS)) {
    Serial.printf("[%lu] [MQTT] Connected!\n", millis());
    client.subscribe("toilet/commands/pump");
    client.subscribe("toilet/commands/uv");
    client.subscribe("toilet/commands/lid");
    client.subscribe("toilet/commands/config");
    digitalWrite(LED_PIN, HIGH);
    return true;
  }
  Serial.printf("[%lu] [MQTT] Failed rc=%d\n", millis(), client.state());
  return false;
}

void reconnectMQTT() {
  if (!client.connected() && millis() - lastReconnectAttempt >= 5000) {
    lastReconnectAttempt = millis();
    connectMQTT();
  }
}

// ── 11. Servo Functions (continuous rotation, time‑based) ────
#define SPEED_OPEN   0      // full speed one direction
#define SPEED_CLOSE  180    // full speed opposite direction
#define OPEN_TIME    1600   // milliseconds to fully open
#define CLOSE_TIME   1300   // milliseconds to fully close
#define STOP_POS     92     // calibrated neutral stop

void openLid() {
  Serial.printf("[%lu] [LID] Opening...\n", millis());
  servo1.attach(SERVO1_PIN);
  servo2.attach(SERVO2_PIN);
  delay(100);               // allow attach to settle

  // Rotate both servos at full speed in the open direction
  servo1.write(SPEED_OPEN);
  servo2.write(SPEED_OPEN);
  delay(OPEN_TIME);

  // Stop both servos
  servo1.write(STOP_POS);
  servo2.write(STOP_POS);
  delay(500);               // extra time to stop completely

  servo1.detach();
  servo2.detach();

  // Publish lid open event
  StaticJsonDocument<100> doc;
  doc["status"]    = "open";
  doc["timestamp"] = millis();
  char buffer[100];
  serializeJson(doc, buffer);
  client.publish("toilet/events/lid", buffer);
  Serial.printf("[%lu] [LID] Open — event published\n", millis());
}

void closeLid() {
  Serial.printf("[%lu] [LID] Closing...\n", millis());
  servo1.attach(SERVO1_PIN);
  servo2.attach(SERVO2_PIN);
  delay(100);

  // Rotate both servos at full speed in the close direction
  servo1.write(SPEED_CLOSE);
  servo2.write(SPEED_CLOSE);
  delay(CLOSE_TIME);

  // Stop both servos
  servo1.write(STOP_POS);
  servo2.write(STOP_POS);
  delay(500);

  servo1.detach();
  servo2.detach();

  // Publish lid closed event
  StaticJsonDocument<100> doc;
  doc["status"]    = "closed";
  doc["timestamp"] = millis();
  char buffer[100];
  serializeJson(doc, buffer);
  client.publish("toilet/events/lid", buffer);
  Serial.printf("[%lu] [LID] Closed — event published\n", millis());
}

// ── 12. Distance Sensor ───────────────────────────────────────
float getDistance() {
  if (millis() - lastDistanceTrigger >= 200) {
    lastDistanceTrigger = millis();
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    long duration = pulseIn(ECHO_PIN, HIGH, 60000);
    float dist = duration / 58.0;
    if (dist > 0 && dist < 400) {
      distanceBuffer[distanceIndex % 5] = dist;
      distanceIndex++;
    }
  }
  // Median of last 5
  float sorted[5];
  memcpy(sorted, distanceBuffer, sizeof(sorted));
  for (int i = 0; i < 4; i++)
    for (int j = i + 1; j < 5; j++)
      if (sorted[i] > sorted[j]) {
        float tmp = sorted[i];
        sorted[i] = sorted[j];
        sorted[j] = tmp;
      }
  return sorted[2];
}

void publishDistance(float distance) {
  if (millis() - lastUltrasonicPublish >= 1000) {
    lastUltrasonicPublish = millis();
    StaticJsonDocument<100> doc;
    doc["distance"]  = distance;
    doc["unit"]      = "cm";
    doc["timestamp"] = millis();
    char buffer[100];
    serializeJson(doc, buffer);
    client.publish("toilet/sensors/ultrasonic", buffer);
    Serial.printf("[%lu] [SENSOR] Distance: %.1f cm\n", millis(), distance);
  }
}

// ── 13. LED Status ────────────────────────────────────────────
void updateLED() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastLedBlink >= 200) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
      lastLedBlink = millis();
    }
  } else if (!client.connected()) {
    if (millis() - lastLedBlink >= 1000) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
      lastLedBlink = millis();
    }
  } else {
    digitalWrite(LED_PIN, HIGH);
  }
}

// ── 14. State Machine ─────────────────────────────────────────
void updateStateMachine(float distance) {
  switch (currentState) {

    case STANDBY:
      if (distance > 0 && distance < DETECTION_THRESHOLD_CM) {
        currentState = PERSON_DETECTED;
        Serial.printf("[%lu] [STATE] STANDBY → PERSON_DETECTED\n", millis());
      }
      break;

    case PERSON_DETECTED:
      // Open lid completely before moving to next state
      openLid();
      currentState    = LID_OPEN;
      personGoneTimer = 0;
      Serial.printf("[%lu] [STATE] PERSON_DETECTED → LID_OPEN\n", millis());
      break;

    case LID_OPEN:
      // Lid is fully open — just wait for person to leave
      currentState = WAITING_FOR_DEPARTURE;
      Serial.printf("[%lu] [STATE] LID_OPEN → WAITING_FOR_DEPARTURE\n", millis());
      break;

    case WAITING_FOR_DEPARTURE:
      if (distance >= DETECTION_THRESHOLD_CM) {
        if (personGoneTimer == 0) {
          personGoneTimer = millis();
          Serial.printf("[%lu] [STATE] Person may have left — confirming...\n", millis());
        } else if (millis() - personGoneTimer >= PERSON_GONE_CONFIRM_MS) {
          personGoneTimer = 0;
          currentState    = LID_CLOSING;
          Serial.printf("[%lu] [STATE] WAITING_FOR_DEPARTURE → LID_CLOSING\n", millis());
        }
      } else {
        if (personGoneTimer != 0) {
          personGoneTimer = 0;
          Serial.printf("[%lu] [STATE] Person still present — resetting timer\n", millis());
        }
      }
      break;

    case LID_CLOSING:
      // Close lid COMPLETELY before pump starts
      closeLid();
      delay(500); // small pause after lid fully closed

      // Now start flush
      pulseCount     = 0;
      totalVolume    = 0;
      flushStartTime = millis();
      pumpStartTime  = millis();
      digitalWrite(PUMP_PIN, LOW);

      // Publish pump active
      {
        StaticJsonDocument<100> doc;
        doc["status"]    = "active";
        doc["timestamp"] = millis();
        char buffer[100];
        serializeJson(doc, buffer);
        client.publish("toilet/events/pump", buffer);
      }

      currentState = FLUSHING;
      Serial.printf("[%lu] [STATE] LID_CLOSING → FLUSHING\n", millis());
      Serial.printf("[%lu] [PUMP] ON\n", millis());
      break;

    case FLUSHING:
      if (millis() - pumpStartTime >= PUMP_DURATION_MS) {
        digitalWrite(PUMP_PIN, HIGH);
        flushDuration = (millis() - flushStartTime) / 1000.0;

        // Calculate volume
        noInterrupts();
        int pulses = pulseCount;
        pulseCount = 0;
        interrupts();
        float flowRate = (pulses / 7.5);
        totalVolume   += (flowRate / 60.0);

        // Publish waterflow
        {
          StaticJsonDocument<100> doc;
          doc["volume"]    = totalVolume;
          doc["duration"]  = flushDuration;
          doc["unit"]      = "L";
          char buffer[100];
          serializeJson(doc, buffer);
          client.publish("toilet/sensors/waterflow", buffer);
        }

        // Publish pump inactive
        {
          StaticJsonDocument<100> doc;
          doc["status"]    = "inactive";
          doc["timestamp"] = millis();
          char buffer[100];
          serializeJson(doc, buffer);
          client.publish("toilet/events/pump", buffer);
        }

        Serial.printf("[%lu] [PUMP] OFF — %.2f L in %.1f s\n",
          millis(), totalVolume, flushDuration);

        // Start UV
        digitalWrite(UV_PIN, LOW);
        uvStartTime  = millis();
        currentState = UV_ACTIVE;
        Serial.printf("[%lu] [STATE] FLUSHING → UV_ACTIVE\n", millis());
        Serial.printf("[%lu] [UV] ON\n", millis());
      }
      break;

    case UV_ACTIVE:
      if (millis() - uvStartTime >= UV_DURATION_MS) {
        digitalWrite(UV_PIN, HIGH);

        // Publish UV cycle
        {
          StaticJsonDocument<100> doc;
          doc["duration"]  = UV_DURATION_MS / 1000;
          doc["completed"] = true;
          doc["timestamp"] = millis();
          char buffer[100];
          serializeJson(doc, buffer);
          client.publish("toilet/events/uv", buffer);
        }

        Serial.printf("[%lu] [UV] OFF — cycle complete\n", millis());
        currentState = STANDBY;
        Serial.printf("[%lu] [STATE] UV_ACTIVE → STANDBY\n", millis());
        Serial.printf("[%lu] [SYSTEM] Ready for next person!\n", millis());
      }
      break;
  }
}

// ── 15. Setup ─────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(UV_PIN,   OUTPUT);
  pinMode(LED_PIN,  OUTPUT);

  digitalWrite(PUMP_PIN, HIGH);
  digitalWrite(UV_PIN,   HIGH);
  digitalWrite(LED_PIN,  LOW);

  servo1.detach();
  servo2.detach();

  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), pulseCounter, RISING);

  espClient.setInsecure();
  connectWiFi();
  client.setServer(MQTT_BROKER, MQTT_PORT);
  client.setCallback(mqttCallback);
  connectMQTT();

  Serial.printf("[%lu] [SYSTEM] Smart Flush ready — STANDBY\n", millis());
}

// ── 16. Loop ──────────────────────────────────────────────────
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[%lu] [WIFI] Dropped — reconnecting...\n", millis());
    connectWiFi();
  }

  if (!client.connected()) reconnectMQTT();
  client.loop();

  float distance = getDistance();

  if (client.connected()) publishDistance(distance);

  updateStateMachine(distance);

  updateLED();
}
