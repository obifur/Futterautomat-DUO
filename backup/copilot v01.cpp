#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

/* ===================== CONFIG ===================== */
const char WIFI_SSID[]     = "obiWlan";
const char WIFI_PASS[]     = "Astrasemperstube213";

const char MQTT_HOST[]     = "192.168.10.10";
const int  MQTT_PORT       = 1883;
const char MQTT_CLIENT[]   = "Futtertempel-DUO";
const char MQTT_USER[]     = "manu";
const char MQTT_PASS[]     = "obi123";

const char TOPIC_PUB[]     = "Futtertempel-DUO/from/feeder";
const char TOPIC_SUB[]     = "Futtertempel-DUO/to/feeder";

/* ===================== PINS ===================== */
constexpr uint8_t PIN_FEED = 22;
constexpr uint8_t PIN_LOCK = 23;

/* ===================== MQTT ===================== */
WiFiClient wifi;
PubSubClient mqtt(wifi);

/* ===================== SERIAL ===================== */
uint8_t rxBuf[64];
uint8_t rxLen = 0;
unsigned long lastByteTime = 0;
constexpr unsigned long MSG_TIMEOUT = 30;

/* ===================== FSM ===================== */
enum FeedState {
  IDLE,
  UNLOCKING,
  FEED_TRIGGER,
  WAIT_FOR_PORTION,
  FEED_DONE,
  ERROR_STATE
};

FeedState state = IDLE;
unsigned long stateStart = 0;

/* ===================== FEEDING ===================== */
int targetPortions = 0;
int currentPortion = 0;
bool feedInitiatedByMqtt = false;
bool unlockAttempted = false;

/* ===================== UTILS ===================== */
void sendMQTT(const char* msg, const char* type = "feedback") {
  StaticJsonDocument<256> doc;
  doc["ts"] = millis();
  doc["type"] = type;
  doc["msg"] = msg;

  char out[256];
  serializeJson(doc, out);
  mqtt.publish(TOPIC_PUB, out);
}

void setState(FeedState s) {
  state = s;
  stateStart = millis();
}

/* ===================== SERIAL2 ===================== */
void serial2Event() {
  while (Serial2.available()) {
    if (rxLen < sizeof(rxBuf)) {
      rxBuf[rxLen++] = Serial2.read();
      lastByteTime = millis();
    }
  }

  if (rxLen > 0 && millis() - lastByteTime > MSG_TIMEOUT) {

    /* ========= START OF FEED (= len >= 7) ========= */
    if (rxLen >= 7) {
      if (!feedInitiatedByMqtt && state == IDLE) {
        sendMQTT("manuelle Fütterung gestartet", "manual");
      }
    }

    /* ========= FEED FINISHED (= Abschluss) ========= */
    if (rxLen <= 4) {
      if (!feedInitiatedByMqtt && state == IDLE) {
        sendMQTT("manuelle Fütterung abgeschlossen", "manual");
      } 
      else if (feedInitiatedByMqtt) {
        setState(FEED_DONE);
      }
    }

    rxLen = 0;
  }
}

/* ===================== FSM LOGIC ===================== */
void handleFSM() {
  switch (state) {

    case IDLE:
      break;

    case UNLOCKING:
      digitalWrite(PIN_LOCK, HIGH);
      if (millis() - stateStart > 2500) {
        digitalWrite(PIN_LOCK, LOW);
        setState(FEED_TRIGGER);
      }
      break;

    case FEED_TRIGGER:
      digitalWrite(PIN_FEED, HIGH);
      delay(200);
      digitalWrite(PIN_FEED, LOW);
      unlockAttempted = false;
      setState(WAIT_FOR_PORTION);
      break;

    case WAIT_FOR_PORTION:
      if (millis() - stateStart > 2000 && !unlockAttempted) {
        unlockAttempted = true;
        sendMQTT("keine Portion erkannt – entsperre", "info");
        setState(UNLOCKING);
      }
      else if (millis() - stateStart > 8000) {
        sendMQTT("Timeout – Portion fehlgeschlagen", "error");
        setState(ERROR_STATE);
      }
      break;

    case FEED_DONE:
      currentPortion++;
      if (currentPortion >= targetPortions) {
        sendMQTT("Fütterung abgeschlossen");
        feedInitiatedByMqtt = false;
        setState(IDLE);
      } else {
        setState(FEED_TRIGGER);
      }
      break;

    case ERROR_STATE:
      feedInitiatedByMqtt = false;
      setState(IDLE);
      break;
  }
}

/* ===================== MQTT ===================== */
void mqttCallback(char*, byte* payload, unsigned int len) {
  String cmd;
  for (unsigned i = 0; i < len; i++) cmd += (char)payload[i];
  cmd.trim();

  if (cmd.startsWith("feedme")) {
    targetPortions = cmd.substring(7).toInt();
    currentPortion = 0;
    feedInitiatedByMqtt = true;
    unlockAttempted = false;
    sendMQTT("MQTT-Fütterung gestartet", "cmd");
    setState(UNLOCKING);
  }
}

/* ===================== SETUP ===================== */
void setup() {
  Serial.begin(9600);
  Serial2.begin(9600, SERIAL_8N1, 16, 17);

  pinMode(PIN_FEED, OUTPUT);
  pinMode(PIN_LOCK, OUTPUT);

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
}

/* ===================== LOOP ===================== */
void loop() {
  if (!mqtt.connected() && WiFi.status() == WL_CONNECTED) {
    mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS);
    mqtt.subscribe(TOPIC_SUB);
  }

  mqtt.loop();
  serial2Event();
  handleFSM();
}
