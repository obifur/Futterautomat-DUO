#ifndef SECRETS_H
#define SECRETS_H

/* ===== WLAN ===== */
#define WIFI_SSID        "obiWlan"
#define WIFI_PASSWORD    "Astrasemperstube213"

/* ===== MQTT ===== */
#define MQTT_HOST        "192.168.10.10"
#define MQTT_PORT        1883   // ggf. 8883 für TLS

#define MQTT_CLIENT_ID   "Futtertempel-DUO"
#define MQTT_USERNAME    "manu"
#define MQTT_PASSWORD    "obi123"

/* ===== MQTT TOPICS ===== */
#define MQTT_TOPIC_STATUS "futtertempel/duo/status"
#define MQTT_TOPIC_EVENT  "futtertempel/duo/event"
#define MQTT_TOPIC_SUB    "futtertempel/duo/cmd"

/* ===== OPTIONAL ===== */
#define MQTT_LWT_MSG      "offline"   // Last Will

/* Debug / Environment */
#define DEVICE_NAME      "Futtertempel-DUO"
#define DEVICE_LOCATION  "home"
#define FW_VERSION       "1.2.3"

#endif // SECRETS_H