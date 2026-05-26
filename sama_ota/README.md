# SAMA Datalogger — OTA sobre EMQX Cloud TLS
**ESP32 DevKitC V4 + W5500 + RUT200 GSM + EMQX Cloud (TLS 8883)**

---

## Estructura del proyecto

```
sama_ota/
├── platformio.ini          ← Configuración PlatformIO
├── partitions_ota.csv      ← Tabla de particiones dual bank OTA
├── include/
│   └── config.h            ← ⚠️ EDITAR antes de compilar
├── src/
│   └── main.cpp            ← Firmware completo con OTA integrado
└── ota/
    └── version.json        ← ⚠️ Subir a GitHub (ver paso 3)
```

---

## Paso 1 — Editar config.h

Solo necesitas cambiar estas dos líneas:

```c
#define FIRMWARE_VERSION   "1.0.0"          // Versión inicial
#define OTA_VERSION_URL_HOST  "raw.githubusercontent.com"
#define OTA_VERSION_URL_PATH  "/TU_ORG/TU_REPO/main/ota/version.json"
```

Las credenciales EMQX ya están configuradas según tu código base.

---

## Paso 2 — Conexión física W5500 → ESP32 DevKitC V4

| W5500 | ESP32  | Notas              |
|-------|--------|--------------------|
| MOSI  | GPIO23 |                    |
| MISO  | GPIO19 |                    |
| SCLK  | GPIO18 |                    |
| CS    | GPIO5  |                    |
| RST   | GPIO4  |                    |
| 3.3V  | 3.3V   |                    |
| GND   | GND    |                    |

---

## Paso 3 — Configurar GitHub para OTA

### 3a. Crear repositorio y subir version.json

El archivo `ota/version.json` debe estar accesible en:
```
https://raw.githubusercontent.com/TU_ORG/TU_REPO/main/ota/version.json
```

Contenido inicial (apuntando a v1.0.0 — sin actualización aún):
```json
{
  "version": "1.0.0",
  "url": "",
  "md5": ""
}
```

### 3b. Compilar y flashear v1.0.0
```bash
pio run --target upload
pio device monitor --baud 115200
```

Salida esperada en serial:
```
==========================================
  SAMA Datalogger — OTA sobre EMQX TLS  
  Firmware: v1.0.0 | ID: Sama
==========================================
[OTA]  Partición activa: app0 | FW: v1.0.0
[ETH]  IP local: 192.168.x.x
[MQTT] Conectado a EMQX Cloud TLS.
[MQTT] Suscrito a: sama/ota/Sama
```

---

## Paso 4 — Probar OTA

### 4a. Preparar firmware v1.1.0
1. Cambiar en `config.h`:
   ```c
   #define FIRMWARE_VERSION "1.1.0"
   ```
2. **Compilar SIN flashear:**
   ```bash
   pio run
   ```
3. Binario generado en:
   ```
   .pio/build/esp32dev/firmware.bin
   ```

### 4b. Calcular MD5 del binario
```bash
# Linux / Mac
md5sum .pio/build/esp32dev/firmware.bin

# Windows PowerShell
Get-FileHash .pio\build\esp32dev\firmware.bin -Algorithm MD5
```

### 4c. Subir binario a GitHub Releases
1. Crear release `v1.1.0` en GitHub
2. Subir `firmware.bin` como asset del release
3. La URL de descarga será:
   ```
   https://github.com/TU_ORG/TU_REPO/releases/download/v1.1.0/firmware.bin
   ```
   ⚠️ GitHub Releases redirige (302). Para evitar problemas, sube
   el .bin también a la carpeta `ota/` del repo y usa:
   ```
   https://raw.githubusercontent.com/TU_ORG/TU_REPO/main/ota/firmware_v1.1.0.bin
   ```

### 4d. Actualizar version.json en GitHub
```json
{
  "version": "1.1.0",
  "url": "https://raw.githubusercontent.com/TU_ORG/TU_REPO/main/ota/firmware_v1.1.0.bin",
  "md5": "PEGAR_MD5_CALCULADO_AQUI"
}
```
Hacer commit y push.

### 4e. Disparar OTA por MQTT
Publicar en el topic `sama/ota/Sama` desde MQTT Explorer o terminal:

```json
{
  "version": "1.1.0",
  "url": "https://raw.githubusercontent.com/TU_ORG/TU_REPO/main/ota/firmware_v1.1.0.bin",
  "md5": "PEGAR_MD5_AQUI"
}
```

**Con mosquitto_pub:**
```bash
mosquitto_pub \
  -h p9d17dd1.ala.us-east-1.emqxsl.com \
  -p 8883 \
  --cafile emqx_ca.crt \
  -u Sama \
  -P "S4m4_mqtt*" \
  -t "sama/ota/Sama" \
  -m '{"version":"1.1.0","url":"https://raw.githubusercontent.com/...","md5":"..."}'
```

### 4f. Verificar en serial
```
[MQTT] Mensaje en 'sama/ota/Sama': {"version":"1.1.0",...}
[OTA]  Actualización programada: v1.0.0 → v1.1.0
[OTA]  Conectando a raw.githubusercontent.com:80...
[OTA]  HTTP status: 200
[OTA]  Tamaño firmware: 1048576 bytes
[OTA]  Progreso:  10%  (104857 / 1048576 bytes)
[OTA]  Progreso:  20%  ...
[OTA]  Progreso: 100%
[OTA]  MD5 calculado: a3f4c2...
[OTA]  MD5 verificado correctamente.
[OTA]  ✓ Firmware escrito. Reiniciando...

==========================================
  SAMA Datalogger — OTA sobre EMQX TLS  
  Firmware: v1.1.0 | ID: Sama           ← ✅ Actualizado
==========================================
[OTA]  Nuevo firmware verificado — confirmando partición.
```

---

## Topics MQTT del sistema

| Topic | Dirección | Descripción |
|-------|-----------|-------------|
| `sama/test` | ESP32 → Broker | Datos de medición |
| `sama/ota/Sama` | Broker → ESP32 | Solicitud de actualización OTA |
| `sama/ota/Sama/status` | ESP32 → Broker | Estado del dispositivo y progreso OTA |

---

## Solución de problemas

| Síntoma | Causa | Solución |
|---------|-------|----------|
| DHCP falló | RUT200 sin DHCP activo | Activar DHCP server en RUT200 |
| rc=-2 en MQTT | Credenciales incorrectas | Verificar user/pass en config.h |
| rc=-4 en MQTT | Timeout TLS | Verificar señal GSM en RUT200 |
| HTTP 302 en OTA | GitHub Releases redirige | Usar raw.githubusercontent.com |
| MD5 error | Binario corrupto | Re-subir el .bin a GitHub |
| Rollback automático | Nuevo FW crashea | Revisar compilación de v nueva |

---

## Próximo paso — Migración a EG915U-LA

Cuando pases del RUT200 al EG915U-LA integrado:
- Reemplazar `EthernetClient` → clase UART del EG915U
- Reemplazar `ESP_SSLClient` → TLS nativo del EG915U (AT+QSSLOPEN)
- El resto del firmware (lógica MQTT, OTA, sensores) **no cambia**
