#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "esp_task_wdt.h"
#include "lwip/sockets.h"

#include "secrets.h"

/* ===================== PINS ===================== */
constexpr uint8_t FEED_PIN = 22;
constexpr uint8_t LOCK_PIN = 23;

/* ===================== TIMING ===================== */
constexpr unsigned long FEED_PULSE_DURATION_MS    = 100;
constexpr unsigned long FEED_START_ACK_TIMEOUT_MS = 800;
constexpr unsigned long UNLOCK_PRESS_DURATION_MS  = 2800;
constexpr unsigned long SERIAL_MSG_TIMEOUT_MS     = 30;

constexpr int UNLOCK_RETRY_MAX = 2;
constexpr int WATCHDOG_TIMEOUT_SEC = 10;

/* ===================== MQTT / WIFI ===================== */
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
bool mqttWasConnected = false;
bool mqttEverConnected = false;
bool debugTiming = false;
unsigned long lastReconnectAttempt = 0;   // Throttle -> loop()
unsigned int mqttSendFailCount = 0;
unsigned int mqttReconnectCount = 0;
static unsigned long mqttDisconnectTime = 0;

bool wifiWasConnected = true;
unsigned int wifiReconnectCount = 0;

constexpr int QUEUE_SIZE = 10;            // für debug besser 20 anstatt 10
String eventQueue[QUEUE_SIZE];
int head = 0;
int tail = 0;
static unsigned long lastSend = 0;

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
int portionTimer = 0;
bool feedButtonPressed = false;

/* ===================== FSM ===================== */
enum FeedState {
  IDLE,
  FEED_SIGNAL,
  FEED_WAIT_START,
  FEED_WAIT_END,
  FEED_WAIT_END_TIMER,
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
  STATUS_FEED_START,  // AB 0F 0F A7 8F 8B 27 || 8e 8f a3 1f ab ..[12] || 0f 0f 2f a7 8f [7] : Fütterung gestartet
  STATUS_FEED_END     // 0F 0F 2F A7 || 0f 0f 2f f2 : Fütterung Ende  
};

DeviceStatus lastDeviceStatus = STATUS_NONE;

/* ===================== MQTT EVENT QUEUE ===================== */
void queueEvent(const String& msg, const String& type) {
  static JsonDocument doc;
  doc.clear();

  doc["device"] = DEVICE_NAME;
  doc["type"]   = type;
  doc["msg"]    = msg;
  doc["feedCount"] = feedCount; 
  doc["ts"]     = millis();

  doc["version"] = FW_VERSION;
  doc["build"]   = __DATE__ " " __TIME__;

  doc["mqttSendFailCount"]  = mqttSendFailCount;
  doc["mqttReconnectCount"]  = mqttReconnectCount;
  doc["WifiReconnectCount"]  = wifiReconnectCount;
  
  if (WiFi.status() == WL_CONNECTED) {
    doc["wifi_rssi"] = WiFi.RSSI();
  }



  static char payload[384];
  serializeJson(doc, payload);


  eventQueue[head] = payload;
  head = (head + 1) % QUEUE_SIZE;             // Ringpuffer‑Queue (FIFO): head = Schreibposition ; tail = Leseposition 


  // overflow protection
  if (head == tail) {
    if (msgDebug) Serial.println("Event Queue overflow!");
    tail = (tail + 1) % QUEUE_SIZE;           // ältestes Element verwerfen
  }
}

