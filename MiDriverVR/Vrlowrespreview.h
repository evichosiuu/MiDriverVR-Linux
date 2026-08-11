#pragma once
//
// VrLowResPreview.h
//
// Preview de video SIN comprimir (RGB565) para clientes que no pueden
// decodificar H.264 en tiempo real -- pensado para el cliente de 3DS
// (3ds_hmd_tracker/main.c), pero sirve para cualquier cliente que
// hable el mismo protocolito UDP.
//
// Protocolo (ver comentarios en 3ds_hmd_tracker/main.c):
//   Cliente -> Driver:  "VIDSUB\0\0"  (8 bytes)  cada ~0.5s (suscripcion + keepalive)
//   Driver  -> Cliente: paquetes "VIDL" trozados:
//       [0:4)   "VIDL"
//       [4:8)   frameId      (uint32 BE)
//       [8:10)  chunkIdx     (uint16 BE)
//       [10:12) totalChunks  (uint16 BE)
//       [12:14) payloadLen   (uint16 BE)
//       [14: )  payload (hasta 1400 bytes de datos RGB565 crudos)
//
// Frame completo = 200x120 RGB565 = 48000 bytes = 35 chunks de 1400
// (34 chunks de 1400 + 1 de 400).
//
// Este preview NO comparte captura con VrH264Streamer a proposito:
// as� no depende de que el stream principal este corriendo, y puede
// usar su propia tasa de refresco (mucho mas baja, ~4-6 fps, que es
// mas que suficiente para un preview de 200x120 en la pantalla
// superior de una 3DS).
//
// IMPORTANTE: winsock2.h/ws2tcpip.h SIEMPRE antes que windows.h en esta
// unidad de compilacion. Si windows.h se incluye primero, arrastra el
// winsock.h VIEJO (Winsock 1.1) por detras, y cuando despues se
// procesa winsock2.h sus tipos (sockaddr, sockaddr_in, fd_set...) y
// macros (AF_IPX, INADDR_ANY, FD_SET...) ya fueron definidos por el
// viejo -> decenas de "redefinicion" y errores de sintaxis en cascada.
// Mismo orden que ya se usa correctamente en dllmain.cpp.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>

class VrLowResPreview {
public:
    static constexpr uint16_t PORT = 47299;
    static constexpr int      FRAME_W = 200;
    static constexpr int      FRAME_H = 120;
    static constexpr int      CHUNK_MAX = 1400;
    static constexpr int      HDR_BYTES = 14;

    // Cuanto tiempo sin recibir un VIDSUB de refresco antes de dar de
    // baja a un suscriptor (el cliente manda VIDSUB cada ~500ms).
    static constexpr ULONGLONG SUBSCRIBER_TIMEOUT_MS = 2000;

    // Intervalo entre frames capturados/enviados. No hace falta ir a
    // 60fps: es un preview chico en una pantalla de 3DS, y mandar
    // menos trafico UDP deja mas ancho de banda para el stream H.264
    // real del telefono.
    static constexpr int SEND_INTERVAL_MS = 180; // ~5.5 fps

    bool Start();
    void Stop();
    ~VrLowResPreview() { Stop(); }

private:
    // ---- Red ----
    SOCKET m_sock = INVALID_SOCKET;
    std::atomic<bool> m_running{ false };
    std::thread m_listenThread;
    std::thread m_captureThread;

    struct Subscriber {
        sockaddr_in addr{};
        ULONGLONG   lastSeenMs = 0;
    };
    std::mutex               m_subsMtx;
    std::vector<Subscriber>  m_subscribers;

    bool InitNetwork();
    void CleanupNetwork();

    void ListenLoop();   // recibe VIDSUB y mantiene m_subscribers
    void CaptureLoop();  // captura + convierte + trocea + envia

    void PruneSubscribers();
    void SendFrameToSubscribers(const uint16_t* rgb565, uint32_t frameId);

    // ---- Captura de pantalla (independiente de VrH264Streamer) ----
    HDC m_screenDC = nullptr;
    HDC m_memDC = nullptr;
    HBITMAP m_memBmp = nullptr;
    HBITMAP m_oldBmp = nullptr;

    int m_captureX = 0, m_captureY = 0, m_captureW = 0, m_captureH = 0;

    bool InitGdi();
    void CleanupGdi();
    void ResolveCaptureRegion(); // mismo fix v12 que VrH264Streamer, version propia
    bool CaptureAndConvert(std::vector<uint16_t>& outRgb565); // captura -> RGB565 200x120

    static void BE16(uint8_t* p, uint16_t v) { p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF; }
    static void BE32(uint8_t* p, uint32_t v) {
        p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
    }
};