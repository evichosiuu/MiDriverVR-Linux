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
