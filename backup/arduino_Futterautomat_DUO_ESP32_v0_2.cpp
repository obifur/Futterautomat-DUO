/* letzte Änderung: 12.03.2023 by Manuel Seiwald
 * This ESP32 code is created by esp32io.com
 * For more detail (instruction and wiring diagram), visit https://esp32io.com/tutorials/esp32-mqtt
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ++++++ Wifi setup ++++++++
const char WIFI_SSID[] = "obiWlan";     // CHANGE TO YOUR WIFI SSID
const char WIFI_PASSWORD[] = "Astrasemperstube213";  // CHANGE TO YOUR WIFI PASSWORD

//+++++++ MQTT Setup ++++++++
const char MQTT_BROKER_ADRRESS[] = "192.168.10.10";  // CHANGE TO MQTT BROKER'S ADDRESS
const int MQTT_PORT = 1883;
const char MQTT_CLIENT_ID[] = "Futtertempel-DUO";  // CHANGE IT AS YOU DESIRE
const char MQTT_USERNAME[] = "manu";                        // CHANGE IT IF REQUIRED, empty if not required
const char MQTT_PASSWORD[] = "obi123";                        // CHANGE IT IF REQUIRED, empty if not required

// The MQTT topics that ESP32 should publish/subscribe
const char PUBLISH_TOPIC[] = "Futtertempel-DUO/from/feeder";    // CHANGE IT AS YOU DESIRE
const char SUBSCRIBE_TOPIC[] = "Futtertempel-DUO/to/feeder";    // CHANGE IT AS YOU DESIRE

const int PUBLISH_INTERVAL = 5000;  // 5 seconds

WiFiClient network;
PubSubClient mqtt(network);


// ++++++++ esp32 PIN ++++++++
//const int led_intern = 2;      // the number of the LED_BUILTIN pin: LOW=on 
const int turnPin = 21;        // D21 Kontakt schließt und öffnet wieder am Ende 1x Portion (Kontakt am rotierenden Portionierrad)
const int feedPin = 22;        // D22 1x Portion wird ausgegeben
const int keyLockPin = 23;     // D23 Tastensperre des feedPin. Muss gelöst werden bevor gefüttert werden kann

//const int adc_in = 36;       //D4 - ADC Poti

// ++++++++ allg variablen ++++++++
unsigned long lastReconnectAttempt = 0;
const unsigned long mqttReconnectInterval = 5000; // 5 Sekunden

unsigned long lastWiFiCheck = 0;
const unsigned long wifiCheckInterval = 5000; // 5 Sekunden

unsigned long lastAlive = 0;
const unsigned long aliveInterval = 30000; // 60 Sekunden

unsigned long lastBlink = 0;
const unsigned long blinkInterval = 2000; // 2 Sekunden

unsigned long unlockTime = 0;
const unsigned long unlockInterval = 2500; // 2 Sekunden

bool restart = 0;             // 1 = Restart device

String inputString = "";      // a String to hold incoming data

bool stringComplete = false;  // whether the string is complete
bool msgComplete = false;

uint8_t rxBytes[64];              // Platz für empfangene Bytes
int rxLen = 0;                    // tatsächliche Länge

unsigned long lastByteTime = 0;
const unsigned long MSG_TIMEOUT = 30;  // ms Pause = Nachricht fertig

int portion = 0;

// Forward declarations
void setup();
void loop();
void unlock();
void feed(int portion);
void handleCommand(String command, String arg);
void messageReceived(char* topic, byte* payload, unsigned int length);
void serialEvent();
void handleSerial2();
void sendToMQTT(String content, String type);
void sendStatus();
void checkWiFi();
bool mqttReconnect();

void setup() {
  Serial.begin(9600);
  Serial2.begin(9600, SERIAL_8N1, 16, 17);  // RX=16, TX=17
  
  analogSetAttenuation(ADC_11db);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("ESP32 - Connecting to Wi-Fi");

  //+++++++ PIN konfig ++++++
  pinMode(keyLockPin, OUTPUT);
  digitalWrite(keyLockPin, LOW);   // PIN 23 HIGH == 3,3V //INPUT_PULLDOWN Pin ist nur intern auf 3,3V gezogen

  pinMode(feedPin, OUTPUT);         // PIN 22
  digitalWrite(feedPin, LOW);

  pinMode(turnPin, INPUT);

  inputString.reserve(200);

  //+++++++ MQTT +++++++++++
  mqtt.setServer(MQTT_BROKER_ADRRESS, MQTT_PORT);
  mqtt.setCallback(messageReceived);

  // Erster Verbindungsversuch
  mqttReconnect();
}

void loop() {
  unsigned long now = millis();

  // WiFi prüfen
  if (now - lastWiFiCheck > wifiCheckInterval) {
    lastWiFiCheck = now;
    checkWiFi();
  }

  // MQTT prüfen
  if (!mqtt.connected()) {
    if (now - lastReconnectAttempt > mqttReconnectInterval) {
      lastReconnectAttempt = now;
      mqttReconnect();
    }
  } else {
    mqtt.loop();
  }

  // Status alle n Sekunden senden
  if (now - lastAlive >= aliveInterval) {
    lastAlive = now;
    sendStatus();
  }

  // Serial Input Handling
  if (stringComplete) {
    Serial.println(inputString);
    inputString = "";
    stringComplete = false;
  }

  // Serial2 prüfen
  handleSerial2();
/* 
  if (msgComplete) {
    msgComplete = false;

    Serial.print("Empfangen (");
    Serial.print(rxLen);
    Serial.println(" Bytes):");

    for (int i = 0; i < rxLen; i++) {
        Serial.printf("%02X ", rxBytes[i]);
    }
    Serial.println();

    // Beispiel: Vergleiche bestimmte Bytes
    if (rxLen >= 4 && rxBytes[0] == 0xE2 && rxBytes[3] == 0xF4) {
        Serial.println("Header OK");
    }
    if (rxLen == 4) {
      Serial.println("sind 4");
    }

    // Buffer zurücksetzen
    rxLen = 0;
  }*/
}

