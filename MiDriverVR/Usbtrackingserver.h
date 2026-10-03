#pragma once

#include "Platform.h"
#include <initializer_list>

struct UsbTrackingCallbacks {
    void (*onPacket)(const void* packetBytes, size_t len) = nullptr;
    void (*onQuality)(const uint8_t* qualBytes13) = nullptr;
};

class UsbTrackingServer {
public:
    static constexpr uint16_t PORT = 47299;
    static constexpr int      PACKET_BYTES = 196;

    bool Start(UsbTrackingCallbacks callbacks);
    void Stop();
    ~UsbTrackingServer() { Stop(); }

    static void EnsureAdbReverseTunnelsAsync(
        std::initializer_list<uint16_t> extraPorts = {});

private:
    void AcceptLoop();
    void ClientLoop(SOCKET client);

    SOCKET m_listenSock = INVALID_SOCKET;
    std::atomic<bool> m_running{ false };
    std::thread m_acceptThread;
    std::mutex m_clientMtx;
    SOCKET m_activeClient = INVALID_SOCKET;

    UsbTrackingCallbacks m_cb{};
};
