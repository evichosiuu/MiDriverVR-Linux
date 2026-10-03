#include "Vrlowrespreview.h"
#include <algorithm>

static void PrevLog(const char* fmt, ...) {
    char buf[256]; va_list va; va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va); va_end(va);
#ifdef _WIN32
    OutputDebugStringA("[CamVR][LowResPreview] "); OutputDebugStringA(buf); OutputDebugStringA("\n");
#else
    fprintf(stderr, "[CamVR][LowResPreview] %s\n", buf);
#endif
}

#ifdef _WIN32
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
    for (auto& m : monitors) {
        LONG w = m.rc.right - m.rc.left, h = m.rc.bottom - m.rc.top;
        if ((UINT32)w == kAnnouncedWindowW && (UINT32)h == kAnnouncedWindowH && !m.isPrimary) {
            chosen = &m; break;
        }
    }

    if (!chosen) {
        for (auto& m : monitors) { if (!m.isPrimary) { chosen = &m; break; } }
    }

    if (chosen) {
        m_captureX = chosen->rc.left;
        m_captureY = chosen->rc.top;
        m_captureW = chosen->rc.right - chosen->rc.left;
        m_captureH = chosen->rc.bottom - chosen->rc.top;
    } else {
        m_captureX = 0; m_captureY = 0;
        m_captureW = GetSystemMetrics(SM_CXSCREEN);
        m_captureH = GetSystemMetrics(SM_CYSCREEN);
    }
}

bool VrLowResPreview::InitGdi() {
    m_screenDC = GetDC(nullptr);
    if (!m_screenDC) return false;
    ResolveCaptureRegion();
    m_memDC = CreateCompatibleDC(m_screenDC);
    if (!m_memDC) return false;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = FRAME_W;
    bi.bmiHeader.biHeight = -FRAME_H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    m_memBmp = CreateDIBSection(m_memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!m_memBmp) return false;

    m_oldBmp = (HBITMAP)SelectObject(m_memDC, m_memBmp);
    return true;
}

void VrLowResPreview::CleanupGdi() {
    if (m_memDC && m_oldBmp) SelectObject(m_memDC, m_oldBmp);
    if (m_memBmp) { DeleteObject(m_memBmp); m_memBmp = nullptr; }
    if (m_memDC) { DeleteDC(m_memDC); m_memDC = nullptr; }
    if (m_screenDC) { ReleaseDC(nullptr, m_screenDC); m_screenDC = nullptr; }
}

bool VrLowResPreview::CaptureAndConvert(std::vector<uint16_t>& outRgb565) {
    SetStretchBltMode(m_memDC, HALFTONE);
    SetBrushOrgEx(m_memDC, 0, 0, nullptr);

    BOOL ok = StretchBlt(
        m_memDC, 0, 0, FRAME_W, FRAME_H,
        m_screenDC, m_captureX, m_captureY, m_captureW, m_captureH,
        SRCCOPY | CAPTUREBLT
    );
    if (!ok) return false;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = FRAME_W;
    bi.bmiHeader.biHeight = -FRAME_H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> bgra((size_t)FRAME_W * FRAME_H * 4);
    int lines = GetDIBits(m_screenDC, m_memBmp, 0, FRAME_H, bgra.data(), &bi, DIB_RGB_COLORS);
    if (lines != FRAME_H) return false;

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
#else
bool VrLowResPreview::InitGdi() {
    m_display = XOpenDisplay(NULL);
    if (m_display) {
        m_rootWindow = DefaultRootWindow(m_display);
    }
    return true;
}

void VrLowResPreview::CleanupGdi() {
    if (m_display) {
        XCloseDisplay(m_display);
        m_display = nullptr;
    }
}

void VrLowResPreview::ResolveCaptureRegion() {}

bool VrLowResPreview::CaptureAndConvert(std::vector<uint16_t>& outRgb565) {
    outRgb565.resize((size_t)FRAME_W * FRAME_H);
    if (m_display && m_rootWindow) {
        XImage* image = XGetImage(m_display, m_rootWindow, 0, 0, FRAME_W, FRAME_H, AllPlanes, ZPixmap);
        if (image) {
            for (int y = 0; y < FRAME_H; ++y) {
                for (int x = 0; x < FRAME_W; ++x) {
                    unsigned long pixel = XGetPixel(image, x, y);
                    uint8_t b = pixel & 0xFF;
                    uint8_t g = (pixel >> 8) & 0xFF;
                    uint8_t r = (pixel >> 16) & 0xFF;
                    uint16_t px = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                    outRgb565[y * FRAME_W + x] = px;
                }
            }
            XDestroyImage(image);
            return true;
        }
    }

    static uint8_t c = 0; c += 4;
    for (int i = 0; i < FRAME_W * FRAME_H; ++i) {
        outRgb565[i] = (uint16_t)(((c >> 3) << 11) | ((128 >> 2) << 5) | ((255 - c) >> 3));
    }
    return true;
}
#endif

bool VrLowResPreview::InitNetwork() {
    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) return false;

    SetSocketReuseAddr(m_sock);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_sock, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        closesocket(m_sock); m_sock = INVALID_SOCKET;
        return false;
    }

    SetSocketRecvTimeout(m_sock, 500);
    SetSocketSendBuffer(m_sock, 256 * 1024);
    return true;
}

void VrLowResPreview::CleanupNetwork() {
    if (m_sock != INVALID_SOCKET) { closesocket(m_sock); m_sock = INVALID_SOCKET; }
}

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
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        int n = recvfrom(m_sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue;

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
                char ipStr[32];
                inet_ntop(AF_INET, &from.sin_addr, ipStr, sizeof(ipStr));
                PrevLog("Nuevo suscriptor de preview: %s:%u", ipStr, ntohs(from.sin_port));
            }
        }
    }
}

void VrLowResPreview::SendFrameToSubscribers(const uint16_t* rgb565, uint32_t frameId) {
    std::vector<Subscriber> subsCopy;
    {
        std::lock_guard<std::mutex> lk(m_subsMtx);
        subsCopy = m_subscribers;
    }
    if (subsCopy.empty()) return;

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(rgb565);
    const int totalBytes = FRAME_W * FRAME_H * 2;
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

void VrLowResPreview::CaptureLoop() {
    if (!InitGdi()) return;

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
    CleanupNetwork();
    if (m_listenThread.joinable())  m_listenThread.join();
    if (m_captureThread.joinable()) m_captureThread.join();
}
