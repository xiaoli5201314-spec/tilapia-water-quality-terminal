/* Public configuration template. Use your own network settings for deployment. */
#ifndef NETWORK_CONFIG_H
#define NETWORK_CONFIG_H

#define WATER_WIFI_SSID                  "YOUR_WIFI_SSID"
#define WATER_WIFI_PASSWORD              "YOUR_WIFI_PASSWORD"
#define WATER_MQTT_HOST                  "192.0.2.10"
#define WATER_MQTT_PORT                  1883U
#define WATER_MQTT_USER                  "YOUR_MQTT_USER"
#define WATER_MQTT_PASSWORD              "YOUR_MQTT_PASSWORD"
#define WATER_ENABLE_4G                  0U
#define WATER_SAMPLE_INTERVAL_MS         5000U
#define WATER_PUBLISH_INTERVAL_MS        3600000U
#define WATER_SENSOR_WARMUP_MS           10000U
#define WATER_LINK_HEALTH_MS             30000U
#define WATER_WIFI_PROBE_MS              60000U
#define WATER_MQTT_PUBLISH_TIMEOUT_MS     10000U

#endif
