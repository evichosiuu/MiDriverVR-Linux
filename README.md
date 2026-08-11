## Requisitos para compilar

### 1. Visual Studio

- **Visual Studio 2022** (Community es gratis y suficiente) — la versión más reciente está bien, no hace falta una "especial".
- Al instalar, selecciona el workload **"Desarrollo para el escritorio con C++"** (*Desktop development with C++*). Esto te da el compilador MSVC, el linker y las herramientas de depuración.
- Asegúrate de incluir el **Windows 10/11 SDK** más reciente (se instala junto con el workload de C++, pero revisa que esté marcado en el instalador — headers como `wincrypt.h`, `mfapi.h`, `audioclient.h`, `iphlpapi.h`, etc. vienen de ahí).

### 2. OpenVR SDK

El proyecto incluye `#include <openvr_driver.h>`, que **no viene con Visual Studio**. Necesitas descargar el SDK de OpenVR de Valve:

- Repositorio: [`github.com/ValveSoftware/openvr`](https://github.com/ValveSoftware/openvr)
- Necesitas la carpeta `headers/` (con `openvr_driver.h`) y el `.lib` correspondiente si tu build lo requiere (normalmente los drivers de SteamVR solo necesitan los headers, ya que se cargan dinámicamente vía `HmdDriverFactory`).
- Agrega esa ruta en **Project Properties → C/C++ → General → Additional Include Directories**.

### 3. Configuración del proyecto

- **Plataforma:** x64 (SteamVR/OpenVR requiere 64 bits).
- **Tipo de proyecto:** DLL (`.dll`), ya que exporta `HmdDriverFactory` con `__declspec(dllexport)`.
- Las librerías ya están enlazadas vía `#pragma comment(lib, ...)` en el propio código (`ws2_32.lib`, `crypt32.lib`, `mfplat.lib`, `mfuuid.lib`, `mf.lib`, `gdi32.lib`, `user32.lib`, `winmm.lib`, `avrt.lib`, `iphlpapi.lib`), así que no necesitas agregarlas manualmente al linker — solo que el SDK de Windows esté instalado.

---

## Configuración paso a paso en Visual Studio (rutas exactas)

### 1. Descargar y ubicar el SDK

Descarga **OpenVR SDK v2.12.14** y descomprímelo en una ruta como:

C:\Users<tu_usuario>\Desktop\cosas\openvr-2.12.14\


Deberías tener dentro las carpetas `headers`, `lib`, `bin`, `src`, etc.

### 2. Directorios de inclusión (Include)

**Propiedades del proyecto → C/C++ → General → Directorios de inclusión adicionales**

C:\Users<tu_usuario>\Desktop\cosas\openvr-2.12.14\headers;%(AdditionalIncludeDirectories)


### 3. Directorios de bibliotecas (Library)

**Propiedades del proyecto → Vinculador → General → Directorios de bibliotecas adicionales**

C:\Users<tu_usuario>\Desktop\cosas\openvr-2.12.14\lib\win64;%(AdditionalLibraryDirectories)


> En ambos casos, deja la **Configuración** en "Todas las config." y la **Plataforma** en "Todas las plataformas" para que aplique a Debug y Release por igual.

### 4. Propiedades generales

| Propiedad | Valor |
|---|---|
| Directorio de salida | `C:\Program Files (x86)\Steam\steamapps\common\SteamVR\drivers\MiDriverVR\bin\win64\` |
| Nombre de destino | `driver_MiDriverVR` |
| Tipo de configuración | Biblioteca dinámica (.dll) |
| Versión del SDK de Windows | 10.0 (última versión instalada) |
| Conjunto de herramientas de la plataforma | Visual Studio 2022 (v143) |
| Estándar de lenguaje C++ | Estándar ISO C++17 (`/std:c++17`) |

> El **Directorio de salida** apunta directo a la carpeta de drivers de SteamVR, así el `.dll` compilado queda listo sin copiarlo manualmente.

### 5. Compilar

- Selecciona plataforma **x64** (obligatorio).
- Compila con `Ctrl + Shift + B` o **Compilar → Compilar solución**.
- El resultado `driver_MiDriverVR.dll` aparecerá automáticamente en la carpeta de drivers de SteamVR.

---

## Instalación en SteamVR

La estructura final dentro de `SteamVR\drivers\` debe verse así:

SteamVR\drivers\MiDriverVR
├── bin\win64\driver_MiDriverVR.dll
├── resources
├── driver.vrdesc
└── driver.vrdrivermanifest


1. Copia (o verifica, según tu directorio de salida) los archivos `driver.vrdesc`, `driver.vrdrivermanifest` y la carpeta `resources` dentro de `SteamVR\drivers\MiDriverVR\`.
2. Abre SteamVR — el driver se carga automáticamente si el manifiesto está bien registrado.
3. Revisa `vrserver.txt` para confirmar que aparecen líneas con el prefijo `[CamVR]` sin errores.

---

## Puertos usados por el driver

| Función | Puerto | Protocolo |
|---|---|---|
| Anuncio/discovery en red | 47294 | UDP (broadcast) |
| Streaming de video | 47295 | TCP |
| Cambio de calidad de video | 47296 | UDP |
| Flip de HMD (comando remoto) | 47297 | UDP |
| Streaming de audio | 47298 | TCP |

---

## Notas técnicas

- El encoder de video intenta usar **hardware (Media Foundation MFT)** primero; si no genera NALs en ~1.2s, cae automáticamente a **software**.
- La captura detecta monitores virtuales automáticamente (no primarios) para evitar capturar el escritorio real.
- La autenticación de sesión usa reto-respuesta con **HMAC-SHA256** truncado a 8 bytes, separando sesiones por loopback (PC) vs red (teléfono).
- El suavizado de posición y curl de dedos usa un filtro exponencial simple para evitar saltos bruscos.
