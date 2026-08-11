#include "VrLowResPreview.h"
#include <cstdio>
#include <cstdarg>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

static void PrevLog(const char* fmt, ...) {
    char buf[256]; va_list va; va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va); va_end(va);
    OutputDebugStringA("[CamVR][LowResPreview] "); OutputDebugStringA(buf); OutputDebugStringA("\n");
}

// ==================================================================
//  Deteccion del monitor virtual real
//
//  Mismo fix que v12 en VrH264Streamer::ResolveCaptureRegion(): el
//  monitor que el driver anuncia (WINDOW_W x WINDOW_H, ver dllmain.cpp)
//  es una pantalla EXTENDIDA que Windows coloca donde el SO decide, no
//  necesariamente en (0,0). Si esto se ignora, se termina capturando
//  el monitor primario (que puede estar en negro con SteamVR activo)
//  en vez de la imagen VR real, y el preview de la 3DS sale negro
//  aunque el resto del pipeline funcione.
//
//  Se duplica aca (en vez de reusar el metodo privado de
//  VrH264Streamer) para que VrLowResPreview no dependa de esa clase y
//  siga funcionando aunque el stream H.264 principal este parado.
//
//  IMPORTANTE: estos numeros deben coincidir con CMyDisplayComponent
//  en dllmain.cpp (WINDOW_W/WINDOW_H). Si los cambias alla, cambialos
//  aca tambien.
// ==================================================================
static const uint32_t kAnnouncedWindowW = 1920;
static const uint32_t kAnnouncedWindowH = 1080;

struct MonitorRectInfo { RECT rc; bool isPrimary; };

static BOOL CALLBACK LowResMonitorEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
    auto* list = reinterpret_cast<std::vector<MonitorRectInfo>*>(lp);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(hMon, &mi)) {
        list->push_back({ mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 });
    }
    return TRUE;
}

void VrLowResPreview::ResolveCaptureRegion() {
    std::vector<MonitorRectInfo> monitors;
    EnumDisplayMonitors(nullptr, nullptr, LowResMonitorEnumProc, reinterpret_cast<LPARAM>(&monitors));

    const MonitorRectInfo* chosen = nullptr;

    // 1) Match exacto de resolucion con el monitor virtual anunciado.
    for (auto& m : monitors) {
        LONG w = m.rc.right - m.rc.left, h = m.rc.bottom - m.rc.top;
        if ((UINT32)w == kAnnouncedWindowW && (UINT32)h == kAnnouncedWindowH && !m.isPrimary) {
            chosen = &m; break;
        }
    }
    // 2) Cualquier monitor no-primario.
    if (!chosen) {
        for (auto& m : monitors) { if (!m.isPrimary) { chosen = &m; break; } }
    }

    if (chosen) {
        m_captureX = chosen->rc.left;
        m_captureY = chosen->rc.top;
        m_captureW = chosen->rc.right - chosen->rc.left;
        m_captureH = chosen->rc.bottom - chosen->rc.top;
        PrevLog("ResolveCaptureRegion: monitor VIRTUAL en (%d,%d) %dx%d",
            m_captureX, m_captureY, m_captureW, m_captureH);
    }
    else {
        // Fallback: monitor primario completo.
        m_captureX = 0; m_captureY = 0;
        m_captureW = GetSystemMetrics(SM_CXSCREEN);
        m_captureH = GetSystemMetrics(SM_CYSCREEN);
        PrevLog("ResolveCaptureRegion: sin monitor separado, usando primario %dx%d (puede salir negro si SteamVR dibuja en otro monitor)",
            m_captureW, m_captureH);
    }
}

// ==================================================================
//  GDI init/cleanup
// ==================================================================
bool VrLowResPreview::InitGdi() {
    m_screenDC = GetDC(nullptr);
    if (!m_screenDC) { PrevLog("GetDC(nullptr) fallo err=%u", (unsigned)GetLastError()); return false; }

    ResolveCaptureRegion();

    m_memDC = CreateCompatibleDC(m_screenDC);
    if (!m_memDC) { PrevLog("CreateCompatibleDC fallo err=%u", (unsigned)GetLastError()); return false; }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = FRAME_W;
    bi.bmiHeader.biHeight = -FRAME_H; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    m_memBmp = CreateDIBSection(m_memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!m_memBmp) { PrevLog("CreateDIBSection fallo err=%u", (unsigned)GetLastError()); return false; }

    m_oldBmp = (HBITMAP)SelectObject(m_memDC, m_memBmp);

    PrevLog("GDI init OK: captura (%d,%d) %dx%d -> preview %dx%d",
        m_captureX, m_captureY, m_captureW, m_captureH, FRAME_W, FRAME_H);
    return true;
}

