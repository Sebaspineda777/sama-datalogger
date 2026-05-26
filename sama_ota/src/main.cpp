// ═══════════════════════════════════════════════════════════════
//  SAMA Datalogger — Prototipo OTA sobre EMQX Cloud TLS
//
//  Hardware:  ESP32 DevKitC V4 + W5500 + RUT200 GSM
//  Broker:    EMQX Cloud (TLS puerto 8883)
//  OTA:       GitHub Releases vía HTTP
//
//  Basado en el código de comunicación EMQX de SAMA
//  con módulo OTA integrado.
// ═══════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <ESP_SSLClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <MD5Builder.h>

#include "config.h"

// ─────────────────────────────────────────────
//  Clientes de red  (igual que tu código base)
// ─────────────────────────────────────────────
byte           mac[]      = MAC_ADDR;
EthernetClient ethClient;
ESP_SSLClient  sslClient;
PubSubClient   mqttClient(sslClient);

// ─────────────────────────────────────────────
//  Variables de control
// ─────────────────────────────────────────────
unsigned long lastPublish       = 0;
unsigned long lastReconnect     = 0;

// OTA — se dispara desde el callback MQTT
// pero se ejecuta en el loop para no bloquear
// el stack de la librería MQTT.
struct OtaPendiente {
    bool    activo;
    char    version[16];
    char    url_host[128];
    char    url_path[256];
    char    md5[33];
} otaPendiente = { false };

// ─────────────────────────────────────────────
//  Prototipos
// ─────────────────────────────────────────────
void  resetW5500();
bool  initEthernet();
void  reconnectMQTT();
void  mqttCallback(char* topic, byte* payload, unsigned int length);
void  publicarDatos();
void  publicarOtaStatus(const char* status);
void  ejecutarOTA();
void  otaInit();
bool  parsearUrlOta(const char* url, char* host, char* path);

// ─────────────────────────────────────────────
//  setup()
// ─────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("==========================================");
    Serial.println("  SAMA Datalogger — OTA sobre EMQX TLS  ");
    Serial.printf ("  Firmware: v%s | ID: %s\n", FIRMWARE_VERSION, DEVICE_ID);
    Serial.println("  ESP32 DevKitC V4 + W5500 + RUT200 GSM  ");
    Serial.println("==========================================");
    Serial.println();

    // Verificar partición OTA al arrancar — confirma si el
    // firmware nuevo arrancó bien (evita rollback innecesario)
    otaInit();

    // Inicializar red Ethernet
    if (!initEthernet()) {
        Serial.println("[ETH] Error crítico. Sistema detenido.");
        while (true) delay(1000);
    }

    // ── Configurar TLS (igual que tu código base) ──
    sslClient.setClient(&ethClient);
    sslClient.setInsecure();               // Sin validar CA — OK para prototipo
    sslClient.setBufferSizes(SSL_RX_BUFFER, SSL_TX_BUFFER);
    sslClient.setDebugLevel(1);

    // ── Configurar MQTT ────────────────────────────
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(512);

    reconnectMQTT();

    Serial.println("[MAIN] Sistema listo. Iniciando loop...");
    Serial.println("─────────────────────────────────────────");
}

// ─────────────────────────────────────────────
//  loop()
// ─────────────────────────────────────────────
void loop() {
    // ── Mantener conexión MQTT ────────────────
    if (!mqttClient.connected()) {
        unsigned long ahora = millis();
        if (ahora - lastReconnect >= MQTT_RECONNECT_DELAY_MS) {
            lastReconnect = ahora;
            reconnectMQTT();
        }
    } else {
        mqttClient.loop();
    }

    // ── Publicar datos cada intervalo ─────────
    if (millis() - lastPublish >= INTERVALO_MEDICION_MS) {
        lastPublish = millis();
        publicarDatos();
    }

    // ── Ejecutar OTA si fue solicitado ────────
    // Se ejecuta aquí (fuera del callback MQTT)
    // para no bloquear el stack de PubSubClient.
    if (otaPendiente.activo) {
        otaPendiente.activo = false;
        ejecutarOTA();
    }
}

