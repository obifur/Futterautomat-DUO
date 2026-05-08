Das Gerät gibt es in mehreren Ausführungen. Dieses Modell ist von	GROOKPET und hat im Originalzustand KEIN Wifi, diese Funtionalität kommt erst mit dem ESP32 hinzu. Dies wurde bewusst gewählt, weil der Futterautomat recht günstig ist und jede Verknüpfung mit einer anderen Cloud als die eigene vermieden werden soll.

# Eigenschaften dieses Modells (GROOKPET):
- 5-Liter Edelstahl-Trockenfutterbehälter
- zwei Edelstahlnäpfe
- Steuerung: Im originalzustand können am Gerät 4 Mahlzeiten über ien kleine LCD-Display programmiert werden
- Es gibt einen eigenen Knopf für manuelles Füttern, unabhängig der Programmierung (Wichtig! Dieser wird zur Steuerung genutzt)
- Eine Tastensperre aktiviert sich automatisch


<img width="500" height="305" alt="grafik" src="https://github.com/user-attachments/assets/c48a7fcc-170a-4010-a893-9bbd7b5a7fe9" />



Dieser Futterautomat wird in ähnlichen Ausführungen unter verschiedenen Markennamen vertrieben. U.a über Amazon oder Media Markt.
Brands: YUGUPAM, VIVIPAL, PELUVIO, Qpets, Gimars


# Eigenschaften des "Futterautomat DUO (ESP32)" nach dem Umbau
- MQTT (WLAN) gesteuert (z.B. über Home Assistant)
- manuelles Füttern integriert unter Umgehung der Tastensperre
- Watchdog + FSM
- Serial Protokoll Reverse Engineered
