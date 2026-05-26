#pragma once

// ─────────────────────────────────────────────
//  SAMA Datalogger — Configuración general
//  Broker: EMQX Cloud (TLS puerto 8883)
//  Red:    ESP32 + W5500 Ethernet + RUT200 GSM
// ─────────────────────────────────────────────

// ── Versión del firmware ──────────────────────
// ⚠️ INCREMENTAR antes de cada release OTA
#define FIRMWARE_VERSION        "1.0.0"
#define DEVICE_ID               "Sama"          // Client ID EMQX

// ── Pines W5500 — ESP32 DevKitC V4 ───────────
#define W5500_CS                5
#define W5500_RST               4
#define W5500_SCK               18
#define W5500_MISO              19
#define W5500_MOSI              23

// ── MAC address ───────────────────────────────
#define MAC_ADDR                { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x03 }

// ── EMQX Cloud — TLS 8883 ────────────────────
#define MQTT_SERVER             "p9d17dd1.ala.us-east-1.emqxsl.com"
#define MQTT_PORT               8883
#define MQTT_USER               "Sama"
#define MQTT_PASSWORD           "S4m4_mqtt*"

// Topics
#define MQTT_TOPIC_DATA         "sama/test"
#define MQTT_TOPIC_OTA          "sama/ota/" DEVICE_ID
#define MQTT_TOPIC_OTA_STATUS   "sama/ota/" DEVICE_ID "/status"

// ── OTA — GitHub Releases ────────────────────
// URL del version.json en tu repositorio GitHub.
// Ejemplo: https://raw.githubusercontent.com/ORG/REPO/main/ota/version.json
#define OTA_VERSION_URL_HOST    "raw.githubusercontent.com"
#define OTA_VERSION_URL_PATH    "/TU_ORG/TU_REPO/main/ota/version.json"

// Timeout de descarga OTA en milisegundos
#define OTA_DOWNLOAD_TIMEOUT_MS 120000          // 2 minutos

// ── Tiempos ───────────────────────────────────
#define INTERVALO_MEDICION_MS   5000            // 5 seg para prueba (15 min en campo)
#define MQTT_RECONNECT_DELAY_MS 5000

// ── SSL ───────────────────────────────────────
#define SSL_RX_BUFFER           4096
#define SSL_TX_BUFFER           1024