// ─────────────────────────────────────────────
//  resetW5500()  — igual que tu código base
// ─────────────────────────────────────────────
void resetW5500() {
    pinMode(W5500_RST, OUTPUT);
    digitalWrite(W5500_RST, LOW);
    delay(200);
    digitalWrite(W5500_RST, HIGH);
    delay(500);
}

// ─────────────────────────────────────────────
//  initEthernet()
// ─────────────────────────────────────────────
bool initEthernet() {
    Serial.println("[ETH] Inicializando W5500...");
    resetW5500();

    SPI.begin(W5500_SCK, W5500_MISO, W5500_MOSI, W5500_CS);
    Ethernet.init(W5500_CS);

    Serial.println("[ETH] Solicitando IP por DHCP al RUT200...");
    if (Ethernet.begin(mac) == 0) {
        Serial.println("[ETH] DHCP falló. Revisa conexión al RUT200.");
        return false;
    }

    Serial.printf("[ETH] IP local:  %s\n", Ethernet.localIP().toString().c_str());
    Serial.printf("[ETH] Gateway:   %s\n", Ethernet.gatewayIP().toString().c_str());
    Serial.printf("[ETH] DNS:       %s\n", Ethernet.dnsServerIP().toString().c_str());
    return true;
}

// ─────────────────────────────────────────────
//  reconnectMQTT()  — igual que tu código base
//  Agregamos: suscripción al topic OTA y LWT
// ─────────────────────────────────────────────
void reconnectMQTT() {
    while (!mqttClient.connected()) {
        Serial.printf("[MQTT] Conectando a %s:%d...\n", MQTT_SERVER, MQTT_PORT);

        // Last Will Testament: si se desconecta abruptamente
        // el broker publica "offline" automáticamente
        bool ok = mqttClient.connect(
            DEVICE_ID,
            MQTT_USER,
            MQTT_PASSWORD,
            MQTT_TOPIC_OTA_STATUS,  // LWT topic
            1,                       // LWT QoS
            true,                    // LWT retain
            "{\"status\":\"offline\",\"fw\":\"" FIRMWARE_VERSION "\"}"
        );

        if (ok) {
            Serial.println("[MQTT] Conectado a EMQX Cloud TLS.");

            // Mensaje de bienvenida (igual que tu código base)
            mqttClient.publish(MQTT_TOPIC_DATA,
                "ESP32 conectado a EMQX Cloud por TLS usando W5500");

            // ── Suscribirse al topic OTA ──────────
            mqttClient.subscribe(MQTT_TOPIC_OTA, 1);
            Serial.printf("[MQTT] Suscrito a: %s\n", MQTT_TOPIC_OTA);

            // Publicar estado online
            publicarOtaStatus("online");

        } else {
            Serial.printf("[MQTT] Falló rc=%d — reintentando en 5s\n",
                          mqttClient.state());
            delay(MQTT_RECONNECT_DELAY_MS);
        }
    }
}

