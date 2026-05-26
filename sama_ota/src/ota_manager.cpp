#include "ota_manager.h"
#include "config.h"

#include <Arduino.h>
#include <Ethernet.h>
#include <HttpClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <MD5Builder.h>

// ─────────────────────────────────────────────
//  Variables internas
// ─────────────────────────────────────────────
static bool    _ota_pendiente  = false;
static ota_info_t _ota_pending_info;

// ─────────────────────────────────────────────
//  ota_init()
//  Verifica la partición activa al arrancar.
//  Si el último OTA no fue confirmado, hace rollback.
// ─────────────────────────────────────────────
void ota_init() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            // El firmware arrancó correctamente — confirmamos
            Serial.println("[OTA] Nuevo firmware verificado correctamente.");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    Serial.printf("[OTA] Firmware activo: %s | Partición: %s\n",
                  FIRMWARE_VERSION,
                  running->label);
}

// ─────────────────────────────────────────────
//  _descargar_json_version()
//  Descarga el archivo version.json desde GitHub
//  y parsea la información del firmware remoto.
// ─────────────────────────────────────────────
static bool _descargar_json_version(ota_info_t* info) {
    EthernetClient client;
    HttpClient http(client, "raw.githubusercontent.com", 80);

    // Extraer path de la URL completa
    // URL ejemplo: https://raw.githubusercontent.com/ORG/REPO/main/ota/version.json
    const char* path = strstr(OTA_VERSION_URL, "githubusercontent.com");
    if (!path) {
        Serial.println("[OTA] URL de versión inválida.");
        return false;
    }
    path += strlen("githubusercontent.com");

    Serial.printf("[OTA] Consultando versión en: %s\n", path);

    http.get(path);

    int statusCode = http.responseStatusCode();
    if (statusCode != 200) {
        Serial.printf("[OTA] Error HTTP al consultar versión: %d\n", statusCode);
        http.stop();
        return false;
    }

    String body = http.responseBody();
    http.stop();

    // Parsear JSON
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[OTA] Error parseando version.json: %s\n", err.c_str());
        return false;
    }

    strlcpy(info->version, doc["version"] | "", sizeof(info->version));
    strlcpy(info->url,     doc["url"]     | "", sizeof(info->url));
    strlcpy(info->md5,     doc["md5"]     | "", sizeof(info->md5));

    Serial.printf("[OTA] Versión remota: %s\n", info->version);
    return true;
}

// ─────────────────────────────────────────────
//  _versiones_son_distintas()
//  Compara versiones en formato semver simple.
// ─────────────────────────────────────────────
static bool _versiones_son_distintas(const char* remota) {
    return strcmp(remota, FIRMWARE_VERSION) != 0;
}

// ─────────────────────────────────────────────
//  ota_check()
//  Consulta si hay una versión más nueva disponible.
// ─────────────────────────────────────────────
bool ota_check(ota_info_t* info) {
    if (!_descargar_json_version(info)) return false;
    if (!_versiones_son_distintas(info->version)) {
        Serial.println("[OTA] Firmware ya está actualizado.");
        return false;
    }
    Serial.printf("[OTA] Actualización disponible: %s → %s\n",
                  FIRMWARE_VERSION, info->version);
    return true;
}