void unlock () {
  unsigned long unlockStart = millis();
  unlockTime = millis() - unlockStart;

  Serial.print(unlockTime);
  Serial.println(" -> unlock Start..");
  while (unlockTime < unlockInterval) {
    digitalWrite(keyLockPin, HIGH);            // Knopf gedrückt halten bis Tastensperre gelöst ist - ca 2s
    unlockTime = millis() - unlockStart;
  }
  digitalWrite(keyLockPin, LOW);             // Knopf wieder loslassen - jetzt ist Tastensperre für ca. 20s offen
  Serial.print(unlockTime);
  Serial.println(" ...lasse los");
  sendToMQTT("unlock completed", "feedback");
  return; 
}

void feed(int portion) {
  String requested = "Portionen angefordert: " + String(portion);
  Serial.println(requested);
  sendToMQTT(requested, "feedback");

  for (int currentPortion = 1; currentPortion <= portion; currentPortion++) {
    Serial.print("Portion ");
    Serial.print(currentPortion);
    Serial.println(" starten...");

    rxLen = 0;
    msgComplete = false;

    // Erstes Ausgangssignal für die Portion
    digitalWrite(feedPin, HIGH);
    delay(100);
    digitalWrite(feedPin, LOW);
    delay(100);

    unsigned long startWait = millis();
    bool portionStarted = false;

    while (millis() - startWait < 700) {
      handleSerial2();
      
      if (rxLen == 7) {         // die Fütterung hat gestartet
        portionStarted = true;
        String msg = "Portion " + String(currentPortion) + " start";
        Serial.println(msg);
        sendToMQTT(msg, "feedback");
        rxLen = 0;
        break;
      }
    }

    // Zweiter Versuch, falls die Portion nicht gestartet ist - wahrscheinlich wegen Tastensperre
    if (!portionStarted) {
      String msg = "Taste entsperren";
      Serial.println(msg);
      sendToMQTT(msg, "feedback");
      unlock();

      // nach dem Entsperren die Portion erneut starten
      rxLen = 0;
      msgComplete = false;
      digitalWrite(feedPin, HIGH);
      delay(100);
      digitalWrite(feedPin, LOW);
      delay(100);

      startWait = millis();
      while (millis() - startWait < 700) {
        handleSerial2();

        if (rxLen == 7) {     // die Fütterung hat gestartet
          portionStarted = true;
          String msg = "Portion " + String(currentPortion) + " start";
          Serial.println(msg);
          sendToMQTT(msg, "feedback");
          rxLen = 0;
          break;
        }
      }
    }

    // Wenn die Portion immer noch nicht gestartet ist, Fehler melden und abbrechen
    if (!portionStarted) {
      String msg = "Fehler: Fütterung konnte nicht gestartet werden";
      Serial.println(msg);
      sendToMQTT(msg, "feedback");
      return;
    }

    unsigned long endWait = millis();
    bool portionFinished = false;

    while (millis() - endWait < 8000) {
      handleSerial2();

      if (rxLen == 3) {     // Fütterung erfolgreich beendet
        portionFinished = true;
        String msg = "Portion " + String(currentPortion) + " Ende";
        Serial.println(msg);
        rxLen = 0;
        break;
      }
    }

    // Wenn die Portion nicht innerhalb von 8 Sekunden fertig ist, Fehler melden und abbrechen
    if (!portionFinished) {
      String msg = "Fehler: Ende der Portion konnte nicht bestätigt werden";
      Serial.println(msg);
      sendToMQTT(msg, "feedback");
      return;
    }
  }

  String msg = "Erfolgreich " + String(portion) + " Portionen gefüttert";
  Serial.println(msg);
  sendToMQTT(msg, "feedback");
  portion = 0;
}

