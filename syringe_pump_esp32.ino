/**
 * @file syringe_pump_esp32.ino
 * @brief Clinical-grade Syringe Pump Firmware for ESP32
 *
 * Features:
 *  - High-precision Stepper Motor Control (non-blocking)
 *  - Real-time Telemetry via WebSockets (100ms updates)
 *  - Auto-Recovery (EEPROM Black Box recording)
 *  - Safety Alarms (Occlusion, Empty Syringe, Dose Completed)
 *
 * @version 1.2.0 (Production Ready)
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <WebSocketsServer.h>
#include <WiFi.h>

// ── Configuration ─────────────────────────────────────────────
namespace Config {
// WiFi Access Point
constexpr char AP_SSID[] = "SyringePump";
constexpr char AP_PASSWORD[] = "12345678";

// Hardware Pins
constexpr uint8_t PIN_MOTOR_IN1 = 25;
constexpr uint8_t PIN_MOTOR_IN2 = 26;
constexpr uint8_t PIN_MOTOR_IN3 = 27;
constexpr uint8_t PIN_MOTOR_IN4 = 14;
constexpr uint8_t PIN_FSR = 32;
constexpr uint8_t PIN_BUZZER = 33;
constexpr uint8_t PIN_IR = 13;

// Sensor States
// HIGH: No reflection (Black object or empty space)
// LOW:  Reflection detected (White object or clear barrel)
constexpr uint8_t IR_EMPTY_STATE = LOW;

// Calibration & Limits
constexpr float STEPS_PER_ML = 1600.0f;
constexpr int BACKLASH_STEPS = 900;
constexpr int OCCLUSION_THRESHOLD = 900;
constexpr int FSR_SAMPLES = 10;
constexpr float MAX_SYRINGE_VOL = 10.0f;
constexpr float MAX_FLOW_RATE = 35.0f; // mL/min
constexpr float MIN_FLOW_RATE = 0.01f; // mL/min

// EEPROM Memory
constexpr size_t EEPROM_SIZE = 128;
constexpr uint32_t EEPROM_MAGIC_NUM = 0xDEADBEEF;
constexpr uint8_t EEPROM_LAYOUT_VER = 1;
} // namespace Config

// ── Memory Optimized Step Matrix ──────────────────────────────
const uint8_t STEP_MATRIX[8][4] = {{1, 0, 0, 0}, {1, 1, 0, 0}, {0, 1, 0, 0},
                                   {0, 1, 1, 0}, {0, 0, 1, 0}, {0, 0, 1, 1},
                                   {0, 0, 0, 1}, {1, 0, 0, 1}};

// ── EEPROM Structure ──────────────────────────────────────────
struct __attribute__((packed)) EEPROM_Layout {
  uint32_t magic;
  uint8_t layoutVersion;
  bool wasRunning;
  float deliveredVol;
  float setVolume;
  float setRate;
};
EEPROM_Layout eepromData;

// ── Global State ──────────────────────────────────────────────
WebSocketsServer wsServer(81);

struct PumpState {
  bool running = false;
  int direction = 0;
  int lastDirection = 1;
  uint32_t backlashRemaining = 0;
  float setRate = 5.0f;
  float setVolume = 5.0f;
  float deliveredVol = 0.0f;
  float actualRate = 0.0f;
  float minsRemaining = -1.0f;
  bool occlusion = false;
  bool empty = false;
  bool doseCompleted = false;
  int fsrRaw = 0;
} pump;

// Timing & Operation Tracking
unsigned long lastStepMicros = 0;
unsigned long lastSendMillis = 0;
unsigned long lastEepromSave = 0;
unsigned long lastBuzzerMillis = 0;
unsigned long lastRateMillis = 0;
unsigned long stepDelayMicros = 2000;

float lastVolForRate = 0.0f;
uint32_t totalSteps = 0;
int currentStepIndex = 0;

// Alarm & Audio State
bool buzzerState = false;
bool buzzerMuted =
    false; // True = user silenced alarm; auto-clears on new alarm
int buzzerPhase = 0;
int lastAlarmType = 0;

// Async EEPROM Deferral (Prevents WDT panics during WebSocket ISR)
bool pendingEepromSave = false;
bool pendingEepromClear = false;

// ─────────────────────────────────────────────────────────────
// HARDWARE CONTROL
// ─────────────────────────────────────────────────────────────

inline void turnOffMotor() {
  digitalWrite(Config::PIN_MOTOR_IN1, LOW);
  digitalWrite(Config::PIN_MOTOR_IN2, LOW);
  digitalWrite(Config::PIN_MOTOR_IN3, LOW);
  digitalWrite(Config::PIN_MOTOR_IN4, LOW);
}

void silenceBuzzer() {
  digitalWrite(Config::PIN_BUZZER, LOW);
  buzzerState = false;
  buzzerMuted = true; // Mute: don't restart even if alarm flag is still active
  buzzerPhase = 0;
  lastAlarmType = 0;
}

void updateStepDelay() {
  if (pump.setRate <= 0.0f) {
    stepDelayMicros = 3000;
    return;
  }
  float mlPerSec = pump.setRate / 60.0f;
  float stepsPerSec = mlPerSec * Config::STEPS_PER_ML;
  stepDelayMicros =
      (stepsPerSec > 0.0f) ? (unsigned long)(1000000.0f / stepsPerSec) : 3000;

  // Physical constraint to prevent motor stalling
  stepDelayMicros = constrain(stepDelayMicros, 800UL, 2000000UL);
}

int readSmoothedFSR() {
  static int buffer[Config::FSR_SAMPLES] = {0};
  static int idx = 0;
  buffer[idx] = analogRead(Config::PIN_FSR);
  idx = (idx + 1) % Config::FSR_SAMPLES;

  long sum = 0;
  for (int i = 0; i < Config::FSR_SAMPLES; i++) {
    sum += buffer[i];
  }
  return sum / Config::FSR_SAMPLES;
}

// ─────────────────────────────────────────────────────────────
// DATA PERSISTENCE (BLACK BOX)
// ─────────────────────────────────────────────────────────────

void saveBlackBox() {
  eepromData.magic = Config::EEPROM_MAGIC_NUM;
  eepromData.layoutVersion = Config::EEPROM_LAYOUT_VER;
  eepromData.wasRunning = pump.running;
  eepromData.deliveredVol = pump.deliveredVol;
  eepromData.setVolume = pump.setVolume;
  eepromData.setRate = pump.setRate;
  EEPROM.put(0, eepromData);
  EEPROM.commit();
}

void clearBlackBox() {
  eepromData.magic = 0;
  EEPROM.put(0, eepromData);
  EEPROM.commit();
}

// ─────────────────────────────────────────────────────────────
// TELEMETRY & COMMS
// ─────────────────────────────────────────────────────────────

void sendTelemetry() {
  if (wsServer.connectedClients() == 0)
    return;

  pump.minsRemaining = (pump.setRate > 0.0f && pump.running)
                           ? (pump.setVolume - pump.deliveredVol) / pump.setRate
                           : -1.0f;

  JsonDocument doc;
  doc["deliveredVol"] = pump.deliveredVol;
  doc["setRate"] = pump.setRate;
  doc["setVolume"] = pump.setVolume;
  doc["running"] = pump.running;
  doc["occlusion"] = pump.occlusion;
  doc["empty"] = pump.empty;
  doc["doseCompleted"] = pump.doseCompleted;
  doc["fsrRaw"] = pump.fsrRaw;
  doc["actualRate"] = pump.actualRate;
  doc["minsRemaining"] = pump.minsRemaining;

  String json;
  serializeJson(doc, json);
  wsServer.broadcastTXT(json);
}

void handleWSMessage(uint8_t clientNum, const String &msg) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err)
    return;

  String cmd = doc["cmd"] | "";
  if (cmd.length() == 0)
    return;

  if (cmd == "start") {
    pump.running = true;
    pump.direction = 1;
    pump.occlusion = false;
    pump.empty = false;
    pump.doseCompleted = false;
    silenceBuzzer();

    // Apply mechanical backlash compensation if direction changed
    if (pump.lastDirection != 1) {
      pump.backlashRemaining = Config::BACKLASH_STEPS;
      pump.lastDirection = 1;
    }
    updateStepDelay();
  } else if (cmd == "pause") {
    pump.running = false;
    pump.direction = 0;
    turnOffMotor();
    silenceBuzzer();
    pendingEepromSave = true; // Safely defer flash write
  } else if (cmd == "reset") {
    pump.running = false;
    pump.deliveredVol = 0.0f;
    totalSteps = 0;
    pump.occlusion = false;
    pump.empty = false;
    pump.doseCompleted = false;
    lastVolForRate = 0.0f;
    pump.actualRate = 0.0f;
    pump.minsRemaining = -1.0f;
    turnOffMotor();
    silenceBuzzer();
    pendingEepromClear = true; // Safely defer flash erase
  } else if (cmd == "jog") {
    int dir = doc["dir"] | 0;
    pump.running = true;
    pump.direction = dir;
    pump.occlusion = false;
    pump.empty = false;
    pump.doseCompleted = false;
    pump.backlashRemaining = 0;
    pump.lastDirection = dir;
    stepDelayMicros = 1200; // Fixed reliable jog speed
    silenceBuzzer();
  } else if (cmd == "silence") {
    silenceBuzzer();
  } else if (cmd == "settings") {
    if (doc["rate"].is<float>()) {
      pump.setRate = constrain((float)doc["rate"], Config::MIN_FLOW_RATE,
                               Config::MAX_FLOW_RATE);
    }
    if (doc["volume"].is<float>()) {
      pump.setVolume =
          constrain((float)doc["volume"], 0.1f, Config::MAX_SYRINGE_VOL);
    }
    updateStepDelay();
  }
}

void onWSEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_TEXT) {
    // Construct string safely to prevent out-of-bounds reading
    String msg;
    msg.reserve(length);
    for (size_t i = 0; i < length; i++) {
      msg += (char)payload[i];
    }
    handleWSMessage(num, msg);
  } else if (type == WStype_CONNECTED) {
    sendTelemetry();
  }
}

// ─────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  EEPROM.begin(Config::EEPROM_SIZE);

  // Initialize Pins
  pinMode(Config::PIN_MOTOR_IN1, OUTPUT);
  pinMode(Config::PIN_MOTOR_IN2, OUTPUT);
  pinMode(Config::PIN_MOTOR_IN3, OUTPUT);
  pinMode(Config::PIN_MOTOR_IN4, OUTPUT);
  pinMode(Config::PIN_BUZZER, OUTPUT);
  pinMode(Config::PIN_IR, INPUT_PULLUP);
  turnOffMotor();

  // Recover Session from Black Box
  EEPROM.get(0, eepromData);
  if (eepromData.magic == Config::EEPROM_MAGIC_NUM &&
      eepromData.layoutVersion == Config::EEPROM_LAYOUT_VER) {
    pump.deliveredVol = eepromData.deliveredVol;
    pump.setVolume = eepromData.setVolume;
    pump.setRate = eepromData.setRate;
    totalSteps = (uint32_t)(pump.deliveredVol * Config::STEPS_PER_ML);
    lastVolForRate = pump.deliveredVol;

    Serial.printf("[INFO] Auto-Recovery: %.3f mL / %.3f mL @ %.2f mL/min\n",
                  pump.deliveredVol, pump.setVolume, pump.setRate);
  }

  // Configure Access Point
  WiFi.mode(WIFI_AP);
  WiFi.softAP(Config::AP_SSID, Config::AP_PASSWORD);
  delay(200);
  Serial.printf("[INFO] AP Online: %s | IP: %s\n", Config::AP_SSID,
                WiFi.softAPIP().toString().c_str());

  // Start Server
  wsServer.begin();
  wsServer.onEvent(onWSEvent);
  updateStepDelay();
}

// ─────────────────────────────────────────────────────────────
// MAIN LOOP
// ─────────────────────────────────────────────────────────────

void loop() {
  wsServer.loop();

  unsigned long nowMicros = micros();
  unsigned long nowMillis = millis();

  // 1. Sensor Safety Monitoring
  if (pump.running && pump.direction == 1 && pump.backlashRemaining == 0) {
    // Detect Empty Syringe (with Debounce to prevent false triggers from noise)
    static int irDebounceCounter = 0;
    if (digitalRead(Config::PIN_IR) == Config::IR_EMPTY_STATE) {
      irDebounceCounter++;
      if (irDebounceCounter >
          5) { // Must be consistently empty for ~5 loop iterations
        pump.empty = true;
        pump.running = false;
        buzzerMuted = false; // New alarm event — unmute
        turnOffMotor();
        pendingEepromSave = true;
        irDebounceCounter = 0;
      }
    } else {
      irDebounceCounter = 0;
    }

    // Detect Occlusion (Blockage)
    pump.fsrRaw = readSmoothedFSR();
    if (pump.fsrRaw >= Config::OCCLUSION_THRESHOLD) {
      pump.occlusion = true;
      pump.running = false;
      buzzerMuted = false; // New alarm event — unmute
      turnOffMotor();
      pendingEepromSave = true;
    }
  }

  // 2. High-Precision Motor Control
  if (pump.running && pump.direction != 0) {
    unsigned long currentDelay =
        (pump.backlashRemaining > 0) ? 1200 : stepDelayMicros;

    if (nowMicros - lastStepMicros >= currentDelay) {
      lastStepMicros = nowMicros;

      // Safe wrap for step matrix
      currentStepIndex = ((currentStepIndex + pump.direction) % 8 + 8) % 8;

      digitalWrite(Config::PIN_MOTOR_IN1, STEP_MATRIX[currentStepIndex][0]);
      digitalWrite(Config::PIN_MOTOR_IN2, STEP_MATRIX[currentStepIndex][1]);
      digitalWrite(Config::PIN_MOTOR_IN3, STEP_MATRIX[currentStepIndex][2]);
      digitalWrite(Config::PIN_MOTOR_IN4, STEP_MATRIX[currentStepIndex][3]);

      if (pump.backlashRemaining > 0) {
        pump.backlashRemaining--;
      } else if (pump.direction == 1) {
        totalSteps++;
        pump.deliveredVol =
            min((float)totalSteps / Config::STEPS_PER_ML, pump.setVolume);

        // Dose Completion Check
        if (pump.deliveredVol >= pump.setVolume) {
          pump.running = false;
          pump.doseCompleted = true;
          turnOffMotor();
          pendingEepromSave = true;
        }
      }
    }
  }

  // 3. Live Rate Calculation
  if (nowMillis - lastRateMillis >= 1000) {
    float dt = (nowMillis - lastRateMillis) / 60000.0f; // Time in minutes
    if (dt > 0.0f) {
      pump.actualRate = (pump.deliveredVol - lastVolForRate) / dt;
    }
    lastVolForRate = pump.deliveredVol;
    lastRateMillis = nowMillis;
  }

  // 4. Acoustic Alarms (Non-blocking patterns)
  int currentAlarm =
      pump.occlusion ? 1 : (pump.empty ? 2 : (pump.doseCompleted ? 3 : 0));

  // Auto-unmute if alarm type changes (new event)
  if (currentAlarm != lastAlarmType && currentAlarm != 0) {
    buzzerMuted = false;
  }

  if (currentAlarm != lastAlarmType) {
    buzzerPhase = 0;
    lastAlarmType = currentAlarm;
    lastBuzzerMillis = nowMillis;
    if (currentAlarm == 0)
      silenceBuzzer();
  }

  // Only sound buzzer if not muted by user
  if (!buzzerMuted) {
    if (currentAlarm == 1) {
      // OCCLUSION: Continuous rapid beep
      if (nowMillis - lastBuzzerMillis >= 200) {
        lastBuzzerMillis = nowMillis;
        buzzerState = !buzzerState;
        digitalWrite(Config::PIN_BUZZER, buzzerState ? HIGH : LOW);
      }
    } else if (currentAlarm == 2) {
      // EMPTY SYRINGE: Double beep
      static const uint16_t emptyPat[] = {200, 200, 200, 800};
      if (nowMillis - lastBuzzerMillis >= emptyPat[buzzerPhase]) {
        lastBuzzerMillis = nowMillis;
        buzzerPhase = (buzzerPhase + 1) % 4;
        digitalWrite(Config::PIN_BUZZER,
                     (buzzerPhase == 0 || buzzerPhase == 2) ? HIGH : LOW);
      }
    } else if (currentAlarm == 3) {
      // DOSE COMPLETED: Triple chirp
      static const uint16_t donePat[] = {100, 100, 100, 100, 100, 1200};
      if (nowMillis - lastBuzzerMillis >= donePat[buzzerPhase]) {
        lastBuzzerMillis = nowMillis;
        buzzerPhase = (buzzerPhase + 1) % 6;
        digitalWrite(Config::PIN_BUZZER,
                     (buzzerPhase == 0 || buzzerPhase == 2 || buzzerPhase == 4)
                         ? HIGH
                         : LOW);
      }
    }
  }

  // 5. Adaptive Telemetry Dispatch
  unsigned long telInterval = 500; // Idle
  if (pump.occlusion || pump.empty || pump.doseCompleted) {
    telInterval = 150; // Alert state
  } else if (pump.running) {
    telInterval = 100; // Active state
  }

  if (nowMillis - lastSendMillis >= telInterval) {
    lastSendMillis = nowMillis;
    sendTelemetry();
  }

  // 6. Periodic & Deferred EEPROM Writes
  if (nowMillis - lastEepromSave >= 10000 && pump.running) {
    lastEepromSave = nowMillis;
    pendingEepromSave = true;
  }

  if (pendingEepromSave) {
    pendingEepromSave = false;
    saveBlackBox();
  }
  if (pendingEepromClear) {
    pendingEepromClear = false;
    clearBlackBox();
  }
}
