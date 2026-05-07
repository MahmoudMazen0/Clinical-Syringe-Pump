// ============================================================
//  Syringe Pump ESP32 - Corrected Production Version
//  Fixes: step index wrap, deliveredVol clamp, recovery flag,
//         EEPROM on-stop save, WiFi log, telemetry guard,
//         minsRemaining calc, actualRate calc, IR pull-up,
//         EEPROM layout version
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

// ── WiFi Access Point Settings ───────────────────────────────
// ESP32 creates its own WiFi network — no router/hotspot needed!
const char* AP_SSID     = "SyringePump";
const char* AP_PASSWORD = "12345678";   // min 8 chars

// ── Hardware Pins ─────────────────────────────────────────────
#define IN1        25
#define IN2        26
#define IN3        27
#define IN4        14
#define FSR_PIN    32
#define BUZZER_PIN 33
#define IR_PIN     13

// ── Calibration & Safety Limits (DO NOT CHANGE) ───────────────
const float STEPS_PER_ML        = 1600.0;
const int   BACKLASH_STEPS      = 900;
const int   OCCLUSION_THRESHOLD = 600;
const int   FSR_SAMPLES         = 10;

const float MAX_SYRINGE_VOL = 10.0;
const float MAX_FLOW_RATE   = 40.0;   // Absolute physical limit (mL/min)
const float MIN_FLOW_RATE   = 0.01;   // mL/min

// ── Memory Optimized Step Matrix ──────────────────────────────
const uint8_t STEP_MATRIX[8][4] = {
  {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0},
  {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1}
};

// ── EEPROM Structure ──────────────────────────────────────────
#define EEPROM_SIZE         128
#define EEPROM_MAGIC_NUM    0xDEADBEEF
#define EEPROM_LAYOUT_VER   1          // FIX #10: bump if struct changes

struct EEPROM_Layout {
  uint32_t magic;
  uint8_t  layoutVersion;              // FIX #10: layout version guard
  bool     wasRunning;
  float    deliveredVol;
  float    setVolume;
  float    setRate;
};
EEPROM_Layout eepromData;

// ── WebSocket Server ──────────────────────────────────────────
WebSocketsServer wsServer(81);

// ── System State ──────────────────────────────────────────────
struct PumpState {
  bool    running           = false;
  int     direction         = 0;
  int     lastDirection     = 1;
  long    backlashRemaining = 0;
  float   setRate           = 5.0;
  float   setVolume         = 5.0;
  float   deliveredVol      = 0.0;
  float   actualRate        = 0.0;
  float   minsRemaining     = -1.0;
  bool    occlusion         = false;
  bool    empty             = false;
  bool    doseCompleted     = false;
  int     fsrRaw            = 0;
  int     alertLevel        = 0;
} pump;

// EEPROM Deferral Flags to prevent WDT panics during WebSocket events
bool pendingEepromSave = false;
bool pendingEepromClear = false;

// ── State Variables ───────────────────────────────────────────
unsigned long lastStepMicros   = 0;
unsigned long lastSendMillis   = 0;
unsigned long lastEepromSave   = 0;
unsigned long lastBuzzerMillis = 0;
unsigned long lastRateMillis   = 0;
unsigned long stepDelayMicros  = 2000;

float lastVolForRate = 0.0;
bool  buzzerState    = false;
int   buzzerPhase    = 0;    // for pattern-based buzzer
int   lastAlarmType  = 0;    // 0=none 1=occlusion 2=empty 3=done

int  currentStepIndex = 0;
long totalSteps       = 0;

// ─────────────────────────────────────────────────────────────
// FUNCTION PROTOTYPES
// ─────────────────────────────────────────────────────────────
void turnOffMotor();
void silenceBuzzer();
void updateStepDelay();
int readSmoothedFSR();
void saveBlackBox();
void clearBlackBox();
void sendTelemetry();
void handleWSMessage(uint8_t clientNum, String msg);
void onWSEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);

// ─────────────────────────────────────────────────────────────
// HARDWARE CONTROL
// ─────────────────────────────────────────────────────────────

void turnOffMotor() {
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}

void silenceBuzzer() {
  digitalWrite(BUZZER_PIN, LOW);
  buzzerState  = false;
  buzzerPhase  = 0;
  lastAlarmType = 0;
}

