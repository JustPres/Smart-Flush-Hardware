# 🚽 Smart Flush System — Hardware

**Justine M. Lopez — Hardware & Tech Lead**
BSIT 3A | SDCA Capstone 2026 | Dr. Alvin Jason A. Virata

> ESP32-based smart toilet IoT system. Detects presence, opens/closes the lid automatically, flushes, measures water volume, and runs UV disinfection — all connected to HiveMQ Cloud via MQTT.

---

## 📋 Project Overview

The Smart Flush System is an automated toilet controller built on the ESP32-WROOM-32. It uses an ultrasonic sensor to detect when a person is present and gone, then runs a full sequence:

1. **Lid opens** when person is detected
2. **Lid closes** after person leaves (3s confirmation)
3. **Pump flushes** for a configurable duration
4. **YF-S201** measures water volume during flush
5. **UV strip** disinfects for a configurable duration
6. All events are **published to HiveMQ Cloud** via MQTT so the web dashboard can display real-time data

---

## 🛠️ Hardware Components

| Component | Model | Quantity |
|---|---|---|
| Microcontroller | ESP32-WROOM-32 (DOIT DevKit V1) | 1 |
| Ultrasonic Sensor | HC-SR04 | 1 |
| Relay Module | 2-channel 5V relay | 1 |
| Water Pump | Submersible DC pump | 1 |
| UV Strip | 5V UV-C LED strip | 1 |
| Servo Motors | SG90 | 2 |
| Water Flow Sensor | YF-S201 | 1 |
| Power Bank | 5V USB power bank | 1 |
| Breadboard | Full-size | 1 |
| Jumper Wires | Male-to-male, male-to-female | Assorted |

---

## 📌 Pin Assignments

> ⚠️ These pins are confirmed from hardware testing. Do not change them.

| Component | GPIO | Notes |
|---|---|---|
| HC-SR04 TRIG | GPIO 13 | Output — trigger pulse |
| HC-SR04 ECHO | GPIO 12 | Input — echo return |
| Pump Relay (IN1) | GPIO 14 | Active LOW — LOW = ON |
| UV Strip Relay (IN2) | GPIO 27 | Active LOW — LOW = ON |
| Servo 1 (left lid) | GPIO 25 | PWM signal |
| Servo 2 (right lid) | GPIO 26 | PWM signal |
| YF-S201 Flow Sensor | GPIO 32 | Interrupt on RISING edge |
| Status LED | GPIO 2 | Built-in LED |

> **Note:** GPIO 32 is used for the YF-S201 instead of the originally planned GPIO 19 because GPIO 19 was unavailable. GPIO 32 is interrupt-capable and supports `INPUT_PULLUP`.

---

## 🔌 Hardware Setup & Wiring

### HC-SR04 Ultrasonic Sensor
```
HC-SR04 VCC  → 5V/VIN rail
HC-SR04 GND  → GND rail
HC-SR04 TRIG → ESP32 GPIO 13
HC-SR04 ECHO → ESP32 GPIO 12
```

### Relay Module (Pump + UV)
```
Relay VCC  → 5V/VIN rail
Relay GND  → GND rail
Relay IN1  → ESP32 GPIO 14  (Pump)
Relay IN2  → ESP32 GPIO 27  (UV Strip)
Relay COM  → Power bank (+) [jumper bridged for both channels]
GND        → Pump GND + UV GND + Power bank GND (twisted together)
```
> Relay is **active LOW** — send `LOW` to turn ON, `HIGH` to turn OFF.

### SG90 Servo Motors (x2)
```
Servo 1 Red (VCC)    → 5V/VIN rail
Servo 1 Brown (GND)  → GND rail
Servo 1 Orange (SIG) → ESP32 GPIO 25

Servo 2 Red (VCC)    → 5V/VIN rail
Servo 2 Brown (GND)  → GND rail
Servo 2 Orange (SIG) → ESP32 GPIO 26
```

