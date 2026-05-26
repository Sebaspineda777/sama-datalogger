#pragma once
#include <Arduino.h>

// ─────────────────────────────────────────────
//  SAMA OTA Manager
//  Gestiona la actualización de firmware OTA
//  vía HTTPS desde GitHub Releases
// ─────────────────────────────────────────────

// Resultado de la operación OTA
typedef enum {
    OTA_OK = 0,
    OTA_ERR_MISMA_VERSION,
    OTA_ERR_BATERIA_BAJA,
    OTA_ERR_DESCARGA,
    OTA_ERR_MD5,
    OTA_ERR_ESCRITURA,
    OTA_ERR_RED,
    OTA_ERR_JSON
} ota_result_t;

// Información del firmware remoto
typedef struct {
    char version[16];
    char url[256];
    char md5[33];
} ota_info_t;

// ── Funciones públicas ────────────────────────

// Inicializa el subsistema OTA y verifica la
// partición activa al arrancar.
void ota_init();

// Consulta el servidor para ver si hay versión nueva.
// Retorna true si hay actualización disponible.
bool ota_check(ota_info_t* info);

// Ejecuta la descarga y aplicación del firmware.
ota_result_t ota_perform(const ota_info_t* info);

// Llamar desde el callback MQTT cuando llega un
// mensaje en el topic sama/ota/{device_id}
// Payload esperado: {"version":"x.y.z","url":"...","md5":"..."}
void ota_handle_mqtt(const char* payload, unsigned int length);

// Retorna un string legible del resultado OTA
const char* ota_result_str(ota_result_t result);