void updateStepDelay() {
  if (pump.setRate <= 0) { stepDelayMicros = 3000; return; }
  float mlPerSec    = pump.setRate / 60.0;  // Rate is in mL/min
  float stepsPerSec = mlPerSec * STEPS_PER_ML;
  stepDelayMicros   = (stepsPerSec > 0)
                        ? (unsigned long)(1000000.0 / stepsPerSec)
                        : 3000;
  // No upper cap: let physics dictate speed so actualRate matches setRate exactly
  // Min 800µs = max ~2800 mL/hr (motor physical limit)
  stepDelayMicros = constrain(stepDelayMicros, 800, 2000000);
}

// Moving Average Filter for FSR
int readSmoothedFSR() {
  static int buffer[FSR_SAMPLES] = {0};
  static int idx = 0;
  buffer[idx] = analogRead(FSR_PIN);
  idx = (idx + 1) % FSR_SAMPLES;
  long sum = 0;
  for (int i = 0; i < FSR_SAMPLES; i++) sum += buffer[i];
  return sum / FSR_SAMPLES;
}

// ─────────────────────────────────────────────────────────────
// EEPROM / BLACKBOX
// ─────────────────────────────────────────────────────────────

void saveBlackBox() {
  eepromData.magic         = EEPROM_MAGIC_NUM;
  eepromData.layoutVersion = EEPROM_LAYOUT_VER;
  eepromData.wasRunning    = pump.running;
  eepromData.deliveredVol  = pump.deliveredVol;
  eepromData.setVolume     = pump.setVolume;
  eepromData.setRate       = pump.setRate;
  EEPROM.put(0, eepromData);
  EEPROM.commit();
}

void clearBlackBox() {
  eepromData.magic = 0;
  EEPROM.put(0, eepromData);
  EEPROM.commit();
}

// ─────────────────────────────────────────────────────────────
// TELEMETRY
// ─────────────────────────────────────────────────────────────

void sendTelemetry() {
  // FIX #6: skip broadcast if no client is connected
  if (wsServer.connectedClients() == 0) return;

  // FIX #7: calculate minsRemaining live (setRate is in mL/min now)
  pump.minsRemaining = (pump.setRate > 0 && pump.running)
    ? (pump.setVolume - pump.deliveredVol) / pump.setRate
    : -1.0f;

  JsonDocument doc;
  doc["deliveredVol"]  = pump.deliveredVol;
  doc["setRate"]       = pump.setRate;
  doc["setVolume"]     = pump.setVolume;
  doc["running"]       = pump.running;
  doc["occlusion"]     = pump.occlusion;
  doc["empty"]         = pump.empty;
  doc["doseCompleted"] = pump.doseCompleted;
  doc["fsrRaw"]        = pump.fsrRaw;
  doc["actualRate"]    = pump.actualRate;    // FIX #8
  doc["minsRemaining"] = pump.minsRemaining; // FIX #7

  String json;
  serializeJson(doc, json);
  wsServer.broadcastTXT(json);
}

// ─────────────────────────────────────────────────────────────
// WEBSOCKET HANDLER
// ─────────────────────────────────────────────────────────────

void handleWSMessage(uint8_t clientNum, String msg) {
  JsonDocument doc;
  deserializeJson(doc, msg);

  // FIX #12: use ArduinoJson String extraction — safer than raw char*
  String cmd = doc["cmd"] | "";
  if (cmd.length() == 0) return;

  if (cmd == "start") {
    pump.running      = true;
    pump.direction    = 1;
    pump.occlusion    = false;
    pump.empty        = false;
    pump.doseCompleted = false;
    silenceBuzzer();
    if (pump.lastDirection != 1) {
      pump.backlashRemaining = BACKLASH_STEPS;
      pump.lastDirection     = 1;
    }
    updateStepDelay();
  }
  else if (cmd == "pause") {
    pump.running  = false;
    pump.direction = 0;
    turnOffMotor();
    silenceBuzzer();
    pendingEepromSave = true; // FIX: Defer EEPROM write to main loop
  }
  else if (cmd == "reset") {
    pump.running       = false;
    pump.deliveredVol  = 0;
    totalSteps         = 0;
    pump.occlusion     = false;
    pump.empty         = false;
    pump.doseCompleted = false;
    lastVolForRate     = 0;
    pump.actualRate    = 0;
    pump.minsRemaining = -1.0f;
    silenceBuzzer();
    turnOffMotor();
    pendingEepromClear = true; // FIX: Defer EEPROM clear to main loop
  }
  else if (cmd == "jog") {
    int dir = doc["dir"] | 0;
    pump.running       = true;
    pump.direction     = dir;
    pump.occlusion     = false;
    pump.empty         = false;
    pump.doseCompleted = false;
    pump.backlashRemaining = 0;
    pump.lastDirection = dir;   // FIX: Track jog direction so Start triggers backlash
    stepDelayMicros    = 1200;  // reliable jog speed for 28BYJ-48
    silenceBuzzer();
  }
  else if (cmd == "silence") {
    // Silence buzzer only — does NOT stop the pump
    silenceBuzzer();
  }
  else if (cmd == "settings") {
    if (doc["rate"].is<float>()) {
      pump.setRate = constrain((float)doc["rate"], MIN_FLOW_RATE, MAX_FLOW_RATE);
    }
    if (doc["volume"].is<float>()) {
      pump.setVolume = constrain((float)doc["volume"], 0.1f, MAX_SYRINGE_VOL);
    }
    updateStepDelay();
  }
}

void onWSEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_TEXT) {
    // Safely construct String using length to prevent out-of-bounds reading
    String msg;
    msg.reserve(length);
    for(size_t i = 0; i < length; i++) msg += (char)payload[i];
    handleWSMessage(num, msg);
  }
  if (type == WStype_CONNECTED) sendTelemetry();
}

// ─────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);

  // Restore state from EEPROM if valid
  EEPROM.get(0, eepromData);
  if (eepromData.magic         == EEPROM_MAGIC_NUM &&
      eepromData.layoutVersion == EEPROM_LAYOUT_VER &&  // FIX #10
      eepromData.wasRunning) {
    // FIX #3: do NOT auto-resume — just restore volume & settings.
    // User must press Start manually (safer for a medical device).
    pump.deliveredVol = eepromData.deliveredVol;
    pump.setVolume    = eepromData.setVolume;
    pump.setRate      = eepromData.setRate;
    totalSteps        = (long)(pump.deliveredVol * STEPS_PER_ML);
    lastVolForRate    = pump.deliveredVol;
    Serial.printf("[INFO] Restored %.3f mL of %.3f mL @ %.2f mL/min\n",
                  pump.deliveredVol, pump.setVolume, pump.setRate);
    Serial.println("[INFO] Press Start to resume infusion.");
  }

  // Hardware Init
  pinMode(IN1,        OUTPUT);
  pinMode(IN2,        OUTPUT);
  pinMode(IN3,        OUTPUT);
  pinMode(IN4,        OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(IR_PIN,     INPUT_PULLUP); // FIX #11: avoid floating input
  turnOffMotor();

  // ── WiFi Access Point Mode ───────────────────────────────────
  // ESP32 creates its own network — IP is always 192.168.4.1
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  delay(200);
  Serial.printf("[INFO] AP started — SSID: %s  IP: %s\n",
                AP_SSID, WiFi.softAPIP().toString().c_str());

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

  // ── 0. Nothing needed — AP mode is always available ─────────

  // ── 1. Sensor Monitoring (normal infusion only) ─────────────
  if (pump.running && pump.direction == 1 && pump.backlashRemaining == 0) {

    // IR empty detection (HIGH = black marker detected)
    if (digitalRead(IR_PIN) == HIGH) {
      pump.empty   = true;
      pump.running = false;
      turnOffMotor();
      saveBlackBox(); // FIX #9: save final state on stop
    }

    pump.fsrRaw = readSmoothedFSR();
    if (pump.fsrRaw >= OCCLUSION_THRESHOLD) {
      pump.occlusion = true;
      pump.running   = false;
      turnOffMotor();
      saveBlackBox(); // FIX #9
    }
  }

  // ── 2. Stepper Motor Execution ──────────────────────────────
  if (pump.running && pump.direction != 0) {
    // Backlash at same speed as jog for consistency
    unsigned long currentDelay = (pump.backlashRemaining > 0) ? 1200 : stepDelayMicros;

    if (nowMicros - lastStepMicros >= currentDelay) {
      lastStepMicros = nowMicros;

      // FIX #1: safe modular wrap for both +1 and -1 directions
      currentStepIndex = ((currentStepIndex + pump.direction) % 8 + 8) % 8;

      digitalWrite(IN1, STEP_MATRIX[currentStepIndex][0]);
      digitalWrite(IN2, STEP_MATRIX[currentStepIndex][1]);
      digitalWrite(IN3, STEP_MATRIX[currentStepIndex][2]);
      digitalWrite(IN4, STEP_MATRIX[currentStepIndex][3]);

      if (pump.backlashRemaining > 0) {
        pump.backlashRemaining--;
      } else if (pump.direction == 1) {
        totalSteps++;
        // FIX #2: clamp deliveredVol to setVolume — no overshoot in telemetry
        pump.deliveredVol = min(totalSteps / STEPS_PER_ML, pump.setVolume);

        if (pump.deliveredVol >= pump.setVolume) {
          pump.running       = false;
          pump.doseCompleted = true;
          turnOffMotor();
          saveBlackBox(); // FIX #9
        }
      }
    }
  }

  // ── 3. Actual Rate Calculation (every 1 s) ──────────────────
  // FIX #8: rolling 1-second rate measurement in mL/min
  if (nowMillis - lastRateMillis >= 1000) {
    float dt = (nowMillis - lastRateMillis) / 60000.0f; // minutes
    pump.actualRate  = (pump.deliveredVol - lastVolForRate) / dt;
    lastVolForRate   = pump.deliveredVol;
    lastRateMillis   = nowMillis;
  }

  // ── 4. Alarm Management (distinct patterns per alarm type) ────
  int currentAlarm = pump.occlusion ? 1 : (pump.empty ? 2 : (pump.doseCompleted ? 3 : 0));
  if (currentAlarm != lastAlarmType) {
    buzzerPhase    = 0;
    lastAlarmType  = currentAlarm;
    lastBuzzerMillis = nowMillis;
    if (currentAlarm == 0) silenceBuzzer();
  }
  if (currentAlarm == 1) {
    // OCCLUSION: rapid continuous beep 200ms (most urgent)
    if (nowMillis - lastBuzzerMillis >= 200) {
      lastBuzzerMillis = nowMillis;
      buzzerState = !buzzerState;
      digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
    }
  } else if (currentAlarm == 2) {
    // EMPTY SYRINGE: double-beep  [on200, off200, on200, off800]
    static const uint16_t emptyPat[] = {200, 200, 200, 800};
    if (nowMillis - lastBuzzerMillis >= emptyPat[buzzerPhase]) {
      lastBuzzerMillis = nowMillis;
      buzzerPhase = (buzzerPhase + 1) % 4;
      digitalWrite(BUZZER_PIN, (buzzerPhase == 0 || buzzerPhase == 2) ? HIGH : LOW);
    }
  } else if (currentAlarm == 3) {
    // DOSE COMPLETED: triple-chirp  [on100, off100, on100, off100, on100, off1200]
    static const uint16_t donePat[] = {100, 100, 100, 100, 100, 1200};
    if (nowMillis - lastBuzzerMillis >= donePat[buzzerPhase]) {
      lastBuzzerMillis = nowMillis;
      buzzerPhase = (buzzerPhase + 1) % 6;
      digitalWrite(BUZZER_PIN, (buzzerPhase==0||buzzerPhase==2||buzzerPhase==4) ? HIGH : LOW);
    }
  }

  // ── 5. Telemetry ────────────────────────────────────────────
  // Send every 100ms during active injection for real-time UI, 500ms when idle
  unsigned long telInterval = 500;
  if (pump.occlusion || pump.empty || pump.doseCompleted) {
    telInterval = 150;
  } else if (pump.running) {
    telInterval = 100;
  }
  if (nowMillis - lastSendMillis >= telInterval) {
    lastSendMillis = nowMillis;
    sendTelemetry();
  }

  // ── 6. Periodic EEPROM Blackbox Save (every 10 s) ───────────
  if (nowMillis - lastEepromSave >= 10000 && pump.running) {
    lastEepromSave = nowMillis;
    pendingEepromSave = true;
  }

  // Execute deferred EEPROM operations in main loop to avoid WDT panic
  if (pendingEepromSave) {
    pendingEepromSave = false;
    saveBlackBox();
  }
  if (pendingEepromClear) {
    pendingEepromClear = false;
    clearBlackBox();
  }
}
