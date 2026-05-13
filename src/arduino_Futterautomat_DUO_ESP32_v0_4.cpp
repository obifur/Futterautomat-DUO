#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "esp_task_wdt.h"

#include "secrets.h"

/* ===================== PINS ===================== */
constexpr uint8_t FEED_PIN = 22;
constexpr uint8_t LOCK_PIN = 23;

/* ===================== TIMING ===================== */
constexpr unsigned long FEED_PULSE_DURATION_MS    = 100;
constexpr unsigned long FEED_START_ACK_TIMEOUT_MS = 800;
constexpr unsigned long UNLOCK_PRESS_DURATION_MS  = 2600;
constexpr unsigned long SERIAL_MSG_TIMEOUT_MS     = 30;

constexpr int UNLOCK_RETRY_MAX = 2;
constexpr int WATCHDOG_TIMEOUT_SEC = 10;

/* ===================== MQTT ===================== */
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
bool mqttWasConnected = false;
unsigned int failCount = 0;

String eventQueue[10];
int head = 0;
int tail = 0;

/* ===================== SERIAL2 ===================== */
uint8_t rxBytes[64];
uint8_t rxLen = 0;
bool msgComplete = false;
unsigned long lastByteTime = 0;

bool msgDebug = false;

/* ===================== PORTION FLAGS ===================== */
bool portionStarted  = false;
bool portionFinished = false;

/* ===================== FEED CONTROL ===================== */
int portionTarget  = 0;
int portionCurrent = 0;
int unlockRetries  = 0;
unsigned long feedCount = 0;
bool isStart = false;

/* ===================== FSM ===================== */
enum FeedState {
  IDLE,
  FEED_SIGNAL,
  FEED_WAIT_START,
  FEED_WAIT_END,
  UNLOCK_PRESS,
  UNLOCK_WAIT,
  UNLOCK_RELEASE,
  FINISHED
};

FeedState state = IDLE;
unsigned long stateStart = 0;

/* ===================== DEVICE STATUS TRACKING ===================== */
enum DeviceStatus {
  STATUS_NONE,
  STATUS_IDLE,        // AF AF 0F F2
  STATUS_LOCK_CHANGED,// 8F 8B 0F A7
  STATUS_FEED_START,  // AB 0F 0F A7 8F 8B 27 - Manuelle Fütterung gestartet
  STATUS_FEED_END     // 0F 0F 2F A7
};

DeviceStatus lastDeviceStatus = STATUS_NONE;

/* ===================== MQTT QUEUE EVENT ===================== */
void queueEvent(const String& msg, const String& type) {
  JsonDocument doc;

  doc["device"] = DEVICE_NAME;
  doc["type"]   = type;
  doc["msg"]    = msg;
  doc["feedCount"] = feedCount; 
  doc["ts"]     = millis();

  if (msgDebug) {
    doc["mqttFailCount"]  = failCount;
  }

  String payload;
  serializeJson(doc, payload);


  eventQueue[head] = payload;
  head = (head + 1) % 10;


  // overflow protection
  if (head == tail) {
    Serial.println("Event Queue overflow!");
    tail = (tail + 1) % 10; // ältestes Element verwerfen
  }
}

/* ===================== MQTT PROCESS EVENT QUEUE ===================== */
void processEventQueue() {

  if (tail == head) return;           // nichts zu senden
  if (!mqtt.connected()) return;      // nicht verbunden

  
  bool ok = mqtt.publish(MQTT_TOPIC_EVENT, eventQueue[tail].c_str());

  if (ok) {
    tail = (tail + 1) % 10;   // nur wenn erfolgreich
  } else {
    failCount++;
    Serial.println("MQTT publish FAILED!");
  }

  delay(2);

}

/* ===================== SERIAL2 EVENT ===================== */
void serial2Event() {
  while (Serial2.available()) {
    if (rxLen < sizeof(rxBytes)) {
      rxBytes[rxLen++] = Serial2.read();
      lastByteTime = millis();
    }
  }
  if (rxLen > 0 && millis() - lastByteTime > SERIAL_MSG_TIMEOUT_MS) {
    msgComplete = true;
  }
}

