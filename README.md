# MiDriverVR

🇪🇸 [Español](#-español) | 🇬🇧 [English](#-english)

---

## 🇪🇸 Español

### Requisitos para compilar en Linux

#### 1. Paquetes y dependencias
Instala los compiladores C++17 y librerías necesarias (OpenSSL, x264, X11, PulseAudio):
```bash
sudo apt-get install -y build-essential cmake libssl-dev libx264-dev libx11-dev libpulse-dev
```

#### 2. Compilación en Linux

Puedes compilar con `make` o con `cmake`:

**Usando Make:**
```bash
make
```

**Usando CMake:**
```bash
mkdir -p build && cd build
cmake ..
make
```

El resultado `driver_MiDriverVR.so` se guardará automáticamente en `bin/linux64/driver_MiDriverVR.so`.

---

### Requisitos para compilar en Windows

#### 1. Visual Studio
- **Visual Studio 2022** con el workload **"Desarrollo para el escritorio con C++"**.
- Windows 10/11 SDK.

#### 2. OpenVR SDK
El proyecto requiere `openvr_driver.h` de Valve (incluido en `headers/`).

#### 3. Compilar en Windows
- Abre `MiDriverVR.sln` en Visual Studio.
- Selecciona la plataforma **x64**.
- Compila con `Ctrl + Shift + B`. El `.dll` se generará en `bin/win64/driver_MiDriverVR.dll`.

---

### Instalación en SteamVR (Linux / Windows)

La estructura final dentro de la carpeta de drivers de SteamVR (`~/.steam/steam/steamapps/common/SteamVR/drivers/MiDriverVR` en Linux o `SteamVR\drivers\MiDriverVR` en Windows) debe verse así:

```
SteamVR/drivers/MiDriverVR
├── bin/
│   ├── win64/driver_MiDriverVR.dll
│   └── linux64/driver_MiDriverVR.so
├── resources/
├── driver.vrdesc
└── driver.vrdrivermanifest
```

1. Copia la carpeta `MiDriverVR` con los binarios `bin/`, `driver.vrdesc`, `driver.vrdrivermanifest` y la carpeta `resources/` a la ruta de drivers de SteamVR.
2. Abre SteamVR — el driver se cargará automáticamente.
3. Revisa `vrserver.txt` para confirmar que aparecen líneas con el prefijo `[CamVR]` sin errores.

---

### Puertos usados por el driver

| Función | Puerto | Protocolo |
|---|---|---|
| Anuncio/discovery en red | 47294 | UDP (broadcast) |
| Streaming de video | 47295 | TCP |
| Cambio de calidad de video | 47296 | UDP |
| Flip de HMD (comando remoto) | 47297 | UDP |
| Streaming de audio | 47298 | TCP |
| USB Tracking & LowRes Preview | 47299 | TCP / UDP |

---

### Notas técnicas

- Soporta conexión inalámbrica (**WiFi**) y por cable **USB** (vía túneles `adb reverse`).
- En Linux, la captura de pantalla utiliza X11 y la codificación H.264 de baja latencia utiliza `libx264`. La captura de audio utiliza la API simple de PulseAudio.
- En Windows, utiliza Media Foundation MFT / Direct3D11 para codificación por hardware con fallback a software, y WASAPI para audio loopback.
- La autenticación de sesión usa reto-respuesta con **HMAC-SHA256** truncado a 8 bytes, separando sesiones por loopback (PC) vs red (teléfono).
- El suavizado de posición y curl de dedos usa un filtro exponencial simple para evitar saltos bruscos.

[⬆ Volver arriba](#midrivervr)

---

## 🇬🇧 English

### Build requirements for Linux

#### 1. Packages and dependencies
Install the required C++17 compiler and libraries (OpenSSL, x264, X11, PulseAudio):
```bash
sudo apt-get install -y build-essential cmake libssl-dev libx264-dev libx11-dev libpulse-dev
```

#### 2. Building on Linux

You can compile using either `make` or `cmake`:

**Using Make:**
```bash
make
```

**Using CMake:**
```bash
mkdir -p build && cd build
cmake ..
make
```

The compiled library `driver_MiDriverVR.so` will be created automatically in `bin/linux64/driver_MiDriverVR.so`.

---

### Build requirements for Windows

#### 1. Visual Studio
- **Visual Studio 2022** with **"Desktop development with C++"** workload.
- Windows 10/11 SDK.

#### 2. OpenVR SDK
Requires Valve's `openvr_driver.h` (included in `headers/`).

#### 3. Building on Windows
- Open `MiDriverVR.sln` in Visual Studio.
- Select platform **x64**.
- Build with `Ctrl + Shift + B`. The `.dll` will be generated in `bin/win64/driver_MiDriverVR.dll`.

---

### Installing in SteamVR (Linux / Windows)

The final directory structure in SteamVR drivers directory (`~/.steam/steam/steamapps/common/SteamVR/drivers/MiDriverVR` on Linux or `SteamVR\drivers\MiDriverVR` on Windows) should look like this:

```
SteamVR/drivers/MiDriverVR
├── bin/
│   ├── win64/driver_MiDriverVR.dll
│   └── linux64/driver_MiDriverVR.so
├── resources/
├── driver.vrdesc
└── driver.vrdrivermanifest
```

1. Copy the `MiDriverVR` folder containing `bin/`, `driver.vrdesc`, `driver.vrdrivermanifest`, and `resources/` into the SteamVR drivers folder.
2. Launch SteamVR — the driver will load automatically.
3. Check `vrserver.txt` to confirm log lines with `[CamVR]` prefix appear without errors.

---

### Ports used by the driver

| Function | Port | Protocol |
|---|---|---|
| Network announce/discovery | 47294 | UDP (broadcast) |
| Video streaming | 47295 | TCP |
| Video quality change | 47296 | UDP |
| HMD flip (remote command) | 47297 | UDP |
| Audio streaming | 47298 | TCP |
| USB Tracking & LowRes Preview | 47299 | TCP / UDP |

---

### Technical notes

- Supports both wireless (**WiFi**) and wired **USB** connections (via `adb reverse` tunnels).
- On Linux, screen capture uses X11 and low-latency H.264 video encoding uses `libx264`. Audio capture uses PulseAudio simple API.
- On Windows, video encoding uses Media Foundation MFT / Direct3D11 with software fallback, and audio loopback uses WASAPI.
- Session authentication uses challenge-response with **HMAC-SHA256** truncated to 8 bytes, separating loopback (PC) and network (phone) sessions.
- Position smoothing and finger curl use a simple exponential filter to prevent abrupt motion jumps.

[⬆ Back to top](#midrivervr)
