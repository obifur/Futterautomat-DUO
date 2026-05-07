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
const unsigned long mqttReconnectInterval = 8000; // 5 Sekunden

unsigned long lastWiFiCheck = 0;
const unsigned long wifiCheckInterval = 8000; // 5 Sekunden

unsigned long lastAlive = 0;
const unsigned long aliveInterval = 30000; // 60 Sekunden

unsigned long unlockStart = 0;
const unsigned long unlockInterval = 2500; // 2,5 Sekunden
bool unlocking = false;

bool restart = 0;             // 1 = Restart device

String inputString = "";      // a String to hold incoming data

bool stringComplete = false;  // whether the string is complete
bool msgComplete = false;
bool isFeeding = false;
bool portionStarted = false;
bool portionFinished = false;
bool firstAttempt = true;
unsigned long startWait = 0;      // Zeitstempel für Wartezeit nach Start der Portion
unsigned long endWait = 0;


uint8_t rxBytes[64];              // Platz für empfangene Bytes
int rxLen = 0;                    // tatsächliche Länge

unsigned long lastByteTime = 0;
const unsigned long MSG_TIMEOUT = 30;  // ms Pause = Nachricht fertig

int portion = 0;
int currentPortion = 0;

// Forward declarations
void setup();
void loop();
void unlock();
void feed();
void feedingRequested();
void msgHandling();
void commandHandling(String command, String arg);
void msgReceived(char* topic, byte* payload, unsigned int length);
void serialEvent();
void serial2Event();
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
  mqtt.setCallback(msgReceived);

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
  serial2Event();

  // MQTT Command Handling
  if (msgComplete) {
    msgHandling();
  }

  if (isFeeding) {
    feedingRequested();
  }
}

void feedingRequested() {    
  if (unlocking) {
    if (millis() - unlockStart > unlockInterval) {
      String msg = "Taste konnte nicht entsperrt werden ";
      Serial.println(msg);
      sendToMQTT(msg, "feedback");

      unlocking = false;
    }
  }

  if (!portionStarted) {
    if (firstAttempt && millis() - startWait > 500) {
      String msg = "Taste entsperren";
      Serial.println(msg);
      sendToMQTT(msg, "feedback");
      
      firstAttempt = false;
      unlock();

    }

    if (millis() - startWait > 500) {  // Wenn die Portion nicht innerhalb von 500ms gestartet ist, Fehler melden und abbrechen
      String msg = "Fehler: Fütterung konnte nicht gestartet werden";
      Serial.println(msg);
      sendToMQTT(msg, "feedback");

      isFeeding = false;
      portion = 0;
    }
  } 
  if (portionStarted && !portionFinished) {
    if (millis() - endWait > 8000) {  // Wenn die Portion nicht innerhalb von 8 Sekunden fertig ist, Fehler melden und abbrechen
      String msg = "Fehler: Ende der Portion konnte nicht bestätigt werden";
      Serial.println(msg);
      sendToMQTT(msg, "feedback");

      isFeeding = false;
      portion = 0;
    }
  }    
}