/* ===================== MESSAGE HANDLING ===================== */
void msgHandling() {
  uint8_t buffer[64];
  int len = rxLen;
  memcpy(buffer, rxBytes, len);
  rxLen = 0;
  msgComplete = false;

  String hex;
  for (int i = 0; i < len; i++) {
    if (buffer[i] < 0x10) hex += "0";
    hex += String(buffer[i], HEX);
    hex += " ";
  }

  Serial.print("Serial2 RX [");
  Serial.print(len);
  Serial.print("]: ");
  Serial.println(hex);

  if (msgDebug && state == IDLE) 
    queueEvent(hex, "debug");

  isStart = false;

  /* ===== Portion ENDE ===== */
  if (len >= 3 &&
    buffer[0] == 0x0F && 
    buffer[1] == 0x0F &&
    buffer[2] == 0x2F) {

    portionFinished = true;
    lastDeviceStatus = STATUS_FEED_END;
    feedCount++;

    Serial.println(">>> PORTION END erkannt");
    Serial.println(feedCount);

  }
    /* ===== Portion START ===== */
  else if (
    (len >= 3 && buffer[0] == 0xAB && buffer[1] == 0x0F && buffer[2] == 0x0F) || 
    (len >= 3 && buffer[0] == 0x8E && buffer[1] == 0x8F && buffer[2] == 0xA3)) {

    // FALL: manuelles Füttern am Gerät (entsperrt)
    if (state == IDLE && portionTarget == 0) {

      queueEvent("Manuelles Füttern am Gerät (entsperrt)", "info");

      portionTarget  = 1;
      portionCurrent = 1;  // sofort 1!

      queueEvent("Portion 1 von 1 gestartet", "info");

      state = FEED_WAIT_END;
      stateStart = millis();

      return;   // wichtig → FSM nicht nochmal triggern
    }

    
    if (portionCurrent < portionTarget) {
      portionStarted = true;
    }

    unlockRetries = 0;
  }

  /* ===== Tastensperre geändert ===== */
  else if (len >= 4 && buffer[2] == 0x0F && buffer[3] == 0xA7) {
    queueEvent("Tastensperre Status geändert", "info");
    lastDeviceStatus = STATUS_LOCK_CHANGED;
  }

  /* ===== AF AF 0F F2 == IDLE ODER Tastendruck bei Sperre ===== */
  else if (len >= 4 && buffer[2] == 0x0F && buffer[3] == 0xF2) {
    
    /* ===== MANUELLER FÜTTER-INTENT ===== */
    if (lastDeviceStatus == STATUS_IDLE &&
        state == IDLE &&
        portionTarget == 0) {

      queueEvent("Manuelles Füttern am Gerät erkannt", "info");

      portionTarget  = 1;
      portionCurrent = 0;
      unlockRetries  = 0;

      portionStarted  = false;
      portionFinished = false;

      state = UNLOCK_PRESS;
      stateStart = millis();
    } else {
    queueEvent("Leerlauf", "info");
    }

    lastDeviceStatus = STATUS_IDLE;
  }  
}

/* ===================== START FEEDING (MQTT) ===================== */
void startFeeding(int portions) {
  portionTarget  = portions;
  portionCurrent = 0;
  unlockRetries  = 0;

  queueEvent("Starte Fütterung: " + String(portionTarget) + " Portion(en)", "cmd");

  state = FEED_SIGNAL;
  stateStart = millis();
}

