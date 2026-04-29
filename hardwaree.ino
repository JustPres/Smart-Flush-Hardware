// ── 1. Includes + Defines ────────────────────────────────────
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

// ── 2. Credentials ───────────────────────────────────────────
#define WIFI_SSID       "4th generation"
#define WIFI_PASSWORD   "Behappy@131516"
#define MQTT_BROKER     "ffc98acba62649a5b591fc33df78cc7a.s1.eu.hivemq.cloud"
#define MQTT_PORT       8883
#define MQTT_USER       "hardware_push"
#define MQTT_PASS       "Qhs8wWtUs5U77bg"

// ── 3. Pin Definitions ───────────────────────────────────────
#define TRIG_PIN        12
#define ECHO_PIN        13
#define PUMP_PIN        14
#define UV_PIN          27
#define SERVO1_PIN      25
 
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
unsigned long lidOpenedAt           = 0;   //   tracks when lid finished opening
unsigned long standbyEnteredAt      = 0;   //   tracks when STANDBY was entered

float distanceBuffer[5] = {0, 0, 0, 0, 0};
int distanceIndex       = 0;
bool ledState           = false;

// ── Helper: flush stale sensor readings ──
void clearDistanceBuffer() {
  for (int i = 0; i < 5; i++) distanceBuffer[i] = 999.0;  // fill with "far away"
  distanceIndex = 0;
  Serial.printf("[%lu] [SENSOR] Buffer cleared\n", millis());
}

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

// ── 11. Servo Functions (standard positional servo) ──────────
//
//    KEY FIX: This is a STANDARD servo, not continuous rotation.
//    - write(0)   = move to 0° (open position)
//    - write(180) = move to 180° (closed position)
//    - write(92)  = move to 92° (center) ← was WRONG, this moved lid BACK!
//
//    Old code did: write(0) → delay → write(92) → detach
//    This opened the lid, then MOVED IT BACK to center, then released torque.
//
//    New code: write(0) → delay → STAY attached at 0° (holds lid open with torque)
//    Only detach after closing since closed = resting position (gravity holds it).

#define LID_OPEN_POS    0      // servo angle for lid fully open
#define LID_CLOSE_POS   180    // servo angle for lid fully closed
#define OPEN_TIME       2500   // milliseconds for servo to reach open position
#define CLOSE_TIME      2500   // milliseconds for servo to reach closed position

void openLid() {
  Serial.printf("[%lu] [LID] Opening — moving servo to %d°\n", millis(), LID_OPEN_POS);
  servo1.attach(SERVO1_PIN);
  delay(10);
  servo1.write(LID_OPEN_POS);    // move to open position (0°)
  delay(OPEN_TIME);               // wait for servo to reach position
  //    Do NOT write STOP_POS — that moves the lid back to center!
  //    Do NOT detach — servo must HOLD the open position with torque!
  // Servo stays attached at 0° and actively holds the lid open.

  Serial.printf("[%lu] [LID] Open complete — servo holding at %d°\n", millis(), LID_OPEN_POS);

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
  Serial.printf("[%lu] [LID] Closing — moving servo to %d°\n", millis(), LID_CLOSE_POS);

  // Servo might already be attached from openLid(); attach is safe to call again
  if (!servo1.attached()) {
    servo1.attach(SERVO1_PIN);
    delay(10);
  }
  servo1.write(LID_CLOSE_POS);   // move to closed position (180°)
  delay(CLOSE_TIME);              // wait for servo to reach position
  servo1.detach();                // safe to detach — gravity/resting holds lid closed

  Serial.printf("[%lu] [LID] Closed — servo detached\n", millis());

  StaticJsonDocument<100> doc;
  doc["status"]    = "closed";
  doc["timestamp"] = millis();
  char buffer[100];
  serializeJson(doc, buffer);
  client.publish("toilet/events/lid", buffer);
  Serial.printf("[%lu] [LID] Closed — event published\n", millis());
}