void msgHandling() {

  // Nachricht kopieren, bevor sie überschrieben werden kann
  uint8_t buffer[64];
  int len = rxLen;
  memcpy(buffer, rxBytes, len);

  // Jetzt erst den globalen Buffer freigeben
  rxLen = 0;
  msgComplete = false;

  // Debug-Ausgabe
  Serial.print("Empfangen (");
  Serial.print(len);
  Serial.println(" Bytes):");

  for (int i = 0; i < len; i++) {
    Serial.printf("%02X ", buffer[i]);
  }
  Serial.println();

  // --- Ab hier NUR buffer[] und len verwenden ---

  // Startsignal (>=7 Bytes)
  if (len >= 7) {
    portionStarted = true;
    endWait = millis();

    if (isFeeding) {
      String msg = "Portion " + String(currentPortion) + " start";
      Serial.println(msg);
      sendToMQTT(msg, "feedback");
    } else {
      String msg = "Manuelle Fütterung Start";
      Serial.println(msg);
      sendToMQTT(msg, "feedback");
    }
  }

  // Tastensperre
  if (len == 4 && buffer[2] == 0x0F && buffer[3] == 0xA7) {
    String msg = "Tastensperre betätigt";
    Serial.println(msg);
    sendToMQTT(msg, "feedback");
    return;
  }

  // Endsignal (4 Bytes)
  if (len == 4) {
    portionFinished = true;

    if (isFeeding) {
      String msg = "Portion " + String(currentPortion) + " Ende";
      Serial.println(msg);

      if (firstAttempt) {
        firstAttempt = false;
      }

      currentPortion++;
      feed();
    } else {
      String msg = "Manuelle Fütterung Ende";
      Serial.println(msg);
      sendToMQTT(msg, "feedback");
    }
  }
}

void unlock () {
  rxLen = 0; 

  if (!unlocking) {
    Serial.print(millis());
    Serial.println(" -> unlock Start..");

    digitalWrite(keyLockPin, HIGH);            // Knopf gedrückt halten bis Tastensperre gelöst ist - ca 2s
  
    unlockStart = millis();
    unlocking = true;

  } else {
  digitalWrite(keyLockPin, LOW);               // Knopf wieder loslassen - jetzt ist Tastensperre für ca. 20s offen
  Serial.print(millis());
  Serial.println(" ...lasse los");
  sendToMQTT("unlock completed", "feedback");

  unlocking = false;
  feed();
  }   
}

void feed() {
  
  // Erster Versuch der Fütterung
  if (firstAttempt) {
    
    Serial.println("1. Versuch ");    

    // Erstes Ausgangssignal für die Portion
    digitalWrite(feedPin, HIGH);
    delay(100);
    digitalWrite(feedPin, LOW);
    delay(100);

    portionStarted = false;
    startWait = millis(); 

  }

  if (currentPortion <= portion) {
    Serial.print("Portion ");
    Serial.print(currentPortion);
    Serial.println(" starten...");

    // Erstes Ausgangssignal für die Portion
    digitalWrite(feedPin, HIGH);
    delay(100);
    digitalWrite(feedPin, LOW);
    delay(100);

    portionStarted = false;   
    startWait = millis();  

  } else {
    String msg = "Erfolgreich " + String(portion) + " Portionen gefüttert";
    Serial.println(msg);
    sendToMQTT(msg, "feedback");

    portion = 0;    
    currentPortion = 0;
    isFeeding = false;
  }
}

void commandHandling(String command, String arg) {
  
  if (command == "feedme") {    
    isFeeding = true;
    firstAttempt = true;
    currentPortion = 1;
    portion = arg.toInt();
    
    String requested = "Portionen angefordert: " + String(portion);
    Serial.println(requested);
    sendToMQTT(requested, "feedback");
    
    feed();
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

void msgReceived(char* topic, byte* payload, unsigned int length) {
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
  commandHandling(command, arg);

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

void serial2Event() {
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

  /* if (msgComplete) {
    msgComplete = false;

    Serial.print("Empfangen (");
    Serial.print(rxLen);
    Serial.println(" Bytes):");

    for (int i = 0; i < rxLen; i++) {
        Serial.printf("%02X ", rxBytes[i]);
    }
    Serial.println();
    
    // Buffer zurücksetzen
    // rxLen = 0;
  } */
}

void sendToMQTT(String content, String type = "feedback") {
  DynamicJsonDocument message(256);

  message["timestamp"] = millis();
  message["type"] = type;
  message["data"] = content;

  char messageBuffer[512];
  serializeJson(message, messageBuffer);

  mqtt.publish(PUBLISH_TOPIC, messageBuffer);

/*   Serial.print("ESP32 - message: ");
  Serial.println(messageBuffer); */
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

  Serial.print("ESP32 - Status: ");
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