/* ===================== FSM ===================== */
void feedingStateMachine() {

  switch (state) {

    case IDLE:
      break;

    case FEED_SIGNAL:
      digitalWrite(FEED_PIN, HIGH);
      state = FEED_WAIT_START;
      stateStart = millis();
      break;

    case FEED_WAIT_START:
      if (millis() - stateStart > FEED_PULSE_DURATION_MS) {
        digitalWrite(FEED_PIN, LOW);
      }

      if (portionStarted && portionCurrent < portionTarget) {
        portionStarted = false;
        portionCurrent++;

        queueEvent(
          "Portion " + String(portionCurrent) +
          " von " + String(portionTarget) + " gestartet",
          "info"
        );

        state = FEED_WAIT_END;
        stateStart = millis();
        break;
      }

      if (millis() - stateStart > FEED_START_ACK_TIMEOUT_MS) {
        if (unlockRetries++ >= UNLOCK_RETRY_MAX) {
          queueEvent("Abbruch: Tastensperre nicht freigegeben", "error");
          state = IDLE;
        } else {
          queueEvent("Tastensperre lösen", "info");
          state = UNLOCK_PRESS;
          stateStart = millis();
        }
      }
      break;

    case FEED_WAIT_END:
      if (portionFinished) {
        portionFinished = false;
        state = (portionCurrent >= portionTarget) ? FINISHED : FEED_SIGNAL;
        stateStart = millis();
      }
      break;

    case UNLOCK_PRESS:
      digitalWrite(LOCK_PIN, HIGH);
      state = UNLOCK_WAIT;
      stateStart = millis();
      break;

    case UNLOCK_WAIT:
      if (millis() - stateStart > UNLOCK_PRESS_DURATION_MS) {
        state = UNLOCK_RELEASE;
      }
      break;

    case UNLOCK_RELEASE:
      digitalWrite(LOCK_PIN, LOW);
      state = FEED_SIGNAL;
      stateStart = millis();
      break;

    case FINISHED:
      queueEvent("Fütterung abgeschlossen", "info");
      portionTarget  = 0;
      portionCurrent = 0;
      state = IDLE;
      break;
  }
}

/* ===================== MQTT RX ===================== */
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String cmd;
  for (unsigned int i = 0; i < len; i++) cmd += (char)payload[i];
  cmd.trim();

  Serial.print("MQTT RX [");
  Serial.print(topic);
  Serial.print("] ");
  Serial.println(cmd);

  if (cmd.startsWith("feedme")) {
    int p = cmd.substring(6).toInt();
    if (p > 0 && state == IDLE) startFeeding(p);
  }

  if (cmd == "debug") {    
    msgDebug = !msgDebug;
    queueEvent(msgDebug ? "Debug ein" : "Debug aus", "debug");
  }

  if (cmd == "reset") {
    queueEvent("ESP wird neu gestartet", "cmd");
    Serial.println(">>> ESP RESTART <<<");
    delay(100);             // Nachricht noch senden lassen
    ESP.restart();
  }
}

/* ===================== SETUP ===================== */
void setup() {
  Serial.begin(9600);
  Serial2.begin(9600);

  pinMode(FEED_PIN, OUTPUT);
  pinMode(LOCK_PIN, OUTPUT);

  Serial.println("=== Futtertempel DUO startet ===");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWLAN verbunden");

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);
  mqtt.setBufferSize(512);

  esp_task_wdt_init(WATCHDOG_TIMEOUT_SEC, true);
  esp_task_wdt_add(NULL);

  Serial.println("Watchdog aktiv");
}

/* ===================== LOOP ===================== */
void loop() {

  /* ===== WATCHDOG ===== */
  esp_task_wdt_reset();

  /* ===== MQTT VERBINDUNG ===== */
  if (!mqtt.connected()) {

    if (mqttWasConnected) {
          mqttWasConnected = false;
          Serial.println("MQTT Verbindung verloren");          
        }

    if (mqtt.connect(
          MQTT_CLIENT_ID,
          MQTT_USERNAME,
          MQTT_PASSWORD,
          MQTT_TOPIC_STATUS,
          1,
          true,
          MQTT_LWT_MSG)) {

      mqtt.subscribe(MQTT_TOPIC_SUB);

      Serial.println("MQTT verbunden");
      queueEvent("MQTT neu verbunden", "status");
    
      mqttWasConnected = true;
      mqtt.publish(MQTT_TOPIC_STATUS, "online", true);
    }
  }

  /* ===== MQTT LOOP ===== */
  mqtt.loop();

  /* ===== SEND MQTT QUEUE ===== */
  processEventQueue();

  /* ===== SERIAL2 EMPFANG vom Futterautomat ===== */
  serial2Event();

  if (msgComplete) 
    msgHandling();

  /* ===== FSM ===== */
  feedingStateMachine();


  /* ===== WLAN / TCP YIELD ===== */
  delay(1);

}