void handleCommand(String command, String arg) {
  
  if (command == "feedme") {
    portion = arg.toInt();
    feed(portion);
    return;
  }

  if (command == "unlock") {
    unlock ();
    return;
  }

  if (command == "status") {
    sendStatus();
    return;
  }

  Serial.println("Unknown command: " + command);
}

void messageReceived(char* topic, byte* payload, unsigned int length) {
  Serial.println("ESP32 - received from MQTT:");
  Serial.print("- topic: ");
  Serial.println(topic);
  Serial.print("- payload: ");
  
  // Convert payload to String
  String payloadStr = "";
  for (unsigned int i = 0; i < length; i++) {
    payloadStr += (char)payload[i];
  }
  Serial.println(payloadStr);

  // Erstes Leerzeichen suchen
  int spaceIndex = payloadStr.indexOf(' ');

  String command;
  String arg;

  if (spaceIndex == -1) {
    // Kein Argument → kompletter Payload ist der Befehl
    command = payloadStr;
    arg = "";
  } else {
    command = payloadStr.substring(0, spaceIndex);
    arg = payloadStr.substring(spaceIndex + 1);
  }

  command.trim();
  arg.trim();

  Serial.println("Command: " + command);
  Serial.println("Arg: " + arg);

  // Jetzt dispatchen
  handleCommand(command, arg);

}

void serialEvent() {
  while (Serial.available()) {
    // get the new byte:
    char inChar = (char)Serial.read();
    // add it to the inputString:
    inputString += inChar;
    // if the incoming character is a newline, set a flag so the main loop can
    // do something about it:
    if (inChar == '\n') {
      stringComplete = true;
    }
  }
  return;
}

void handleSerial2() {
  while (Serial2.available()) {
    uint8_t b = Serial2.read();

    if (rxLen < sizeof(rxBytes)) {  
      rxBytes[rxLen++] = b;         // Schreibe das empfangene Byte b an Position rxLen, Erhöhe danach rxLen um 1
    }

    lastByteTime = millis();
  }

  // Nachricht fertig, wenn 30ms keine neuen Bytes
  if (rxLen > 0 && millis() - lastByteTime > MSG_TIMEOUT) {
      msgComplete = true;
  }

  if (msgComplete) {
    msgComplete = false;

    Serial.print("Empfangen (");
    Serial.print(rxLen);
    Serial.println(" Bytes):");

    for (int i = 0; i < rxLen; i++) {
        Serial.printf("%02X ", rxBytes[i]);
    }
    Serial.println();
    
    // Buffer zurücksetzen
    rxLen = 0;
  }
}

void sendToMQTT(String content, String type = "feedback") {
  DynamicJsonDocument message(256);

  message["timestamp"] = millis();
  message["type"] = type;
  message["data"] = content;

  char messageBuffer[512];
  serializeJson(message, messageBuffer);

  mqtt.publish(PUBLISH_TOPIC, messageBuffer);

  Serial.println("ESP32 - sent to MQTT:");
  Serial.print("- topic: ");
  Serial.println(PUBLISH_TOPIC);
  Serial.print("- payload:");
  Serial.println(messageBuffer);
}

void sendStatus() {
  DynamicJsonDocument status(256);

  status["timestamp"] = millis();
  status["wifi_rssi"] = WiFi.RSSI();
  status["feedPin"] = digitalRead(feedPin);
  status["turnPin"] = digitalRead(turnPin);
  

  char buffer[256];
  serializeJson(status, buffer);

  mqtt.publish(PUBLISH_TOPIC, buffer);

  Serial.println("ESP32 - Status sent:");
  Serial.println(buffer);

}

void checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost → reconnecting...");
    WiFi.reconnect();
  }
}

bool mqttReconnect() {
  Serial.println("MQTT reconnect...");

  if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD)) {
    Serial.println("MQTT connected");
    mqtt.subscribe(SUBSCRIBE_TOPIC);
    return true;
  }

  return false;
}
