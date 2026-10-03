#pragma once

#include "Platform.h"
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>

#ifndef _WIN32
  #include <X11/Xlib.h>
  #include <X11/Xutil.h>
#endif

class VrLowResPreview {
public:
    static constexpr uint16_t PORT = 47299;
    static constexpr int      FRAME_W = 200;
    static constexpr int      FRAME_H = 120;
    static constexpr int      CHUNK_MAX = 1400;
    static constexpr int      HDR_BYTES = 14;

    static constexpr ULONGLONG SUBSCRIBER_TIMEOUT_MS = 2000;
    static constexpr int SEND_INTERVAL_MS = 180;

    bool Start();
    void Stop();
    ~VrLowResPreview() { Stop(); }

private:
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

    void ListenLoop();
    void CaptureLoop();

    void PruneSubscribers();
    void SendFrameToSubscribers(const uint16_t* rgb565, uint32_t frameId);

#ifdef _WIN32
    HDC m_screenDC = nullptr;
    HDC m_memDC = nullptr;
    HBITMAP m_memBmp = nullptr;
    HBITMAP m_oldBmp = nullptr;
    int m_captureX = 0, m_captureY = 0, m_captureW = 0, m_captureH = 0;
#else
    Display* m_display = nullptr;
    Window m_rootWindow = 0;
#endif

    bool InitGdi();
    void CleanupGdi();
    void ResolveCaptureRegion();
    bool CaptureAndConvert(std::vector<uint16_t>& outRgb565);

    static void BE16(uint8_t* p, uint16_t v) { p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF; }
    static void BE32(uint8_t* p, uint32_t v) {
        p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
    }
};