/* ===================== MQTT PROCESS EVENT QUEUE ===================== */
void processEventQueue() {

  if (tail == head) return;           // nichts zu senden
  if (!mqtt.connected()) return;      // nicht verbunden
  
  bool ok = mqtt.publish(MQTT_TOPIC_EVENT, eventQueue[tail].c_str());

  if (ok) {
    tail = (tail + 1) % QUEUE_SIZE;   // nur wenn erfolgreich
  } else {
    mqttSendFailCount++;
    if (msgDebug) Serial.println("MQTT publish FAILED!");
  }
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

  // ===== DEBUG HEX LOG =====
  if (msgDebug) {
    static unsigned long lastDebug = 0;

    char hex[3 * 64];
    int pos = 0;
    for (int i = 0; i < len && pos < (int)sizeof(hex) - 4; i++) {
      pos += snprintf(&hex[pos], sizeof(hex) - pos, "%02x ", buffer[i]);
    }

    if (millis() - lastDebug > 200) {
      queueEvent(hex, "debug");
      lastDebug = millis();
    }
  }

  /* ==========================================================
   * 1. PORTION ENDE und ggf. WEITERE
   * Pattern: 0F 0F 2F ?? [4] ODER [7]
   * ========================================================== */
  if (len >= 3 &&
    buffer[0] == 0x0F &&
    buffer[1] == 0x0F &&
    buffer[2] == 0x2F) {

    // Portionen zählen
    feedCount++;
    portionTimer++;     // Hilfszähler für TIMER Fütterung über 

    portionFinished = true;
    lastDeviceStatus = STATUS_FEED_END;

    // Timer-Feed: weitere Portionen folgen
    if (len >= 7) {
      queueEvent("Portion " + String(portionTimer +1) + " gestartet (Timer)","info");
    }

    // Letzte Portion erkannt
    if (len == 4) {
      if (portionTarget == 0) {   // nur bei Timer-Fütterung!
        queueEvent("Fütterung abgeschlossen (Timer)", "info");
        state = IDLE; 
      }
      portionTimer = 0;   // reset
    }

    return;   // nichts weiter verarbeiten
  }

  /* ==========================================================
   * 2. START (MQTT RX / manuell)
   * Pattern: ?? 0F 0F A7 8F ?? 27 [7]
   * ========================================================== */
  else if (len == 7 &&
    buffer[1] == 0x0F &&
    buffer[2] == 0x0F &&
    buffer[3] == 0xA7) {

    // manuelle Fütterung direkt am Gerät
    if (state == IDLE && portionTarget == 0) {

      queueEvent("Manuelles Füttern am Gerät (entsperrt)", "info");

      portionTarget  = 1;
      portionCurrent = 1;

      queueEvent("Portion 1 von 1 gestartet (Gerät)", "info");

      state = FEED_WAIT_END;
      stateStart = millis();
      return;
    }

    // MQTT - FSM Feed
    if (portionCurrent < portionTarget) {   
      portionStarted = true;                // Fütterungsbefehl über MQTT
    }

    unlockRetries = 0;
    return;
  }

  /* ==========================================================
   * 3. START Timer / Geräteprogrammierung
   * Pattern: 8E 8F A3 1F AB A3 ?? 2B ?? 8F 8B 27 [12]
   * ========================================================== */
  else if (len == 12 &&
    buffer[1] == 0x8F &&
    buffer[2] == 0xA3 &&
    buffer[3] == 0x1F) {

    // nur wenn noch keine aktive FSM läuft
    if (state == IDLE && portionTarget == 0) {

      portionTimer = 0;

      queueEvent("Portion 1 gestartet (Timer)", "info");

      state = FEED_WAIT_END_TIMER;
      stateStart = millis();
    }

    return;
  }


  /* ==========================================================
   * 4. Tastensperre geändert
   * Pattern: 8F 8B 0F A7
   * ========================================================== */
  else if (len >= 4 &&
    buffer[0] == 0x8F &&
    buffer[1] == 0x8B &&
    buffer[2] == 0x0F &&
    buffer[3] == 0xA7) {

    queueEvent("Tastensperre Status geändert", "info");
    lastDeviceStatus = STATUS_LOCK_CHANGED;
    return;
  }


  /* ==========================================================
   * 5. IDLE / Knopfdruck am Gerät
   * Pattern: AF ?? 0F F2
   * ========================================================== */
  else if (len >= 4 &&
    buffer[0] == 0xAF &&
    buffer[2] == 0x0F &&
    buffer[3] == 0xF2) {

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
    }
    else {
      queueEvent("Leerlauf", "info");
    }

    lastDeviceStatus = STATUS_IDLE;
    return;
  }
}

/* ===================== START FEEDING (MQTT) ===================== */
void startFeeding(int portions) {
  
  portionTarget  = portions;
  portionCurrent = 0;
  unlockRetries  = 0;
  portionStarted  = false;
  portionFinished = false;

  // queueEvent("Starte Fütterung: " + String(portionTarget) + " Portion(en)", "cmd");

  state = FEED_SIGNAL;
  stateStart = millis();
}

