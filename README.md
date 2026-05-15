Das Gerät gibt es in mehreren Ausführungen. Dieses Modell ist von	GROOKPET und hat im Originalzustand KEIN Wifi, diese Funtionalität kommt erst mit dem ESP32 hinzu. Dies wurde bewusst gewählt, weil der Futterautomat recht günstig ist und jede Verknüpfung mit einer anderen Cloud als die eigene vermieden werden soll.

# Eigenschaften dieses Modells (GROOKPET):
- 5-Liter Edelstahl-Trockenfutterbehälter
- zwei Edelstahlnäpfe
- Steuerung: Im Originalzustand können am Gerät 4 Mahlzeiten mit jeweils mehreren Portionen über ein kleine LCD-Display programmiert werden
- Es gibt einen eigenen Knopf für manuelles Füttern, unabhängig der Programmierung (Wichtig! Dieser wird zur Steuerung genutzt)
- Eine Tastensperre aktiviert sich automatisch


<img width="400" src="https://github.com/user-attachments/assets/c48a7fcc-170a-4010-a893-9bbd7b5a7fe9" /> <img width="300" src="https://github.com/user-attachments/assets/3b3893e1-82e5-4945-b8b4-9e95f3644ba3" />



Dieser Futterautomat wird in ähnlichen Ausführungen unter verschiedenen Markennamen vertrieben. U.a über Amazon oder Media Markt.
Brands: YUGUPAM, VIVIPAL, PELUVIO, Qpets, Gimars


# Eigenschaften des "Futterautomat DUO (ESP32)" nach dem Umbau
- MQTT (WLAN) gesteuert (z.B. über Home Assistant)
- manuelles und Timerbasierte Fütterung am Gerät berücksichtigt
- Watchdog + FSM
- Serial Protokoll (teilweise) Reverse Engineered


## MQTT TOPICS 
| Topic | value | payload |
| --- | --- | --- |
| MQTT_TOPIC_STATUS | "futtertempel/duo/status" | Statusmeldung: "online" oder "MQTT neu verbunden" |
| MQTT_TOPIC_EVENT  | "futtertempel/duo/event"  | Allg. Meldungen (JSON formatiert) für: "device" , "type", "msg", "feedCount", "ts", "mqttSendFailCount", "mqttReconnectCount", "WifiReconnectCount", "wifi_rssi"
| MQTT_TOPIC_SUB    | "futtertempel/duo/cmd"    | Befehle an den Automanten:<br/> "feedme" + Zahl: Fütterbefehl (z.B. "feedme3" oder "feedme 3")<br/> "debug": lässt zusätzlich die HEX Rückgabewerte ausgeben<br/> "reset":  ESP wird neu gestartet

Beispiel für ein MQTT_TOPIC_EVENT:
```
{
    "device": "Futtertempel-DUO",
    "type": "info",
    "msg": "Portion 2 von 3 gestartet",
    "feedCount": 56,
    "ts": 2547494,
    "mqttSendFailCount": 0,
    "mqttReconnectCount": 0,
    "WifiReconnectCount": 2,
    "wifi_rssi": -56
}
```
Hinweise: 
- Es werden alle ausgegebene Portionen erfasst - also neben den über die MQTT befehligten Portionen auch die manuell ausgelösten Fütterungen am Gerät selbst oder auch die am Gerät programmierten Mahlzeiten.
- Weil die Tastensperre für die manuelle Fütterung echt nervt, lasse ich sie nach einem Knopfdruck auf die tastensperre oder Fütterungstaste selbsttätig entsperren und die Fütterung ausführen. Mit dem Registrieren des Knopfdrucks und der Entsperrung dauert das allerdings ~4 s bis zum Start der Portionsausgabe.

# Hardware Modifikation
Im konkreten fall wurde ein ESP-32 NodeMCU Development Board DevKit C V4 benutzt - andere ESP32 Boards funktionieren aber meist ohne Anpassung genauso.
Die Knöpfe "Fütterung" und "Tastensperre" werden jeweil über ein NPN Transistor (hier BC 637) an die Tasten angeschlossen (mit z.B. 1 kΩ an Basis) und zum Auslösen auf 0 V geschaltet. Außerdem musste der Pin 16 (RX) über einen Pull‑Up (z. B. 10 kΩ auf 3.3 V), damit er einen definierten High‑Pegel hat.

<img width="300" src="https://github.com/user-attachments/assets/69f8fb02-7251-4d68-a1d1-abd53ba96baf" /> <img width="300" src="https://github.com/user-attachments/assets/2765892e-ed53-420e-8f63-5b740fb1e6fe" />

<img width="300" src="https://github.com/user-attachments/assets/740fa80e-b2a4-460c-bce0-d22ab9a3e772" />
<img width="300" src="https://github.com/user-attachments/assets/05fb560a-4c97-4c4b-b1db-3661623ad422" />


# Details zur Funkitonsweise
## Serielle Schnittstelle
Über die serielle Schnittstelle wird die Rückmeldung nach einem Knopfdruck, am Gerät programmierte Fütterungen oder Zustandsänderungen ausgewertet.

## Manuelle Fütterung am Gerät über Knopfdruck

| HEX  |  Bedeutung |
| --- | --- |
| 8F 8B 0F A7       |       Tastensperre geändert (auf / zu)
| AB 0F 0F A7 8F 8B 27   |  Fütterung gestartet 
| 0F 0F 2F A7       |       Fütterung beendet 
| AF AF 0F F2       |       IDLE, bzw:<br/> - Tastensperre aktiviert nach ~20 s <br/> - Tastenbetätigung bei aktiver Sperre (nach ~2 s) <br/> - Meldung nach manueller Aktivierung der Tastensperre

## Am Gerät programmierte Mahlzeiten
Falls Mahlzeiten am Gerät programmiert werden, sieht das wie folgt aus.

Beispiel:
```
12:01h 1 Portion
Serial2 RX [12]: 8e 8f a3 1f ab a3 68 2b a7 8f 8b 27 -> Portion 1 gestartet 
Serial2 RX [4]:  0f 0f 2f a7                         -> Fütterung beendet

12:02h 2 Portionen
Serial2 RX [12]: 8e 8f a3 1f ab a3 83 2b f2 8f 8b 27 -> Portion 1 gestartet 
Serial2 RX [7]:  0f 0f 2f a7 8f 6b 27                -> Portion 2 gestartet 
Serial2 RX [4]:  0f 0f 2f f2                         -> Fütterung beendet

12:03h 3 Portionen
Serial2 RX [12]: 8e 8f a3 1f ab a3 83 2b f2 8f 6b 27 -> Portion 1 gestartet 
Serial2 RX [7]:  0f 0f 2f a7 8f 8b 27                -> Portion 2 gestartet
Serial2 RX [7]:  0f 0f 2f f2 8f 8b 27                -> Portion 3 gestartet 
Serial2 RX [4]:  0f 0f 2f f2                         -> Fütterung beendet

12:04h 4 Portionen
Serial2 RX [12]: 8e 8f a3 1f ab a3 83 2b a7 8f 8b 27 -> Portion 1 gestartet 
Serial2 RX [7]:  0f 0f 2f a7 8f 8b 27                -> Portion 2 gestartet
Serial2 RX [7]:  0f 0f 2f f2 8f 8b 27                -> Portion 3 gestartet
Serial2 RX [7]:  0f 0f 2f f2 8f 8b 27                -> Portion 4 gestartet 
Serial2 RX [4]:  0f 0f 2f a7                         -> Fütterung beendet 
```

..ein paat Fotos folgen bei Gelegenheit...
