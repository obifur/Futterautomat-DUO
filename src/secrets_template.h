#ifndef SECRETS_H
#define SECRETS_H

/* ===== WLAN ===== */
#define WIFI_SSID        "myWifi"
#define WIFI_PASSWORD    "password"

/* ===== MQTT ===== */
#define MQTT_HOST        "192.168.x.x"
#define MQTT_PORT        1883   // ggf. 8883 für TLS

#define MQTT_CLIENT_ID   "Futtertempel-DUO"
#define MQTT_USERNAME    "user"
#define MQTT_PASSWORD    "password"

/* ===== MQTT TOPICS ===== */
#define MQTT_TOPIC_STATUS "futtertempel/duo/status"
#define MQTT_TOPIC_EVENT  "futtertempel/duo/event"
#define MQTT_TOPIC_SUB    "futtertempel/duo/cmd"

/* ===== OPTIONAL ===== */
#define MQTT_LWT_MSG      "offline"   // Last Will

/* Debug / Environment */
#define DEVICE_NAME      "Futtertempel-DUO"
#define DEVICE_LOCATION  "home"

#endif // SECRETS_H