/* ===================== FSM ===================== */
void feedingStateMachine() {

  switch (state) {

    case IDLE:
      break;

    case FEED_SIGNAL:
      digitalWrite(FEED_PIN, HIGH);                                 // Fütterung Knopf drücken -> beginnt sofort
      feedButtonPressed = true;
      state = FEED_WAIT_START;
      stateStart = millis();
      break;

    case FEED_WAIT_START:                                           // Fütterung läuft
      
      if (feedButtonPressed && millis() - stateStart > FEED_PULSE_DURATION_MS) {        // Fütterung Knopf loslassen
        digitalWrite(FEED_PIN, LOW);
        feedButtonPressed = false;
      }
      
      // Erste Futterportion angefordert über MQTT
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

      if (portionFinished) {
        portionFinished = false;

        state = FEED_WAIT_END;
        stateStart = millis();
        break;
      }

      // Timeout
      if (!feedButtonPressed && millis() - stateStart > FEED_START_ACK_TIMEOUT_MS) {
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
        state = (portionCurrent >= portionTarget) ? FINISHED : FEED_SIGNAL;   // ggf. weitere Portion füttern
        stateStart = millis();
      }
      break;

    case FEED_WAIT_END_TIMER:
      if (portionFinished) {
        portionFinished = false;
      }
      break;

    case UNLOCK_PRESS:                                    // Tastensperre Knopf drücken
      digitalWrite(LOCK_PIN, HIGH);
      state = UNLOCK_WAIT;
      stateStart = millis();
      break;

    case UNLOCK_WAIT:                                    // Tastensperre Knopf halten
      if (millis() - stateStart > UNLOCK_PRESS_DURATION_MS) {
        state = UNLOCK_RELEASE;
      }
      break;

    case UNLOCK_RELEASE:                                 // Tastensperre Knopf loslassen und Fütterung starten
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
  
  char cmd[32] = {0};
  unsigned int copyLen = min(len, (unsigned int)31);
  memcpy(cmd, payload, copyLen);
  for (int i = copyLen - 1; i >= 0 && isspace((unsigned char)cmd[i]); i--) cmd[i] = 0;

  if (strncmp(cmd, "feedme", 6) == 0) {
    int p = atoi(cmd + 6);
    if (p > 0 && state == IDLE) startFeeding(p);
  }

  if (strcmp(cmd, "info") == 0)
    queueEvent("Info", "info");

  if (strcmp(cmd, "debug") == 0) {
    msgDebug = !msgDebug;
    queueEvent(msgDebug ? "Debug ein" : "Debug aus", "debug");
  }
  if (strcmp(cmd, "debugTiming") == 0) {
    debugTiming = !debugTiming;
    queueEvent(debugTiming ? "Timing Debug ein" : "Timing Debug aus", "debug");
  }

  if (strcmp(cmd, "reset") == 0) {
    queueEvent("ESP wird neu gestartet", "cmd");
    delay(100);
    ESP.restart();
  }
}

/* ===================== SETUP ===================== */
void setup() {
  Serial.begin(9600);
  Serial2.begin(9600);

  pinMode(FEED_PIN, OUTPUT);
  pinMode(LOCK_PIN, OUTPUT);

  esp_task_wdt_init(WATCHDOG_TIMEOUT_SEC, true);
  esp_task_wdt_add(NULL);

  Serial.println("=== Futtertempel DUO startet ===");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false);

  while (WiFi.status() != WL_CONNECTED) {
    esp_task_wdt_reset();
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWLAN verbunden");

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);
  mqtt.setBufferSize(512);

  Serial.println("Watchdog aktiv");
}