// ─────────────────────────────────────────────
//  ota_perform()
//  Descarga y aplica el nuevo firmware.
// ─────────────────────────────────────────────
ota_result_t ota_perform(const ota_info_t* info) {
    Serial.printf("[OTA] Iniciando descarga: %s\n", info->url);

    // Extraer host y path de la URL
    // URL ejemplo: https://github.com/ORG/REPO/releases/download/v1.1.0/firmware.bin
    char host[128] = {0};
    char path[256] = {0};
    int  port      = 80;

    // Parsear URL manualmente
    const char* start = info->url;
    if (strncmp(start, "https://", 8) == 0) { start += 8; port = 443; }
    else if (strncmp(start, "http://", 7) == 0) { start += 7; }

    const char* slash = strchr(start, '/');
    if (!slash) return OTA_ERR_RED;

    size_t host_len = slash - start;
    strlcpy(host, start, host_len + 1);
    strlcpy(path, slash, sizeof(path));

    Serial.printf("[OTA] Host: %s | Path: %s | Puerto: %d\n", host, path, port);

    // Para GitHub (que usa HTTPS con redirección),
    // usamos HTTP directo al host de releases de GitHub.
    // En producción usar EthernetClientSecure con certificado raíz.
    EthernetClient client;
    HttpClient http(client, host, 80);

    http.get(path);

    int statusCode = http.responseStatusCode();
    // GitHub Releases devuelve 302 redirect — manejar manualmente en producción.
    // Para la prueba, alojar el .bin en raw.githubusercontent.com (HTTP directo).
    if (statusCode != 200) {
        Serial.printf("[OTA] Error HTTP descarga: %d\n", statusCode);
        http.stop();
        return OTA_ERR_DESCARGA;
    }

    int contentLength = http.contentLength();
    Serial.printf("[OTA] Tamaño firmware: %d bytes\n", contentLength);

    // Iniciar escritura en partición de actualización
    const esp_partition_t* update_partition =
        esp_ota_get_next_update_partition(NULL);

    if (!update_partition) {
        Serial.println("[OTA] No se encontró partición de actualización.");
        http.stop();
        return OTA_ERR_ESCRITURA;
    }

    Serial.printf("[OTA] Escribiendo en partición: %s\n", update_partition->label);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, contentLength, &ota_handle);
    if (err != ESP_OK) {
        Serial.printf("[OTA] esp_ota_begin falló: %s\n", esp_err_to_name(err));
        http.stop();
        return OTA_ERR_ESCRITURA;
    }

    // Descargar y escribir en chunks
    MD5Builder md5;
    md5.begin();

    uint8_t  buffer[512];
    int      total_written = 0;
    int      bytes_read    = 0;
    uint32_t timeout_start = millis();

    while (total_written < contentLength) {
        // Timeout de seguridad
        if (millis() - timeout_start > OTA_DOWNLOAD_TIMEOUT_MS) {
            Serial.println("[OTA] Timeout de descarga.");
            esp_ota_abort(ota_handle);
            http.stop();
            return OTA_ERR_DESCARGA;
        }

        bytes_read = http.read(buffer, sizeof(buffer));
        if (bytes_read <= 0) {
            delay(10);
            continue;
        }

        md5.add(buffer, bytes_read);

        err = esp_ota_write(ota_handle, buffer, bytes_read);
        if (err != ESP_OK) {
            Serial.printf("[OTA] Error escritura: %s\n", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            http.stop();
            return OTA_ERR_ESCRITURA;
        }

        total_written += bytes_read;

        // Progreso cada 10%
        if (contentLength > 0) {
            int pct = (total_written * 100) / contentLength;
            static int last_pct = -1;
            if (pct / 10 != last_pct / 10) {
                Serial.printf("[OTA] Progreso: %d%%  (%d / %d bytes)\n",
                              pct, total_written, contentLength);
                last_pct = pct;
            }
        }
    }

    http.stop();

    // Verificar MD5 si fue provisto
    md5.calculate();
    String md5_calculado = md5.toString();
    Serial.printf("[OTA] MD5 calculado:  %s\n", md5_calculado.c_str());
    Serial.printf("[OTA] MD5 esperado:   %s\n", info->md5);

    if (strlen(info->md5) > 0 &&
        !md5_calculado.equalsIgnoreCase(info->md5)) {
        Serial.println("[OTA] ¡Error MD5! El firmware descargado está corrupto.");
        esp_ota_abort(ota_handle);
        return OTA_ERR_MD5;
    }

    // Finalizar escritura
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        Serial.printf("[OTA] esp_ota_end falló: %s\n", esp_err_to_name(err));
        return OTA_ERR_ESCRITURA;
    }

    // Marcar nueva partición como pendiente de boot
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        Serial.printf("[OTA] esp_ota_set_boot_partition falló: %s\n",
                      esp_err_to_name(err));
        return OTA_ERR_ESCRITURA;
    }

    Serial.println("[OTA] Firmware escrito correctamente. Reiniciando...");
    delay(1000);
    esp_restart();

    return OTA_OK;  // No se llega aquí
}

// ─────────────────────────────────────────────
//  ota_handle_mqtt()
//  Callback para mensajes MQTT de OTA.
//  El ESP32 principal llama esta función desde
//  su callback MQTT cuando llega un mensaje en
//  el topic sama/ota/{device_id}
// ─────────────────────────────────────────────
void ota_handle_mqtt(const char* payload, unsigned int length) {
    Serial.println("[OTA] Mensaje MQTT de actualización recibido.");

    char buf[512];
    strlcpy(buf, payload, min((unsigned int)sizeof(buf), length + 1));

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf);
    if (err) {
        Serial.printf("[OTA] JSON inválido: %s\n", err.c_str());
        return;
    }

    strlcpy(_ota_pending_info.version, doc["version"] | "", 16);
    strlcpy(_ota_pending_info.url,     doc["url"]     | "", 256);
    strlcpy(_ota_pending_info.md5,     doc["md5"]     | "", 33);

    if (strlen(_ota_pending_info.version) == 0 ||
        strlen(_ota_pending_info.url) == 0) {
        Serial.println("[OTA] Payload incompleto — falta version o url.");
        return;
    }

    if (!_versiones_son_distintas(_ota_pending_info.version)) {
        Serial.println("[OTA] Ya tenemos esa versión. Ignorando.");
        return;
    }

    Serial.printf("[OTA] Actualización programada: %s → %s\n",
                  FIRMWARE_VERSION, _ota_pending_info.version);

    // Marcar como pendiente para ejecutar en el loop principal
    // (no ejecutar OTA directamente en el callback MQTT)
    _ota_pendiente = true;
}

// Verificar y ejecutar si hay OTA pendiente desde el loop principal
// Llamar esta función en el loop() del main
void ota_process_pending() {
    if (!_ota_pendiente) return;
    _ota_pendiente = false;

    Serial.println("[OTA] Ejecutando actualización pendiente...");
    ota_result_t result = ota_perform(&_ota_pending_info);

    if (result != OTA_OK) {
        Serial.printf("[OTA] Falló: %s\n", ota_result_str(result));
    }
}

// ─────────────────────────────────────────────
//  ota_result_str()
// ─────────────────────────────────────────────
const char* ota_result_str(ota_result_t result) {
    switch (result) {
        case OTA_OK:               return "OK";
        case OTA_ERR_MISMA_VERSION: return "Misma versión";
        case OTA_ERR_BATERIA_BAJA: return "Batería insuficiente";
        case OTA_ERR_DESCARGA:     return "Error de descarga";
        case OTA_ERR_MD5:          return "Error MD5 (firmware corrupto)";
        case OTA_ERR_ESCRITURA:    return "Error de escritura en flash";
        case OTA_ERR_RED:          return "Error de red";
        case OTA_ERR_JSON:         return "Error de JSON";
        default:                   return "Error desconocido";
    }
}
