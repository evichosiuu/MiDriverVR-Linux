#include "UsbTrackingServer.h"
#include <ws2tcpip.h>
#include <wincrypt.h>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

static void UsbLog(const char* fmt, ...) {
    char buf[256]; va_list va; va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va); va_end(va);
    OutputDebugStringA("[CamVR][USB] "); OutputDebugStringA(buf); OutputDebugStringA("\n");
}


static void LoadSecret(uint8_t out[16]) {
    const uint8_t a[4] = { 0x4b,0x3a,0x1f,0x08 }, b[4] = { 0xc2,0x77,0x9e,0x34 };
    const uint8_t c[4] = { 0x05,0xab,0x61,0xd9 }, d[4] = { 0xf8,0x2e,0x47,0xbc };
    memcpy(out, a, 4); memcpy(out + 4, b, 4); memcpy(out + 8, c, 4); memcpy(out + 12, d, 4);
}
static bool ComputeHMAC(const uint8_t* key, size_t keyLen,
    const uint8_t* data, size_t dataLen, uint8_t* out8) {
    HCRYPTPROV hp = 0; HCRYPTKEY hk = 0; HCRYPTHASH hh = 0; bool ok = false;
    struct { BLOBHEADER h; DWORD len; BYTE k[32]; } blob{};
    blob.h.bType = PLAINTEXTKEYBLOB; blob.h.bVersion = CUR_BLOB_VERSION;
    blob.h.aiKeyAlg = CALG_RC2; blob.len = (DWORD)keyLen;
    memcpy(blob.k, key, keyLen);
    if (!CryptAcquireContext(&hp, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return false;
    if (!CryptImportKey(hp, (BYTE*)&blob, sizeof(BLOBHEADER) + sizeof(DWORD) + (DWORD)keyLen, 0, CRYPT_IPSEC_HMAC_KEY, &hk)) goto end;
    {
        HMAC_INFO hi{}; hi.HashAlgid = CALG_SHA_256;
        if (!CryptCreateHash(hp, CALG_HMAC, hk, 0, &hh)) goto end;
        if (!CryptSetHashParam(hh, HP_HMAC_INFO, (BYTE*)&hi, 0)) goto end;
        if (!CryptHashData(hh, data, (DWORD)dataLen, 0)) goto end;
        DWORD hl = 32; uint8_t dig[32];
        if (!CryptGetHashParam(hh, HP_HASHVAL, dig, &hl, 0)) goto end;
        memcpy(out8, dig, 8); ok = true;
    }
end:
    if (hh)CryptDestroyHash(hh); if (hk)CryptDestroyKey(hk); if (hp)CryptReleaseContext(hp, 0);
    return ok;
}
static uint32_t CryptoRand32() {
    uint32_t v = 0; HCRYPTPROV hp = 0;
    if (CryptAcquireContext(&hp, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        CryptGenRandom(hp, 4, (BYTE*)&v); CryptReleaseContext(hp, 0);
    }
    else v = (uint32_t)(GetTickCount64() ^ (uintptr_t)&v);
    return v;
}


static void BE32(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
}
static bool RecvAll(SOCKET s, uint8_t* buf, int len) {
    int got = 0;
    while (got < len) {
        int n = recv(s, (char*)buf + got, len - got, 0);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}
static bool SendAll(SOCKET s, const uint8_t* buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, (const char*)buf + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}
static bool RecvFrame(SOCKET s, std::vector<uint8_t>& out, int maxLen = 4096) {
    uint8_t hdr[4];
    if (!RecvAll(s, hdr, 4)) return false;
    uint32_t len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
    if ((int)len > maxLen) return false; // frame absurdamente grande: cortar la conexion
    out.resize(len);
    if (len > 0 && !RecvAll(s, out.data(), (int)len)) return false;
    return true;
}
static bool SendFrame(SOCKET s, const uint8_t* payload, int len) {
    uint8_t hdr[4]; BE32(hdr, (uint32_t)len);
    return SendAll(s, hdr, 4) && SendAll(s, payload, len);
}


bool UsbTrackingServer::Start(UsbTrackingCallbacks callbacks) {
    if (m_running.exchange(true)) return true;
    m_cb = callbacks;

    m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSock == INVALID_SOCKET) { m_running = false; return false; }
    int reuse = 1; setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_listenSock, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        UsbLog("bind fallo err=%d (puerto %u ya en uso?)", WSAGetLastError(), PORT);
        closesocket(m_listenSock); m_listenSock = INVALID_SOCKET; m_running = false; return false;
    }
    if (listen(m_listenSock, 1) == SOCKET_ERROR) {
        UsbLog("listen fallo err=%d", WSAGetLastError());
        closesocket(m_listenSock); m_listenSock = INVALID_SOCKET; m_running = false; return false;
    }
    DWORD to = 1000; setsockopt(m_listenSock, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));

    m_acceptThread = std::thread([this] { AcceptLoop(); });
    UsbLog("Listener de tracking USB activo en TCP:%u (esperando 'adb reverse tcp:%u tcp:%u')", PORT, PORT, PORT);
    return true;
}

void UsbTrackingServer::Stop() {
    if (!m_running.exchange(false)) return;
    if (m_listenSock != INVALID_SOCKET) { closesocket(m_listenSock); m_listenSock = INVALID_SOCKET; }
    {
        std::lock_guard<std::mutex> lk(m_clientMtx);
        if (m_activeClient != INVALID_SOCKET) { closesocket(m_activeClient); m_activeClient = INVALID_SOCKET; }
    }
    if (m_acceptThread.joinable()) m_acceptThread.join();
}

void UsbTrackingServer::AcceptLoop() {
    while (m_running.load()) {
        sockaddr_in cl{}; int cl_len = sizeof(cl);
        SOCKET s = accept(m_listenSock, (sockaddr*)&cl, &cl_len);
        if (s == INVALID_SOCKET) continue; // timeout normal (SO_RCVTIMEO), reintentar
        int flag = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
        {
            std::lock_guard<std::mutex> lk(m_clientMtx);
            if (m_activeClient != INVALID_SOCKET) closesocket(m_activeClient);
            m_activeClient = s;
        }
        UsbLog("Cliente USB conectado, iniciando handshake");

        ClientLoop(s);
    }
}

void UsbTrackingServer::ClientLoop(SOCKET client) {
    uint8_t secret[16]; LoadSecret(secret);
    uint32_t token = 0;
    std::vector<uint8_t> frame;
    bool handshakeOk = false;


    do {
        if (!RecvFrame(client, frame) || frame.size() != 8 || memcmp(frame.data(), "CVRHELLO", 8) != 0) {
            UsbLog("Handshake USB: CVRHELLO invalido o ausente"); break;
        }
        uint32_t r1 = CryptoRand32(), r2 = CryptoRand32();
        uint8_t challenge[8]; memcpy(challenge, &r1, 4); memcpy(challenge + 4, &r2, 4);
        uint8_t chlg[12]; memcpy(chlg, "CHLG", 4); memcpy(chlg + 4, challenge, 8);
        if (!SendFrame(client, chlg, 12)) { UsbLog("Handshake USB: fallo enviando CHLG"); break; }

        if (!RecvFrame(client, frame) || frame.size() != 12 || memcmp(frame.data(), "RESP", 4) != 0) {
            UsbLog("Handshake USB: RESP invalido o ausente"); break;
        }
        uint8_t exp[8];
        if (!ComputeHMAC(secret, 16, challenge, 8, exp) || memcmp(frame.data() + 4, exp, 8) != 0) {
            UsbLog("Handshake USB: HMAC no coincide (clave incorrecta?)"); break;
        }

        token = CryptoRand32();
        uint8_t tokn[8]; memcpy(tokn, "TOKN", 4); memcpy(tokn + 4, &token, 4);
        if (!SendFrame(client, tokn, 8)) { UsbLog("Handshake USB: fallo enviando TOKN"); break; }

        handshakeOk = true;
    } while (false);

    if (handshakeOk) {
        UsbLog("Handshake USB OK, token=0x%08X - streaming de tracking iniciado", token);

 
        while (m_running.load()) {
            if (!RecvFrame(client, frame)) break; // cliente desconectado
            if ((int)frame.size() == PACKET_BYTES) {
                uint32_t rt; memcpy(&rt, frame.data(), 4);
                if (rt != token) continue; // token viejo/ajeno, ignorar
                if (m_cb.onPacket) m_cb.onPacket(frame.data(), frame.size());
            }
            else if (frame.size() == 13 && memcmp(frame.data(), "QUAL", 4) == 0) {
                if (m_cb.onQuality) m_cb.onQuality(frame.data());
            }
 
        }
    }

    closesocket(client);
    {
        std::lock_guard<std::mutex> lk(m_clientMtx);
        if (m_activeClient == client) m_activeClient = INVALID_SOCKET;
    }
    UsbLog("Cliente USB desconectado");
}


static void RunAdbReverse(uint16_t port) {
    char cmd[128];
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "adb reverse tcp:%u tcp:%u", port, port);

    STARTUPINFOA si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 3000);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        if (exitCode == 0) UsbLog("adb reverse tcp:%u tcp:%u OK", port, port);
        else UsbLog("adb reverse tcp:%u tcp:%u salio con codigo %lu (sin dispositivo conectado/autorizado todavia?)", port, port, exitCode);
    }
    else {
        UsbLog("No se pudo lanzar adb.exe (err=%lu) - asegurate de que 'adb' este en el PATH "
            "(platform-tools de Android SDK). El modo USB no estara disponible; "
            "el modo WiFi sigue funcionando igual.", GetLastError());
    }
}

void UsbTrackingServer::EnsureAdbReverseTunnelsAsync(std::initializer_list<uint16_t> extraPorts) {
    std::vector<uint16_t> ports = { PORT };
    for (auto p : extraPorts) ports.push_back(p);

    std::thread([ports] {

        for (int i = 0; i < 60; ++i) {
            for (auto port : ports) RunAdbReverse(port);
            Sleep(5000);
        }
        }).detach();
}