// ── 12. Distance Sensor ───────────────────────────────────────
//    FIX: Only update the buffer when shouldUpdate=true (i.e., during
//    states where the sensor is actually measuring the person).
//    During FLUSHING/UV_ACTIVE the lid is closed and the sensor sees ~3cm,
//    which would poison the buffer and cause false detections on STANDBY.
float getDistance(bool shouldUpdate = true) {
  if (shouldUpdate && millis() - lastDistanceTrigger >= 200) {
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
//
//    KEY FIXES:
//   1. LID_OPEN now records lidOpenedAt and stays in this state for a
//      SENSOR_GRACE_MS grace period so the ultrasonic sensor can stabilize
//      after the lid finishes moving. During this time, sensor readings
//      are completely ignored.
//   2. WAITING_FOR_DEPARTURE only starts checking the sensor AFTER the
//      grace period expires.
//   3. LID_CLOSING no longer blindly falls through — it's now safe because
//      the state machine guarantees MIN_OPEN_TIME has passed before we
//      ever reach LID_CLOSING.

#define SENSOR_GRACE_MS    5000  // ignore sensor for 5s after lid opens (adjustable)
#define STANDBY_SETTLE_MS  2000  // ignore sensor for 2s after entering STANDBY (let buffer refill)

void updateStateMachine(float distance) {
  switch (currentState) {

    case STANDBY:
      //    FIX: After returning from UV_ACTIVE, give the sensor buffer time to
      //    refill with fresh readings before checking for person detection.
      if (standbyEnteredAt > 0 && millis() - standbyEnteredAt < STANDBY_SETTLE_MS) {
        break;  // still settling — ignore sensor
      }
      if (distance > 0 && distance < DETECTION_THRESHOLD_CM) {
        currentState = PERSON_DETECTED;
        Serial.printf("[%lu] [STATE] STANDBY → PERSON_DETECTED\n", millis());
      }
      break;

    case PERSON_DETECTED:
      // Open lid completely before moving to next state
      openLid();
      lidOpenedAt     = millis();  //   record when lid finished opening
      currentState    = LID_OPEN;
      personGoneTimer = 0;
      Serial.printf("[%lu] [STATE] PERSON_DETECTED → LID_OPEN\n", millis());
      break;

    case LID_OPEN:
      //    FIX: Stay in LID_OPEN for SENSOR_GRACE_MS to let the sensor stabilize.
      //    The open lid can deflect ultrasonic echoes, causing false "person gone" readings.
      //    We ignore ALL sensor data during this period.
      if (millis() - lidOpenedAt < SENSOR_GRACE_MS) {
        // Still in grace period — do nothing, just wait
        break;
      }

      // Grace period over — sensor should be stable now
      currentState    = WAITING_FOR_DEPARTURE;
      personGoneTimer = 0;
      Serial.printf("[%lu] [STATE] LID_OPEN → WAITING_FOR_DEPARTURE (grace period ended)\n", millis());
      break;

    case WAITING_FOR_DEPARTURE:
      {
        bool personPresent = (distance > 0 && distance < DETECTION_THRESHOLD_CM);

        if (!personPresent) {
          // Person not detected — start or continue the departure timer
          if (personGoneTimer == 0) {
            personGoneTimer = millis();
            Serial.printf("[%lu] [STATE] Person not detected — starting departure timer\n", millis());
          } else if (millis() - personGoneTimer >= (unsigned long)PERSON_GONE_CONFIRM_MS) {
            // Person has been gone long enough — close lid
            personGoneTimer = 0;
            currentState    = LID_CLOSING;
            Serial.printf("[%lu] [STATE] WAITING_FOR_DEPARTURE → LID_CLOSING\n", millis());
          }
        } else {
          // Person is still present — reset timer
          if (personGoneTimer != 0) {
            personGoneTimer = 0;
            Serial.printf("[%lu] [STATE] Person still present — resetting timer\n", millis());
          }
        }
      }
      break;

    case LID_CLOSING:
      // Close lid completely before pump starts
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
      if (millis() - pumpStartTime >= (unsigned long)PUMP_DURATION_MS) {
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
      if (millis() - uvStartTime >= (unsigned long)UV_DURATION_MS) {
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

        //    FIX: Clear the distance buffer so stale close-lid readings
        //    (e.g. 3.6 cm) don't immediately trigger false person detection.
        clearDistanceBuffer();
        standbyEnteredAt = millis();  // start settle timer

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

  //    FIX: Only update the sensor buffer during states where we're
  //    actually measuring person presence. During FLUSHING/UV_ACTIVE/LID_CLOSING
  //    the lid is closed and the sensor would read ~3cm (the lid itself),
  //    poisoning the median buffer.
  bool sensorRelevant = (currentState == STANDBY ||
                         currentState == PERSON_DETECTED ||
                         currentState == LID_OPEN ||
                         currentState == WAITING_FOR_DEPARTURE);

  float distance = getDistance(sensorRelevant);

  if (client.connected()) publishDistance(distance);

  updateStateMachine(distance);

  updateLED();
}