// ─────────────────────────────────────────────
//  mqttCallback()
//  Recibe mensajes en topics suscritos.
//  ⚠️ No ejecutar OTA aquí directamente —
//  solo marcar como pendiente para el loop().
// ─────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    // Convertir payload a string con null terminator
    char buf[512] = {0};
    strlcpy(buf, (char*)payload, min((unsigned int)511, length + 1));

    Serial.printf("[MQTT] Mensaje en '%s': %s\n", topic, buf);

    // ── Mensaje de OTA ────────────────────────
    if (strcmp(topic, MQTT_TOPIC_OTA) == 0) {
        Serial.println("[OTA] Solicitud de actualización recibida.");

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, buf);
        if (err) {
            Serial.printf("[OTA] JSON inválido: %s\n", err.c_str());
            return;
        }

        const char* version = doc["version"] | "";
        const char* url     = doc["url"]     | "";
        const char* md5     = doc["md5"]     | "";

        // Validar campos mínimos
        if (strlen(version) == 0 || strlen(url) == 0) {
            Serial.println("[OTA] Payload incompleto. Se requiere 'version' y 'url'.");
            return;
        }

        // Verificar si ya tenemos esa versión
        if (strcmp(version, FIRMWARE_VERSION) == 0) {
            Serial.printf("[OTA] Ya tenemos la versión %s. Ignorando.\n", version);
            return;
        }

        // Parsear URL en host y path
        char host[128] = {0};
        char path[256] = {0};
        if (!parsearUrlOta(url, host, path)) {
            Serial.println("[OTA] URL inválida.");
            return;
        }

        // Guardar info y marcar como pendiente
        strlcpy(otaPendiente.version,  version, sizeof(otaPendiente.version));
        strlcpy(otaPendiente.url_host, host,    sizeof(otaPendiente.url_host));
        strlcpy(otaPendiente.url_path, path,    sizeof(otaPendiente.url_path));
        strlcpy(otaPendiente.md5,      md5,     sizeof(otaPendiente.md5));
        otaPendiente.activo = true;

        Serial.printf("[OTA] Actualización programada: v%s → v%s\n",
                      FIRMWARE_VERSION, version);
        Serial.printf("[OTA] Host: %s | Path: %s\n", host, path);
    }
}

// ─────────────────────────────────────────────
//  publicarDatos()
//  Construye y publica el payload JSON.
//  Estructura igual a tu código base +
//  campos de diagnóstico para campo.
// ─────────────────────────────────────────────
void publicarDatos() {
    if (!mqttClient.connected()) return;

    // Simular lecturas (reemplazar con sensores reales)
    float nivel   = random(100, 300) / 10.0;
    float bateria = random(115, 130) / 10.0;

    // Construir JSON con ArduinoJson (más robusto que concatenar strings)
    JsonDocument doc;
    doc["equipo"]   = "PRUEBA_SAMA_MQTT";
    doc["fw"]       = FIRMWARE_VERSION;
    doc["nivel"]    = round(nivel * 100) / 100.0;
    doc["bateria"]  = round(bateria * 100) / 100.0;
    doc["uptime_s"] = millis() / 1000;

    char payload[256];
    serializeJson(doc, payload);

    Serial.printf("[DATOS] Publicando en '%s': %s\n", MQTT_TOPIC_DATA, payload);

    bool ok = mqttClient.publish(MQTT_TOPIC_DATA, payload);
    Serial.println(ok ? "[DATOS] Publicado correctamente."
                      : "[DATOS] Error publicando.");
}

// ─────────────────────────────────────────────
//  publicarOtaStatus()
//  Notifica al broker el estado del dispositivo.
// ─────────────────────────────────────────────
void publicarOtaStatus(const char* status) {
    if (!mqttClient.connected()) return;

    JsonDocument doc;
    doc["status"]  = status;
    doc["fw"]      = FIRMWARE_VERSION;
    doc["device"]  = DEVICE_ID;

    char payload[128];
    serializeJson(doc, payload);

    // retain=true para que el broker lo recuerde
    mqttClient.publish(MQTT_TOPIC_OTA_STATUS, payload, true);
    Serial.printf("[STATUS] %s → %s\n", MQTT_TOPIC_OTA_STATUS, payload);
}

// ─────────────────────────────────────────────
//  otaInit()
//  Verifica la partición activa al arrancar.
//  Confirma el firmware si arrancó correctamente
//  (cancela el rollback automático).
// ─────────────────────────────────────────────
void otaInit() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t   ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            Serial.println("[OTA] Nuevo firmware verificado — confirmando partición.");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
    Serial.printf("[OTA] Partición activa: %s | FW: v%s\n",
                  running->label, FIRMWARE_VERSION);
}