### YF-S201 Water Flow Sensor
```
YF-S201 Red (VCC)    → 5V/VIN rail
YF-S201 Black (GND)  → GND rail
YF-S201 Yellow (SIG) → ESP32 GPIO 32
```

---

## 📦 How to Install Libraries

Open **Arduino IDE 2.x** → Go to **Sketch → Include Library → Manage Libraries** and install:

| Library | Search For | Author |
|---|---|---|
| ESP32Servo | `ESP32Servo` | Kevin Harrington |
| PubSubClient | `PubSubClient` | Nick O'Leary |
| ArduinoJson | `ArduinoJson` | Benoit Blanchon |

> `WiFi.h`, `WiFiClientSecure.h` are built-in with the ESP32 board package — no separate install needed.

### Installing ESP32 Board Package
1. Open Arduino IDE → **File → Preferences**
2. Add this URL to **Additional Boards Manager URLs**:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Go to **Tools → Board → Boards Manager**
4. Search `esp32` and install **esp32 by Espressif Systems**
5. Select board: **Tools → Board → ESP32 Arduino → DOIT ESP32 DEVKIT V1**

---

## 🔐 How to Configure Credentials

> ⚠️ **Never commit real credentials to GitHub.** The firmware uses `#define` placeholders that you fill in locally before flashing.

Open the `.ino` file and update these lines at the very top:

```cpp
// UPDATE THESE BEFORE FLASHING ─────────────────────────────────
#define WIFI_SSID      "YOUR_WIFI_SSID"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"
#define MQTT_BROKER    "YOUR_HIVEMQ_BROKER_URL"
#define MQTT_USERNAME  "YOUR_HIVEMQ_USERNAME"
#define MQTT_PASSWORD  "YOUR_HIVEMQ_PASSWORD"
```

### Where to find your HiveMQ credentials
1. Go to [HiveMQ Cloud](https://console.hivemq.cloud/)
2. Open your cluster
3. **Broker URL** — shown on the cluster overview page (e.g. `xxxxxxxx.s1.eu.hivemq.cloud`)
4. **Username / Password** — go to **Access Management → Credentials**

### Where to find your WiFi credentials
- **SSID** — your WiFi network name
- **Password** — your WiFi password

> After filling in credentials, upload to ESP32. The Serial Monitor (115200 baud) will confirm connection.

---

## 🏗️ Build Phases

| Phase | Status | Description |
|---|---|---|
| H1 | ✅ Done | HC-SR04 + Pump + UV Relay — sequence confirmed working |
| H2 | 🔄 In Progress | SG90 Servo Motors — wired to GPIO 25 & 26 |
| H3 | 🔄 In Progress | YF-S201 Flow Sensor — wired to GPIO 32 |
| H4 | ⏳ Pending | WiFi + HiveMQ MQTT connection |
| H5 | ⏳ Pending | Full state machine — run after H1–H4 all pass |

---

## 📡 Serial Monitor

Set baud rate to **115200**. All logs follow this format:

```
[MILLIS] [MODULE] message
```

Example output:
```
[12345] [WIFI] Connected! IP: 192.168.1.100
[12400] [MQTT] Connected!
[15000] [SENSOR] Distance: 14.23cm
[15001] [SYSTEM] Person detected.
[20000] [LID] Opening...
[20510] [LID] Open
[22020] [LID] Closing...
[22520] [PUMP] ON
[25530] [PUMP] OFF Total: 0.082L
[26530] [UV] ON
[31530] [UV] OFF
[31531] [SYSTEM] Sequence complete.
```

---

## 👥 Team

| Name | Role |
|---|---|
| Justine M. Lopez | Hardware & Tech Lead |
| James Carl V. Alvarez | Backend Developer |
| Christian Ivan A. Los Baños | Frontend Developer |

**Adviser:** Dr. Alvin Jason A. Virata
**Section:** BSIT 3A | SDCA Capstone 2026