/* ===================== LOOP ===================== */
void loop() {

  unsigned long t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0, t5 = 0;

  if (debugTiming) t0 = micros();


  /* ===== WATCHDOG ===== */
  esp_task_wdt_reset();

  if (debugTiming) t1 = micros();


  /* ===== MQTT LOOP ===== */
  mqtt.loop();                        // sofort

  if (debugTiming) t2 = micros();


  /* ===== WIFI STATUS TRACKING ===== */
  if (WiFi.status() == WL_CONNECTED) {    // Wifi ist verbunden

    if (!wifiWasConnected) {
      wifiWasConnected = true;
      wifiReconnectCount++;
      
      if (msgDebug) {
        Serial.println("WIFI neu verbunden");
        queueEvent("WIFI neu verbunden", "debug");
      }
    }
  } 
  else {
    if (wifiWasConnected) {                // Wifi ist nicht verbunden
      wifiWasConnected = false;
    }
  }

  if (debugTiming) t3 = micros();


  /* ===== MQTT VERBINDUNG ===== */
  if (!mqtt.connected()) {

    // merken, wann MQTT verloren ging
    if (mqttDisconnectTime == 0)
      mqttDisconnectTime = millis();

    if (wifiClient.connected() && millis() - mqttDisconnectTime < 3000)
      return;
    
    mqttDisconnectTime = 0;

    if (mqttWasConnected) {
      mqttWasConnected = false;
      if (msgDebug) {
        int state = mqtt.state();
        // -4=TIMEOUT, -3=LOST, -2=FAILED, -1=DISCONNECTED, 1=BAD_PROTOCOL,
        // 2=BAD_CLIENT_ID, 3=UNAVAILABLE, 4=BAD_CREDENTIALS, 5=UNAUTHORIZED
        queueEvent("MQTT verloren, state=" + String(state), "debug");
      }
    }

    if (millis() - lastReconnectAttempt > 10000) { // nicht ständig prüfen -> throttle
      
      lastReconnectAttempt = millis();
    
      if (mqtt.connect(
        MQTT_CLIENT_ID,
        MQTT_USERNAME,
        MQTT_PASSWORD,
        MQTT_TOPIC_STATUS,
        1, true,
        MQTT_LWT_MSG)) {

        // TCP Keepalive direkt nach Connect setzen
        if (wifiClient.connected()) {
            int sock = wifiClient.fd();
            if (sock >= 0) {
                int ka = 1, idle = 15, intvl = 5, cnt = 3;
                setsockopt(sock, SOL_SOCKET,  SO_KEEPALIVE,  &ka,    sizeof(ka));
                setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
                setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
                setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
            }
        }

        mqtt.subscribe(MQTT_TOPIC_SUB);

        if (msgDebug) { 
          queueEvent("MQTT neu verbunden", "debug");
        }

        if (mqttEverConnected) {
            mqttReconnectCount++;
          }

        mqttEverConnected = true;
        mqttWasConnected = true;

        mqtt.publish(MQTT_TOPIC_STATUS, "online", true);
      }
    }
  }

  if (debugTiming) t4 = micros();


  /* ===== SEND MQTT QUEUE ===== */
  if (millis() - lastSend > (msgDebug ? 150 : 50)) {
    processEventQueue();
    lastSend = millis();
  }

  if (debugTiming) t5 = micros();

  /* ===== SERIAL2 EMPFANG vom Futterautomat ===== */
  serial2Event();

  if (msgComplete) 
    msgHandling();


  /* ===== FSM ===== */
  feedingStateMachine();

  /* ===== BACKUP MQTT LOOP ===== */
  static unsigned long lastMqtt = 0;
  if (mqtt.connected() && millis() - lastMqtt > 5) {
    mqtt.loop();                      // garantiert alle 5ms
    lastMqtt = millis();
  }


  /* ===== TIMING REPORT ===== */
  static unsigned long lastTimingReport = 0;

  if (debugTiming && millis() - lastTimingReport > 10000) {
    lastTimingReport = millis();

    char msg[128];
    snprintf(msg, sizeof(msg),
      "t[µs] wd %lu wifi %lu mqttConn %lu mqttLoop %lu mqttQueue %lu",
      t1 - t0,          // watchdog + start
      t2 - t1,          // mqtt.loop
      t3 - t2,          // wifi
      t4 - t3,          // mqtt connect
      t5 - t4           // queue
    );
    
    msg[sizeof(msg) - 1] = '\0';
    queueEvent(msg, "timing");
  }

  /* ===== WLAN / TCP YIELD ===== */
  delay(1);

}
