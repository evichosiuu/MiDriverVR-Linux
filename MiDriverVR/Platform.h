#pragma once

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <chrono>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <wincrypt.h>
  #include <iphlpapi.h>
  #include <mmsystem.h>
  #pragma comment(lib, "ws2_32.lib")
  #pragma comment(lib, "crypt32.lib")
  #pragma comment(lib, "iphlpapi.lib")

  #define DRIVER_EXPORT __declspec(dllexport)
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <netdb.h>
  #include <ifaddrs.h>
  #include <net/if.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <time.h>
  #include <sys/time.h>
  #include <openssl/hmac.h>
  #include <openssl/rand.h>
  #include <openssl/evp.h>

  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR (-1)
  #define closesocket(s) close(s)
  #define SD_BOTH SHUT_RDWR
  typedef uint64_t ULONGLONG;
  typedef uint32_t DWORD;
  typedef uint8_t BYTE;
  typedef int32_t LONG;
  typedef uint32_t ULONG;
  typedef void* HANDLE;
  typedef void* HWND;

  union LARGE_INTEGER {
      struct {
          DWORD LowPart;
          LONG HighPart;
      } u;
      int64_t QuadPart;
  };

  inline int WSAGetLastError() { return errno; }

  inline ULONGLONG GetTickCount64() {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (ULONGLONG)ts.tv_sec * 1000 + (ULONGLONG)ts.tv_nsec / 1000000;
  }

  inline void Sleep(uint32_t ms) {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  }

  #define DRIVER_EXPORT __attribute__((visibility("default")))
#endif

inline void DbgLog(const char* fmt, ...) {
    char buf[512];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
#ifdef _WIN32
    OutputDebugStringA("[MiDriverVR] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
#else
    fprintf(stderr, "[MiDriverVR] %s\n", buf);
    fflush(stderr);
#endif
}

inline void SetSocketRecvTimeout(SOCKET s, uint32_t ms) {
#ifdef _WIN32
    DWORD to = ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif
}

inline void SetSocketSendBuffer(SOCKET s, int sizeBytes) {
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&sizeBytes, sizeof(sizeBytes));
}

inline void SetSocketReuseAddr(SOCKET s) {
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
}

inline void SetSocketNoDelay(SOCKET s) {
    int flag = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
}

inline bool ComputeHMAC(const uint8_t* key, size_t keyLen,
                        const uint8_t* data, size_t dataLen, uint8_t* out8) {
#ifdef _WIN32
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
    if (hh) CryptDestroyHash(hh);
    if (hk) CryptDestroyKey(hk);
    if (hp) CryptReleaseContext(hp, 0);
    return ok;
#else
    unsigned int len = 0;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned char* res = HMAC(EVP_sha256(), key, (int)keyLen, data, dataLen, digest, &len);
    if (!res) return false;
    memcpy(out8, digest, 8);
    return true;
#endif
}

inline uint32_t CryptoRand32() {
    uint32_t v = 0;
#ifdef _WIN32
    HCRYPTPROV hp = 0;
    if (CryptAcquireContext(&hp, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        CryptGenRandom(hp, 4, (BYTE*)&v);
        CryptReleaseContext(hp, 0);
    } else {
        v = (uint32_t)(GetTickCount64() ^ (uintptr_t)&v);
    }
#else
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&v), sizeof(v)) != 1) {
        v = (uint32_t)(GetTickCount64() ^ (uintptr_t)&v);
    }
#endif
    return v;
}

inline std::vector<sockaddr_in> GetSubnetBroadcastAddressesPort(uint16_t announcePort) {
    std::vector<sockaddr_in> result;
#ifdef _WIN32
    ULONG bufLen = 15000;
    std::vector<uint8_t> buf(bufLen);
    PIP_ADAPTER_ADDRESSES addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());

    ULONG ret = GetAdaptersAddresses(
        AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, addrs, &bufLen);

    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
        ret = GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, addrs, &bufLen);
    }

    if (ret == NO_ERROR) {
        for (auto* a = addrs; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

            for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
                if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;

                sockaddr_in* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                ULONG prefixLen = ua->OnLinkPrefixLength;
                if (prefixLen == 0 || prefixLen > 32) continue;

                uint32_t ip = sa->sin_addr.s_addr;
                uint32_t mask = (prefixLen == 32) ? 0xFFFFFFFFu : htonl(~0u << (32 - prefixLen));
                uint32_t bcast = (ip & mask) | ~mask;

                sockaddr_in dest{};
                dest.sin_family = AF_INET;
                dest.sin_port = htons(announcePort);
                dest.sin_addr.s_addr = bcast;

                result.push_back(dest);
            }
        }
    }
#else
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) == 0) {
        for (struct ifaddrs* ifa = ifap; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;
            if (!(ifa->ifa_flags & IFF_UP)) continue;

            sockaddr_in* sa = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_port = htons(announcePort);

            if ((ifa->ifa_flags & IFF_BROADCAST) && ifa->ifa_broadaddr && ifa->ifa_broadaddr->sa_family == AF_INET) {
                sockaddr_in* bsa = reinterpret_cast<sockaddr_in*>(ifa->ifa_broadaddr);
                dest.sin_addr.s_addr = bsa->sin_addr.s_addr;
            } else if (ifa->ifa_netmask && ifa->ifa_netmask->sa_family == AF_INET) {
                sockaddr_in* nmsa = reinterpret_cast<sockaddr_in*>(ifa->ifa_netmask);
                dest.sin_addr.s_addr = sa->sin_addr.s_addr | ~(nmsa->sin_addr.s_addr);
            } else {
                dest.sin_addr.s_addr = htonl(ntohl(sa->sin_addr.s_addr) | 0x000000FF);
            }
            result.push_back(dest);
        }
        freeifaddrs(ifap);
    }
#endif
    return result;
}