// ─────────────────────────────────────────────
//  ejecutarOTA()
//  Descarga el firmware desde GitHub vía HTTP
//  y lo escribe en la partición de actualización.
//
//  Usa EthernetClient directo (sin SSL) porque
//  GitHub raw/releases por HTTP plano o se puede
//  usar un servidor propio HTTP.
//
//  Para producción: migrar a sslClient con CA.
// ─────────────────────────────────────────────
void ejecutarOTA() {
    Serial.println("\n[OTA] ══════════════════════════════════");
    Serial.printf ("[OTA] Iniciando: v%s → v%s\n",
                   FIRMWARE_VERSION, otaPendiente.version);
    Serial.printf ("[OTA] Host: %s\n", otaPendiente.url_host);
    Serial.printf ("[OTA] Path: %s\n", otaPendiente.url_path);
    Serial.println("[OTA] ══════════════════════════════════");

    // Publicar estado en broker antes de iniciar
    publicarOtaStatus("downloading");
    mqttClient.loop();  // Asegurar que el mensaje salga

    // ── Conectar al servidor de descarga ──────
    // Se usa EthernetClient directo (HTTP) para simplificar.
    // El servidor debe estar accesible sin TLS, o usar
    // raw.githubusercontent.com en vez de github.com/releases.
    EthernetClient dlClient;

    Serial.printf("[OTA] Conectando a %s:80...\n", otaPendiente.url_host);
    if (!dlClient.connect(otaPendiente.url_host, 80)) {
        Serial.println("[OTA] Error: no se pudo conectar al servidor.");
        publicarOtaStatus("error_conexion");
        return;
    }

    // ── Request HTTP GET ──────────────────────
    dlClient.printf("GET %s HTTP/1.1\r\n", otaPendiente.url_path);
    dlClient.printf("Host: %s\r\n", otaPendiente.url_host);
    dlClient.println("Connection: close");
    dlClient.println();

    // ── Leer cabeceras HTTP ───────────────────
    int    contentLength = -1;
    int    statusCode    = 0;
    String statusLine    = dlClient.readStringUntil('\n');
    statusLine.trim();

    // Parsear código HTTP (ej. "HTTP/1.1 200 OK")
    if (statusLine.startsWith("HTTP/")) {
        statusCode = statusLine.substring(9, 12).toInt();
    }

    Serial.printf("[OTA] HTTP status: %d\n", statusCode);

    if (statusCode != 200) {
        Serial.printf("[OTA] Error HTTP %d. Verifica la URL del binario.\n", statusCode);
        Serial.println("[OTA] Tip: usa raw.githubusercontent.com, no github.com/releases");
        dlClient.stop();
        publicarOtaStatus("error_http");
        return;
    }

    // Leer el resto de cabeceras hasta línea vacía
    while (dlClient.connected()) {
        String header = dlClient.readStringUntil('\n');
        header.trim();
        if (header.startsWith("Content-Length:")) {
            contentLength = header.substring(16).toInt();
        }
        if (header.length() == 0) break;  // Fin de cabeceras
    }

    Serial.printf("[OTA] Tamaño firmware: %d bytes\n", contentLength);

    if (contentLength <= 0) {
        Serial.println("[OTA] Content-Length no disponible. Abortando.");
        dlClient.stop();
        publicarOtaStatus("error_tamano");
        return;
    }

    // ── Iniciar escritura OTA en flash ────────
    const esp_partition_t* update_part =
        esp_ota_get_next_update_partition(NULL);

    if (!update_part) {
        Serial.println("[OTA] No se encontró partición de actualización.");
        dlClient.stop();
        publicarOtaStatus("error_particion");
        return;
    }

    Serial.printf("[OTA] Escribiendo en partición: %s\n", update_part->label);

    esp_ota_handle_t ota_handle;
    if (esp_ota_begin(update_part, contentLength, &ota_handle) != ESP_OK) {
        Serial.println("[OTA] esp_ota_begin falló.");
        dlClient.stop();
        publicarOtaStatus("error_flash");
        return;
    }

    // ── Descarga y escritura por chunks ───────
    MD5Builder md5;
    md5.begin();

    uint8_t  buf[512];
    int      totalEscrito = 0;
    int      bytesLeidos  = 0;
    uint32_t tInicio      = millis();
    int      pctAnterior  = -1;

    while (totalEscrito < contentLength) {

        // Timeout de seguridad
        if (millis() - tInicio > OTA_DOWNLOAD_TIMEOUT_MS) {
            Serial.println("[OTA] Timeout de descarga. Abortando.");
            esp_ota_abort(ota_handle);
            dlClient.stop();
            publicarOtaStatus("error_timeout");
            return;
        }

        // Esperar datos disponibles
        if (!dlClient.available()) {
            if (!dlClient.connected()) break;
            delay(1);
            continue;
        }

        bytesLeidos = dlClient.read(buf, sizeof(buf));
        if (bytesLeidos <= 0) continue;

        md5.add(buf, bytesLeidos);

        if (esp_ota_write(ota_handle, buf, bytesLeidos) != ESP_OK) {
            Serial.println("[OTA] Error de escritura en flash.");
            esp_ota_abort(ota_handle);
            dlClient.stop();
            publicarOtaStatus("error_escritura");
            return;
        }

        totalEscrito += bytesLeidos;

        // Mostrar progreso cada 10%
        int pct = (totalEscrito * 100) / contentLength;
        if (pct / 10 != pctAnterior / 10) {
            pctAnterior = pct;
            Serial.printf("[OTA] Progreso: %3d%%  (%d / %d bytes)\n",
                          pct, totalEscrito, contentLength);
            // Publicar progreso al broker cada 25%
            if (pct % 25 == 0) {
                char statusBuf[32];
                snprintf(statusBuf, sizeof(statusBuf), "downloading_%d%%", pct);
                publicarOtaStatus(statusBuf);
                mqttClient.loop();
            }
        }
    }

    dlClient.stop();

    // ── Verificar MD5 ─────────────────────────
    md5.calculate();
    String md5Calc = md5.toString();
    Serial.printf("[OTA] MD5 calculado: %s\n", md5Calc.c_str());

    if (strlen(otaPendiente.md5) > 0) {
        Serial.printf("[OTA] MD5 esperado:  %s\n", otaPendiente.md5);
        if (!md5Calc.equalsIgnoreCase(otaPendiente.md5)) {
            Serial.println("[OTA] ¡Error MD5! Firmware corrupto. Abortando.");
            esp_ota_abort(ota_handle);
            publicarOtaStatus("error_md5");
            return;
        }
        Serial.println("[OTA] MD5 verificado correctamente.");
    } else {
        Serial.println("[OTA] MD5 no provisto — omitiendo verificación.");
    }

    // ── Finalizar y marcar partición ──────────
    if (esp_ota_end(ota_handle) != ESP_OK) {
        Serial.println("[OTA] esp_ota_end falló.");
        publicarOtaStatus("error_ota_end");
        return;
    }

    if (esp_ota_set_boot_partition(update_part) != ESP_OK) {
        Serial.println("[OTA] esp_ota_set_boot_partition falló.");
        publicarOtaStatus("error_boot_partition");
        return;
    }

    // Publicar éxito antes de reiniciar
    publicarOtaStatus("rebooting");
    mqttClient.loop();
    delay(500);

    Serial.println("[OTA] ✓ Firmware escrito. Reiniciando...");
    Serial.println("[OTA] ══════════════════════════════════\n");
    delay(500);
    esp_restart();
}

// ─────────────────────────────────────────────
//  parsearUrlOta()
//  Extrae host y path de una URL HTTP/HTTPS.
//  Ejemplo:
//   "https://raw.githubusercontent.com/ORG/REPO/main/ota/fw.bin"
//    host → "raw.githubusercontent.com"
//    path → "/ORG/REPO/main/ota/fw.bin"
// ─────────────────────────────────────────────
bool parsearUrlOta(const char* url, char* host, char* path) {
    const char* start = url;

    if      (strncmp(start, "https://", 8) == 0) start += 8;
    else if (strncmp(start, "http://",  7) == 0) start += 7;
    else return false;

    const char* slash = strchr(start, '/');
    if (!slash) return false;

    size_t hostLen = slash - start;
    strlcpy(host, start, hostLen + 1);
    strlcpy(path, slash, 256);
    return true;
}