void VrLowResPreview::CleanupGdi() {
    if (m_memDC && m_oldBmp) SelectObject(m_memDC, m_oldBmp);
    if (m_memBmp) { DeleteObject(m_memBmp); m_memBmp = nullptr; }
    if (m_memDC) { DeleteDC(m_memDC); m_memDC = nullptr; }
    if (m_screenDC) { ReleaseDC(nullptr, m_screenDC); m_screenDC = nullptr; }
}

// ==================================================================
//  Captura + downscale + conversion BGRA -> RGB565
//
//  StretchBlt hace el downscale de la region completa (m_captureW x
//  m_captureH) a FRAME_W x FRAME_H de una sola pasada -- mucho mas
//  barato que capturar a resolucion completa y reescalar aparte.
//
//  GDI entrega BGRA (B en el byte 0), igual que en VrH264Streamer.
//  RGB565 empaqueta: bits [15:11]=R(5), [10:5]=G(6), [4:0]=B(5).
// ==================================================================
bool VrLowResPreview::CaptureAndConvert(std::vector<uint16_t>& outRgb565) {
    SetStretchBltMode(m_memDC, HALFTONE);
    SetBrushOrgEx(m_memDC, 0, 0, nullptr);

    BOOL ok = StretchBlt(
        m_memDC, 0, 0, FRAME_W, FRAME_H,
        m_screenDC, m_captureX, m_captureY, m_captureW, m_captureH,
        SRCCOPY | CAPTUREBLT
    );
    if (!ok) { PrevLog("StretchBlt fallo err=%u", (unsigned)GetLastError()); return false; }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = FRAME_W;
    bi.bmiHeader.biHeight = -FRAME_H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> bgra((size_t)FRAME_W * FRAME_H * 4);
    int lines = GetDIBits(m_screenDC, m_memBmp, 0, FRAME_H, bgra.data(), &bi, DIB_RGB_COLORS);
    if (lines != FRAME_H) {
        PrevLog("GetDIBits: lines=%d esperado=%d err=%u", lines, FRAME_H, (unsigned)GetLastError());
        return false;
    }

    outRgb565.resize((size_t)FRAME_W * FRAME_H);
    for (int i = 0; i < FRAME_W * FRAME_H; ++i) {
        uint8_t b = bgra[i * 4 + 0];
        uint8_t g = bgra[i * 4 + 1];
        uint8_t r = bgra[i * 4 + 2];
        uint16_t px = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        outRgb565[i] = px;
    }
    return true;
}

// ==================================================================
//  Red
// ==================================================================
bool VrLowResPreview::InitNetwork() {
    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) { PrevLog("socket() fallo err=%d", WSAGetLastError()); return false; }

    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_sock, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        PrevLog("bind fallo err=%d", WSAGetLastError());
        closesocket(m_sock); m_sock = INVALID_SOCKET;
        return false;
    }

    DWORD to = 500; setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));

    int sndbuf = 256 * 1024; setsockopt(m_sock, SOL_SOCKET, SO_SNDBUF, (char*)&sndbuf, sizeof(sndbuf));
    return true;
}

void VrLowResPreview::CleanupNetwork() {
    if (m_sock != INVALID_SOCKET) { closesocket(m_sock); m_sock = INVALID_SOCKET; }
}

// ---- Suscriptores ----
void VrLowResPreview::PruneSubscribers() {
    ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lk(m_subsMtx);
    m_subscribers.erase(
        std::remove_if(m_subscribers.begin(), m_subscribers.end(),
            [&](const Subscriber& s) { return (now - s.lastSeenMs) > SUBSCRIBER_TIMEOUT_MS; }),
        m_subscribers.end());
}

void VrLowResPreview::ListenLoop() {
    uint8_t buf[16];
    while (m_running.load()) {
        sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(m_sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue; // timeout normal (500ms) o error -> reintenta

        if (n == 8 && memcmp(buf, "VIDSUB\0\0", 8) == 0) {
            ULONGLONG now = GetTickCount64();
            std::lock_guard<std::mutex> lk(m_subsMtx);
            bool found = false;
            for (auto& s : m_subscribers) {
                if (s.addr.sin_addr.s_addr == from.sin_addr.s_addr && s.addr.sin_port == from.sin_port) {
                    s.lastSeenMs = now; found = true; break;
                }
            }
            if (!found) {
                Subscriber s; s.addr = from; s.lastSeenMs = now;
                m_subscribers.push_back(s);
                char ipStr[32]; inet_ntop(AF_INET, &from.sin_addr, ipStr, sizeof(ipStr));
                PrevLog("Nuevo suscriptor de preview: %s:%u", ipStr, ntohs(from.sin_port));
            }
        }
    }
}

// ---- Envio de un frame trozado a todos los suscriptores vivos ----
void VrLowResPreview::SendFrameToSubscribers(const uint16_t* rgb565, uint32_t frameId) {
    std::vector<Subscriber> subsCopy;
    {
        std::lock_guard<std::mutex> lk(m_subsMtx);
        subsCopy = m_subscribers;
    }
    if (subsCopy.empty()) return;

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(rgb565);
    const int totalBytes = FRAME_W * FRAME_H * 2; // RGB565 = 2 bytes/pixel
    const int totalChunks = (totalBytes + CHUNK_MAX - 1) / CHUNK_MAX;

    std::vector<uint8_t> pkt(HDR_BYTES + CHUNK_MAX);

    for (int chunkIdx = 0; chunkIdx < totalChunks; ++chunkIdx) {
        int offset = chunkIdx * CHUNK_MAX;
        int payloadLen = (totalBytes - offset < CHUNK_MAX) ? (totalBytes - offset) : CHUNK_MAX;

        memcpy(pkt.data(), "VIDL", 4);
        BE32(pkt.data() + 4, frameId);
        BE16(pkt.data() + 8, (uint16_t)chunkIdx);
        BE16(pkt.data() + 10, (uint16_t)totalChunks);
        BE16(pkt.data() + 12, (uint16_t)payloadLen);
        memcpy(pkt.data() + HDR_BYTES, bytes + offset, payloadLen);

        int pktLen = HDR_BYTES + payloadLen;
        for (auto& s : subsCopy) {
            sendto(m_sock, (const char*)pkt.data(), pktLen, 0, (sockaddr*)&s.addr, sizeof(s.addr));
        }
    }
}

// ---- Loop de captura/envio ----
void VrLowResPreview::CaptureLoop() {
    if (!InitGdi()) {
        PrevLog("InitGdi fallo - preview no disponible (el resto del driver sigue funcionando igual)");
        return;
    }

    uint32_t frameId = 0;
    std::vector<uint16_t> frame;

    while (m_running.load()) {
        ULONGLONG t0 = GetTickCount64();

        PruneSubscribers();

        bool hasSubs;
        {
            std::lock_guard<std::mutex> lk(m_subsMtx);
            hasSubs = !m_subscribers.empty();
        }

        if (hasSubs) {
            if (CaptureAndConvert(frame)) {
                SendFrameToSubscribers(frame.data(), frameId++);
            }
        }

        ULONGLONG elapsed = GetTickCount64() - t0;
        if (elapsed < (ULONGLONG)SEND_INTERVAL_MS) {
            Sleep((DWORD)(SEND_INTERVAL_MS - elapsed));
        }
    }

    CleanupGdi();
}

// ==================================================================
//  API publica
// ==================================================================
bool VrLowResPreview::Start() {
    if (m_running.exchange(true)) return true;
    if (!InitNetwork()) { m_running = false; return false; }

    m_listenThread = std::thread([this] { ListenLoop(); });
    m_captureThread = std::thread([this] { CaptureLoop(); });

    PrevLog("Preview de baja resolucion iniciado puerto UDP:%u (%dx%d RGB565, cada %dms)",
        PORT, FRAME_W, FRAME_H, SEND_INTERVAL_MS);
    return true;
}

void VrLowResPreview::Stop() {
    if (!m_running.exchange(false)) return;
    CleanupNetwork(); // desbloquea recvfrom() en ListenLoop
    if (m_listenThread.joinable())  m_listenThread.join();
    if (m_captureThread.joinable()) m_captureThread.join();
}