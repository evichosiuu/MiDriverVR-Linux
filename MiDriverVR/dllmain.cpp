#include <openvr_driver.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <icodecapi.h>
#include <codecapi.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>
#include <iphlpapi.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include "GpuEncodePipeline.h"
#include "HandSkeleton.h"
#include "VrLowResPreview.h" 
#include "UsbTrackingServer.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "iphlpapi.lib")

using namespace vr;


static uint16_t ResolveDataPort() { volatile uint16_t a = 0xa000u, b = 0x18bbu; return a ^ b; }
static uint16_t ResolveAuthPort() { volatile uint16_t a = 0xa000u, b = 0x18bcu; return a ^ b; }
static uint16_t ResolveAnnouncePort() { return 47294; }
static uint16_t ResolveStreamPort() { return 47295; }

static uint32_t ResolveBaseCode() {
    volatile uint32_t _rx = 0x1832e80u, _ry = 0x1828f22u;
    volatile uint32_t _x = (_rx >> 7u) | (_rx << 25u);
    return _x + (_ry ^ _rx);
}
static void LoadSecret(uint8_t out[16]) {
    const uint8_t _a[4] = { 0x4b,0x3a,0x1f,0x08 }, _b[4] = { 0xc2,0x77,0x9e,0x34 };
    const uint8_t _c[4] = { 0x05,0xab,0x61,0xd9 }, _d[4] = { 0xf8,0x2e,0x47,0xbc };
    memcpy(out, _a, 4); memcpy(out + 4, _b, 4); memcpy(out + 8, _c, 4); memcpy(out + 12, _d, 4);
}


static void DbgLog(const char* fmt, ...) {
    char buf[256]; va_list va; va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va); va_end(va);
    OutputDebugStringA("[CamVR] "); OutputDebugStringA(buf); OutputDebugStringA("\n");
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
    if (CryptAcquireContext(&hp, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
    {
        CryptGenRandom(hp, 4, (BYTE*)&v); CryptReleaseContext(hp, 0);
    }
    else { v = (uint32_t)(GetTickCount64() ^ (uintptr_t)&v); }
    return v;
}


struct HandData {
    float x, y, z, qx, qy, qz, qw, trigger, grip, joyX, joyY, sysBtn, appBtn, clickBtn, isTracked;
    float curlThumb, curlIndex, curlMiddle, curlRing, curlPinky;
};
struct HmdData { float x, y, z, qx, qy, qz, qw, isTracked; };
#define PACKET_FLOATS 49
#define PACKET_BYTES  (PACKET_FLOATS*4)
struct TrackingPacket { float token; HandData left, right; HmdData hmd; };
static_assert(sizeof(TrackingPacket) == PACKET_BYTES, "size mismatch");


struct ClientSession { bool active = false; uint32_t token = 0; sockaddr_in addr = {}; uint8_t challenge[8] = {}; ULONGLONG lastDataMs = 0; };
static ClientSession g_kbdSession, g_phoneSession;
static TrackingPacket g_kbdPkt = {}, g_phonePkt = {}, g_cur = {}, g_tgt = {};
static const ULONGLONG SESSION_TIMEOUT_MS = 2000;
static bool IsLoopbackAddr(const sockaddr_in& a) { return (ntohl(a.sin_addr.s_addr) >> 24) == 127u; }


static std::atomic<bool> g_hmdFlipped{ false };

static uint16_t ResolveFlipPort() { return 47297; }

static SOCKET      g_flipSock = INVALID_SOCKET;
static std::thread g_flipThread;
static std::atomic<bool> g_flipRunning{ false };

static void FlipListenLoop() {
    uint8_t buf[16];
    while (g_flipRunning.load()) {
        sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(g_flipSock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue; // timeout normal, se reintenta
        if (n != 4 || memcmp(buf, "FLIP", 4) != 0) continue;
        if ((ntohl(from.sin_addr.s_addr) >> 24) != 127u) continue; // solo loopback

        g_hmdFlipped = !g_hmdFlipped.load();
        DbgLog("FLIP recibido (Python): HMD flip %s", g_hmdFlipped.load() ? "ACTIVADO" : "desactivado");
    }
}

static void StartHotkeyListener() {
    g_flipSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_flipSock == INVALID_SOCKET) { DbgLog("Flip listener: socket() fallo"); return; }
    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_port = htons(ResolveFlipPort()); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_flipSock, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        DbgLog("Flip listener: bind fallo err=%d", WSAGetLastError());
        closesocket(g_flipSock); g_flipSock = INVALID_SOCKET;
        return;
    }
    DWORD to = 500; setsockopt(g_flipSock, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));
    g_flipRunning = true;
    g_flipThread = std::thread(FlipListenLoop);
    DbgLog("Flip listener activo puerto UDP:%u (esperando paquetes FLIP desde Python)", ResolveFlipPort());
}
static void StopHotkeyListener() {
    g_flipRunning = false;
    if (g_flipSock != INVALID_SOCKET) { closesocket(g_flipSock); g_flipSock = INVALID_SOCKET; }
    if (g_flipThread.joinable()) g_flipThread.join();
}


static std::atomic<bool> g_kbdHotkeyRunning{ false };
static std::thread       g_kbdHotkeyThread;

static void KeyboardHotkeyLoop() {
    bool wasComboPressed = false;
    while (g_kbdHotkeyRunning.load()) {
        bool zDown = (GetAsyncKeyState('Z') & 0x8000) != 0;
        bool nineDown = (GetAsyncKeyState('9') & 0x8000) != 0;
        bool comboPressed = zDown && nineDown;

        if (comboPressed && !wasComboPressed) {
            g_hmdFlipped = !g_hmdFlipped.load();
            DbgLog("Hotkey Z+9: HMD flip %s", g_hmdFlipped.load() ? "ACTIVADO" : "desactivado");
        }
        wasComboPressed = comboPressed;
        Sleep(30);
    }
}
static void StartKeyboardHotkey() {
    g_kbdHotkeyRunning = true;
    g_kbdHotkeyThread = std::thread(KeyboardHotkeyLoop);
    DbgLog("Hotkey de teclado activo: Z+9 alterna el flip de HMD");
}
static void StopKeyboardHotkey() {
    g_kbdHotkeyRunning = false;
    if (g_kbdHotkeyThread.joinable()) g_kbdHotkeyThread.join();
}


static double g_flipAnchorX = 0.0, g_flipAnchorY = 0.0, g_flipAnchorZ = 0.0;
static bool   g_wasFlipped = false;


static HmdQuaternion_t MulQ(const HmdQuaternion_t& a, const HmdQuaternion_t& b) {
    return { a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
}
static HmdQuaternion_t MatQ(const HmdMatrix34_t& m) {
    HmdQuaternion_t q;
    q.w = sqrt(fmax(0., 1. + m.m[0][0] + m.m[1][1] + m.m[2][2])) / 2.;
    q.x = sqrt(fmax(0., 1. + m.m[0][0] - m.m[1][1] - m.m[2][2])) / 2.;
    q.y = sqrt(fmax(0., 1. - m.m[0][0] + m.m[1][1] - m.m[2][2])) / 2.;
    q.z = sqrt(fmax(0., 1. - m.m[0][0] - m.m[1][1] + m.m[2][2])) / 2.;
    q.x = _copysign(q.x, m.m[2][1] - m.m[1][2]); q.y = _copysign(q.y, m.m[0][2] - m.m[2][0]);
    q.z = _copysign(q.z, m.m[1][0] - m.m[0][1]); return q;
}


static const uint32_t WINDOW_W = 1920, WINDOW_H = 1080;
static const int32_t  WINDOW_X = 0, WINDOW_Y = 0;
class CMyDisplayComponent : public IVRDisplayComponent {
public:
    void GetWindowBounds(int32_t* x, int32_t* y, uint32_t* w, uint32_t* h) override
    {
        *x = WINDOW_X; *y = WINDOW_Y; *w = WINDOW_W; *h = WINDOW_H;
    }
    bool IsDisplayOnDesktop()   override { return true; }
    bool IsDisplayRealDisplay() override { return false; }
    void GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) override { *w = WINDOW_W; *h = WINDOW_H; }
    void GetEyeOutputViewport(EVREye e, uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h) override
    {
        *y = 0; *w = WINDOW_W / 2; *h = WINDOW_H; *x = (e == Eye_Left) ? 0 : (WINDOW_W / 2);
    }
    void GetProjectionRaw(EVREye, float* l, float* r, float* t, float* b) override
    {
        *l = -1.f; *r = 1.f; *t = -1.f; *b = 1.f;
    }
    DistortionCoordinates_t ComputeDistortion(EVREye, float u, float v) override
    {
        DistortionCoordinates_t c; c.rfRed[0] = u; c.rfRed[1] = v; c.rfGreen[0] = u; c.rfGreen[1] = v; c.rfBlue[0] = u; c.rfBlue[1] = v; return c;
    }
    bool ComputeInverseDistortion(HmdVector2_t*, EVREye, uint32_t, float, float) override { return false; }
};
static CMyDisplayComponent g_displayComponent;


class CMyHmd : public ITrackedDeviceServerDriver {
public:
    uint32_t m_id = k_unTrackedDeviceIndexInvalid;
    EVRInitError Activate(uint32_t id) override {
        m_id = id; auto p = VRProperties()->TrackedDeviceToPropertyContainer(m_id);
        VRProperties()->SetStringProperty(p, Prop_ModelNumber_String, "CamHMD Virtual");
        VRProperties()->SetStringProperty(p, Prop_ManufacturerName_String, "Custom");
        VRProperties()->SetStringProperty(p, Prop_TrackingSystemName_String, "Camera");
        VRProperties()->SetFloatProperty(p, Prop_UserIpdMeters_Float, 0.063f);
        VRProperties()->SetFloatProperty(p, Prop_DisplayFrequency_Float, 60.0f);
        VRProperties()->SetFloatProperty(p, Prop_SecondsFromVsyncToPhotons_Float, 0.011f);
        VRProperties()->SetBoolProperty(p, Prop_IsOnDesktop_Bool, true);
        return VRInitError_None;
    }
    DriverPose_t GetPose() override {
        DriverPose_t pose{}; pose.qWorldFromDriverRotation = { 1,0,0,0 }; pose.qDriverFromHeadRotation = { 1,0,0,0 };
        pose.poseIsValid = true; pose.deviceIsConnected = true;
        const HmdData& h = g_cur.hmd;
        if (h.isTracked < 0.5f) {
            pose.result = TrackingResult_Running_OutOfRange;
            pose.vecPosition[0] = 0.; pose.vecPosition[1] = 1.6; pose.vecPosition[2] = 0.; pose.qRotation = { 1,0,0,0 }; return pose;
        }
        pose.result = TrackingResult_Running_OK;

        HmdQuaternion_t q = { h.qw, h.qx, h.qy, h.qz };


        double px = h.x, py = h.y, pz = h.z;
        if (g_hmdFlipped.load()) {
            static const HmdQuaternion_t kFlip180 = { 0.0, 0.0, 0.0, 1.0 };
            q = MulQ(kFlip180, q);
            px = 2.0 * g_flipAnchorX - h.x;
            py = 2.0 * g_flipAnchorY - h.y;
            pz = h.z;

            static int s_logCount = 0;
            if (s_logCount++ % 30 == 0) {
                DbgLog("FLIP debug: h.z=%.4f anchor=%.4f delta=%.4f -> pz=%.4f",
                    h.z, g_flipAnchorZ, h.z - g_flipAnchorZ, pz);
            }
        }

        pose.vecPosition[0] = px; pose.vecPosition[1] = py; pose.vecPosition[2] = pz;
        pose.qRotation = q;
        return pose;
    }
    void Deactivate() override { m_id = k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {} void DebugRequest(const char*, char*, uint32_t) override {}
    void* GetComponent(const char* i) override
    {
        if (strcmp(IVRDisplayComponent_Version, i) == 0)return &g_displayComponent; return nullptr;
    }
};
static CMyHmd g_hmd;


class CMyController : public ITrackedDeviceServerDriver {
public:
    uint32_t m_id = k_unTrackedDeviceIndexInvalid;
    ETrackedControllerRole m_role;
    VRInputComponentHandle_t hTrig = 0, hGrip = 0, hPX = 0, hPY = 0, hPC = 0, hPT = 0, hSys = 0, hApp = 0;
    VRInputComponentHandle_t hSkeleton = 0;
    explicit CMyController(ETrackedControllerRole r) :m_role(r) {}
    EVRInitError Activate(uint32_t id) override {
        m_id = id; auto p = VRProperties()->TrackedDeviceToPropertyContainer(m_id);
        VRProperties()->SetStringProperty(p, Prop_ModelNumber_String, "Vive Controller MV");
        VRProperties()->SetStringProperty(p, Prop_RenderModelName_String, "vr_controller_vive_1_5");
        VRProperties()->SetStringProperty(p, Prop_ManufacturerName_String, "HTC");
        VRProperties()->SetStringProperty(p, Prop_InputProfilePath_String, "{htc}/input/vive_controller_profile.json");
        VRProperties()->SetInt32Property(p, Prop_ControllerRoleHint_Int32, m_role);
        VRDriverInput()->CreateScalarComponent(p, "/input/trigger/value", &hTrig, VRScalarType_Absolute, VRScalarUnits_NormalizedOneSided);
        VRDriverInput()->CreateScalarComponent(p, "/input/grip/value", &hGrip, VRScalarType_Absolute, VRScalarUnits_NormalizedOneSided);
        VRDriverInput()->CreateScalarComponent(p, "/input/trackpad/x", &hPX, VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);
        VRDriverInput()->CreateScalarComponent(p, "/input/trackpad/y", &hPY, VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);
        VRDriverInput()->CreateBooleanComponent(p, "/input/trackpad/click", &hPC);
        VRDriverInput()->CreateBooleanComponent(p, "/input/trackpad/touch", &hPT);
        VRDriverInput()->CreateBooleanComponent(p, "/input/system/click", &hSys);
        VRDriverInput()->CreateBooleanComponent(p, "/input/application_menu/click", &hApp);


        const bool isRight = (m_role == TrackedControllerRole_RightHand);
        const char* compName = isRight ? "/input/skeleton/right" : "/input/skeleton/left";
        const char* skeletonPath = isRight ? "/skeleton/hand/right" : "/skeleton/hand/left";
        EVRInputError skErr = VRDriverInput()->CreateSkeletonComponent(
            p, compName, skeletonPath, "/pose/raw",
            VRSkeletalTracking_Partial, nullptr, 0, &hSkeleton);
        if (skErr != VRInputError_None) {
            DbgLog("CreateSkeletonComponent fallo err=%d (%s)", (int)skErr, compName);
        }

        return VRInitError_None;
    }
    DriverPose_t GetPose() override {
        DriverPose_t pose{}; pose.result = TrackingResult_Running_OK;
        pose.deviceIsConnected = true; pose.poseIsValid = true;
        pose.qWorldFromDriverRotation = { 1,0,0,0 }; pose.qDriverFromHeadRotation = { 1,0,0,0 };
        HandData* h = (m_role == TrackedControllerRole_LeftHand) ? &g_cur.left : &g_cur.right;
        if (h->isTracked < .5f) { pose.poseIsValid = false; return pose; }
        const float OX = 0.f, OY = -0.15f, OZ = 0.f;

        const HmdQuaternion_t handQ = { h->qw, h->qx, h->qy, h->qz };


        auto rotateVecByQuat = [](const HmdQuaternion_t& q, float vx, float vy, float vz,
            float& outX, float& outY, float& outZ) {

                HmdQuaternion_t v = { 0, vx, vy, vz };
                HmdQuaternion_t qInv = { q.w, -q.x, -q.y, -q.z };
                HmdQuaternion_t t = MulQ(MulQ(q, v), qInv);
                outX = t.x; outY = t.y; outZ = t.z;
            };

        float offX, offY, offZ;
        rotateVecByQuat(handQ, OX, OY, OZ, offX, offY, offZ);

        TrackedDevicePose_t hmd; VRServerDriverHost()->GetRawTrackedDevicePoses(0, &hmd, 1);
        if (hmd.bPoseIsValid) {
            const auto& m = hmd.mDeviceToAbsoluteTracking;
            float lx = h->x + offX, ly = h->y + offY, lz = h->z + offZ;
            pose.vecPosition[0] = m.m[0][3] + m.m[0][0] * lx + m.m[0][1] * ly + m.m[0][2] * lz;
            pose.vecPosition[1] = m.m[1][3] + m.m[1][0] * lx + m.m[1][1] * ly + m.m[1][2] * lz;
            pose.vecPosition[2] = m.m[2][3] + m.m[2][0] * lx + m.m[2][1] * ly + m.m[2][2] * lz;
            pose.qRotation = MulQ(MatQ(m), handQ);
        }
        else {
            pose.vecPosition[0] = h->x + offX; pose.vecPosition[1] = h->y + offY; pose.vecPosition[2] = h->z + offZ;
            pose.qRotation = handQ;
        }
        return pose;
    }
    void UpdateInputs() {
        if (m_id == k_unTrackedDeviceIndexInvalid)return;
        HandData* h = (m_role == TrackedControllerRole_LeftHand) ? &g_cur.left : &g_cur.right;
        VRDriverInput()->UpdateScalarComponent(hTrig, h->trigger, 0.);
        VRDriverInput()->UpdateScalarComponent(hGrip, h->grip, 0.);
        VRDriverInput()->UpdateScalarComponent(hPX, h->joyX, 0.);
        VRDriverInput()->UpdateScalarComponent(hPY, h->joyY, 0.);
        VRDriverInput()->UpdateBooleanComponent(hSys, h->sysBtn > .5f, 0.);
        VRDriverInput()->UpdateBooleanComponent(hApp, h->appBtn > .5f, 0.);
        VRDriverInput()->UpdateBooleanComponent(hPC, h->clickBtn > .5f, 0.);
        VRDriverInput()->UpdateBooleanComponent(hPT, fabsf(h->joyX) > .1f || fabsf(h->joyY) > .1f || h->clickBtn > .5f, 0.);

        // ── Skeletal Input: recalcula los 31 huesos con el curl actual
        //    y los sube a SteamVR en los dos rangos de movimiento. ──
        if (hSkeleton != 0) {
            const bool isRight = (m_role == TrackedControllerRole_RightHand);
            const float curl[5] = { h->curlThumb, h->curlIndex, h->curlMiddle, h->curlRing, h->curlPinky };
            vr::VRBoneTransform_t bones[HandSkeleton::kBoneCount];
            HandSkeleton::BuildHandSkeleton(isRight, curl, bones);
            VRDriverInput()->UpdateSkeletonComponent(hSkeleton, VRSkeletalMotionRange_WithoutController, bones, HandSkeleton::kBoneCount);
            VRDriverInput()->UpdateSkeletonComponent(hSkeleton, VRSkeletalMotionRange_WithController, bones, HandSkeleton::kBoneCount);
        }
    }
    void Deactivate() override { m_id = k_unTrackedDeviceIndexInvalid; }
    void* GetComponent(const char*) override { return nullptr; }
    void EnterStandby() override {} void DebugRequest(const char*, char*, uint32_t) override {}
};
static CMyController g_left(TrackedControllerRole_LeftHand);
static CMyController g_right(TrackedControllerRole_RightHand);


class VrH264Streamer {
public:

    int      m_encW = 1280;
    int      m_encH = 720;
    uint32_t m_fps = 30;
    uint32_t m_bitrate = 6000000;
    uint32_t m_gopSize = 30;

    static constexpr ULONGLONG HW_WATCHDOG_MS = 1200;


    static constexpr ULONGLONG CAPTURE_REGION_RETRY_MS = 2000;

    bool Start() {
        if (m_running.exchange(true)) return true;
        if (!InitNetwork()) { m_running = false; return false; }
        if (!InitQualityListener()) {
            DbgLog("Aviso: no se pudo iniciar el listener de calidad (puerto %u) - "
                "los cambios de calidad desde el telefono NO se aplicaran",
                ResolveQualityPort());
        }
        m_acceptThread = std::thread([this] {AcceptLoop(); });
        m_captureThread = std::thread([this] {CaptureLoop(); });
        DbgLog("Streamer GDI iniciado puerto TCP:%u", ResolveStreamPort());
        return true;
    }
    void Stop() {
        if (!m_running.exchange(false)) return;
        CleanupNetwork();
        if (m_acceptThread.joinable())  m_acceptThread.join();
        if (m_captureThread.joinable()) m_captureThread.join();
        if (m_qualityThread.joinable()) m_qualityThread.join();
    }
    ~VrH264Streamer() { Stop(); }


    void QueueExternalQuality(const uint8_t* qual13) {
        if (!qual13 || memcmp(qual13, "QUAL", 4) != 0) return;
        uint16_t w = (uint16_t)((qual13[4] << 8) | qual13[5]);
        uint16_t h = (uint16_t)((qual13[6] << 8) | qual13[7]);
        uint32_t br = ((uint32_t)qual13[8] << 24) | ((uint32_t)qual13[9] << 16) |
            ((uint32_t)qual13[10] << 8) | (uint32_t)qual13[11];
        uint8_t  fps = qual13[12];
        DbgLog("QUAL recibido (USB/ADB): %ux%u @%ukbps %ufps", w, h, br / 1000, fps);
        std::lock_guard<std::mutex> lk(m_pendingQualityMtx);
        m_pendingW = w; m_pendingH = h;
        m_pendingBitrate = br; m_pendingFps = fps;
        m_hasPendingQuality = true;
    }

private:
    // GDI
    HDC     m_screenDC = nullptr;
    HDC     m_memDC = nullptr;
    HBITMAP m_memBmp = nullptr;
    HBITMAP m_oldBmp = nullptr;
    int     m_desktopW = 0;
    int     m_desktopH = 0;

    int m_captureX = 0;
    int m_captureY = 0;
    int m_captureW = 0;
    int m_captureH = 0;


    bool      m_capturingRealMonitor = false;
    ULONGLONG m_lastCaptureRegionRetryMs = 0;

    std::vector<uint8_t> m_bgraFrame;

    GpuEncodePipeline m_gpu;
    MftDriver         m_mftDriver;


    IMFTransform* m_enc = nullptr;
    DWORD         m_inSt = 0, m_outSt = 0;
    LONGLONG      m_pts = 0;
    bool          m_usingHwEncoder = false;


    ULONGLONG         m_encoderInitTickMs = 0;
    std::atomic<int>  m_nalsSinceInit{ 0 };
    bool              m_hwFallbackDone = false;


    std::atomic<bool> m_qualityOverrideActive{ false };
    std::mutex        m_pendingQualityMtx;
    bool              m_hasPendingQuality = false;
    int               m_pendingW = 0, m_pendingH = 0;
    uint32_t          m_pendingBitrate = 0;
    uint32_t          m_pendingFps = 0;

    SOCKET      m_qualitySock = INVALID_SOCKET;
    std::thread m_qualityThread;

    static uint16_t ResolveQualityPort() { return 47296; }

    // Network
    SOCKET     m_listenSock = INVALID_SOCKET;
    SOCKET     m_clientSock = INVALID_SOCKET;
    std::mutex m_sockMtx;
    std::atomic<bool> m_needIDR{ false };

    // Threads
    std::atomic<bool> m_running{ false };
    std::thread m_acceptThread, m_captureThread;

    static void BE32(uint8_t* p, uint32_t v) {
        p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
    }


    struct MonitorRect { RECT rc; bool isPrimary; };

    static BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
        auto* list = reinterpret_cast<std::vector<MonitorRect>*>(lp);
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        if (GetMonitorInfo(hMon, &mi)) {
            list->push_back({ mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 });
        }
        return TRUE;
    }


    bool ResolveCaptureRegion() {
        std::vector<MonitorRect> monitors;
        EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&monitors));

        DbgLog("ResolveCaptureRegion: %zu monitor(es) detectado(s)", monitors.size());
        for (auto& m : monitors) {
            DbgLog("  - (%ld,%ld) %ldx%ld primario=%d",
                m.rc.left, m.rc.top, m.rc.right - m.rc.left, m.rc.bottom - m.rc.top, (int)m.isPrimary);
        }

        const MonitorRect* chosen = nullptr;
        bool exactMatch = false;


        for (auto& m : monitors) {
            LONG w = m.rc.right - m.rc.left, h = m.rc.bottom - m.rc.top;
            if ((UINT32)w == WINDOW_W && (UINT32)h == WINDOW_H && !m.isPrimary) {
                chosen = &m;
                exactMatch = true;
                break;
            }
        }


        if (!chosen) {
            for (auto& m : monitors) {
                if (!m.isPrimary) { chosen = &m; break; }
            }
        }

        if (chosen) {
            m_captureX = chosen->rc.left;
            m_captureY = chosen->rc.top;
            m_captureW = chosen->rc.right - chosen->rc.left;
            m_captureH = chosen->rc.bottom - chosen->rc.top;
            m_desktopW = m_captureW;
            m_desktopH = m_captureH;
            DbgLog("ResolveCaptureRegion: usando monitor VIRTUAL en (%d,%d) %dx%d (match_exacto=%d)",
                m_captureX, m_captureY, m_captureW, m_captureH, (int)exactMatch);
            return exactMatch;
        }


        m_desktopW = GetSystemMetrics(SM_CXSCREEN);
        m_desktopH = GetSystemMetrics(SM_CYSCREEN);
        m_captureX = 0; m_captureY = 0;
        m_captureW = m_desktopW; m_captureH = m_desktopH;
        DbgLog("ResolveCaptureRegion: no se encontro un monitor separado, usando el primario completo (%dx%d) - "
            "se reintentara la deteccion cada %llums hasta encontrar el monitor virtual real",
            m_desktopW, m_desktopH, (unsigned long long)CAPTURE_REGION_RETRY_MS);
        return false;
    }

    bool InitGdi() {
        m_screenDC = GetDC(nullptr);
        if (!m_screenDC) { DbgLog("GetDC(nullptr) fallo err=%u", (unsigned)GetLastError()); return false; }


        m_capturingRealMonitor = ResolveCaptureRegion();
        m_lastCaptureRegionRetryMs = GetTickCount64();

        m_memDC = CreateCompatibleDC(m_screenDC);
        if (!m_memDC) { DbgLog("CreateCompatibleDC fallo err=%u", (unsigned)GetLastError()); return false; }

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = m_encW;
        bi.bmiHeader.biHeight = -m_encH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        m_memBmp = CreateDIBSection(m_memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!m_memBmp) { DbgLog("CreateDIBSection fallo err=%u", (unsigned)GetLastError()); return false; }

        m_oldBmp = (HBITMAP)SelectObject(m_memDC, m_memBmp);

        m_bgraFrame.resize((size_t)m_encW * m_encH * 4);

        DbgLog("GDI init OK: captura (%d,%d) %dx%d -> encode %dx%d",
            m_captureX, m_captureY, m_captureW, m_captureH, m_encW, m_encH);
        return true;
    }
    void CleanupGdi() {
        if (m_memDC && m_oldBmp) SelectObject(m_memDC, m_oldBmp);
        if (m_memBmp) { DeleteObject(m_memBmp); m_memBmp = nullptr; }
        if (m_memDC) { DeleteDC(m_memDC);    m_memDC = nullptr; }
        if (m_screenDC) { ReleaseDC(nullptr, m_screenDC); m_screenDC = nullptr; }
    }


    void RetryCaptureRegionIfNeeded() {
        if (m_capturingRealMonitor) return;
        ULONGLONG now = GetTickCount64();
        if (now - m_lastCaptureRegionRetryMs < CAPTURE_REGION_RETRY_MS) return;
        m_lastCaptureRegionRetryMs = now;

        bool found = ResolveCaptureRegion();
        if (found) {
            m_capturingRealMonitor = true;
            m_needIDR.store(true);
            DbgLog("RetryCaptureRegionIfNeeded: monitor virtual encontrado - "
                "capturando la fuente correcta a partir de ahora, IDR forzado");
        }
    }

    bool CaptureFrame() {
        SetStretchBltMode(m_memDC, HALFTONE);
        SetBrushOrgEx(m_memDC, 0, 0, nullptr);

        BOOL ok = StretchBlt(
            m_memDC, 0, 0, m_encW, m_encH,
            m_screenDC, m_captureX, m_captureY, m_captureW, m_captureH,
            SRCCOPY | CAPTUREBLT
        );
        if (!ok) { DbgLog("StretchBlt fallo err=%u", (unsigned)GetLastError()); return false; }

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = m_encW;
        bi.bmiHeader.biHeight = -m_encH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        int lines = GetDIBits(m_screenDC, m_memBmp, 0, m_encH,
            m_bgraFrame.data(), &bi, DIB_RGB_COLORS);
        if (lines != m_encH) {
            DbgLog("GetDIBits: lines=%d esperado=%d err=%u", lines, m_encH, (unsigned)GetLastError());
            return false;
        }
        return true;
    }


    bool TryInitSoftwareFromCandidates(IMFActivate** acts, UINT32 cnt) {
        if (cnt == 0) {
            DbgLog("InitEncoder [SOFTWARE]: enumeracion devolvio 0 MFTs");
            return false;
        }
        DbgLog("InitEncoder [SOFTWARE]: %u candidato(s) encontrados, probando el primero", cnt);

        HRESULT hr = acts[0]->ActivateObject(__uuidof(IMFTransform), (void**)&m_enc);
        if (FAILED(hr)) {
            DbgLog("InitEncoder [SOFTWARE]: ActivateObject FAILED 0x%08X", (unsigned)hr);
            m_enc = nullptr;
            return false;
        }

        {
            IMFAttributes* attr = nullptr;
            if (SUCCEEDED(m_enc->GetAttributes(&attr))) { attr->SetUINT32(MF_LOW_LATENCY, TRUE); attr->Release(); }
        }

        IMFMediaType* omt = nullptr; MFCreateMediaType(&omt);
        omt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        omt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        MFSetAttributeSize(omt, MF_MT_FRAME_SIZE, (UINT32)m_encW, (UINT32)m_encH);
        MFSetAttributeRatio(omt, MF_MT_FRAME_RATE, m_fps, 1);
        omt->SetUINT32(MF_MT_AVG_BITRATE, m_bitrate);
        omt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        omt->SetUINT32(MF_MT_MPEG2_PROFILE, 66);
        omt->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_Wide);
        omt->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601);
        hr = m_enc->SetOutputType(m_outSt, omt, 0); omt->Release();
        if (FAILED(hr)) {
            DbgLog("InitEncoder [SOFTWARE]: SetOutputType FAILED 0x%08X", (unsigned)hr);
            m_enc->Release(); m_enc = nullptr;
            return false;
        }

        IMFMediaType* imt = nullptr; MFCreateMediaType(&imt);
        imt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        imt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(imt, MF_MT_FRAME_SIZE, (UINT32)m_encW, (UINT32)m_encH);
        MFSetAttributeRatio(imt, MF_MT_FRAME_RATE, m_fps, 1);
        imt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        imt->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_Wide);
        imt->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601);
        hr = m_enc->SetInputType(m_inSt, imt, 0); imt->Release();
        if (FAILED(hr)) {
            DbgLog("InitEncoder [SOFTWARE]: SetInputType FAILED 0x%08X", (unsigned)hr);
            m_enc->Release(); m_enc = nullptr;
            return false;
        }

        GpuEncodePipeline::ApplyLowLatencyCodecSettings(m_enc, m_bitrate, m_gopSize);

        {

            ICodecAPI* codec = nullptr;
            if (SUCCEEDED(m_enc->QueryInterface(IID_PPV_ARGS(&codec)))) {
                VARIANT v; VariantInit(&v); v.vt = VT_UI4; v.ulVal = 0; // 0 = auto (todos los cores)
                codec->SetValue(&CODECAPI_AVEncNumWorkerThreads, &v);
                codec->Release();
            }
        }

        m_enc->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        m_enc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_enc->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        m_mftDriver.Attach(m_enc); // sincrono para software; el driver lo detecta solo
        DbgLog("InitEncoder [SOFTWARE]: OK %dx%d @%ufps %ukbps GOP=%u", m_encW, m_encH, m_fps, m_bitrate / 1000, m_gopSize);
        return true;
    }


    void ReleaseEncoderOnly() {
        m_mftDriver.Detach();
        if (m_enc) { m_enc->Release(); m_enc = nullptr; }
    }

    bool InitEncoder() {
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE) {
            DbgLog("CoInitializeEx FAILED 0x%08X", (unsigned)hrCo);
        }
        HRESULT hrMf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hrMf)) {
            DbgLog("MFStartup FAILED 0x%08X", (unsigned)hrMf);
            return false;
        }


        bool gpuOk = m_gpu.DetectAndCreateDevice();
        if (gpuOk) {
            if (m_qualityOverrideActive) {
                m_gpu.OverrideProfile((uint32_t)m_encW, (uint32_t)m_encH, m_fps, m_bitrate);
            }
            else {
                const EncodeProfile& prof = m_gpu.Profile();
                m_encW = (int)prof.width; m_encH = (int)prof.height;
                m_fps = prof.fps; m_bitrate = prof.bitrate; m_gopSize = prof.fps;
            }
            m_gpu.CreateTexturePool(3);
        }


        if (gpuOk) {
            ComPtr<IMFTransform> hwEnc;
            MftDriver hwDriver;
            DWORD inSt = 0, outSt = 0;
            if (m_gpu.SelectAndInitHardwareEncoder(hwEnc, hwDriver, inSt, outSt)) {
                m_enc = hwEnc.Detach(); // nos quedamos con la referencia (ComPtr la libera si no)
                m_mftDriver = std::move(hwDriver);
                m_inSt = inSt; m_outSt = outSt;
                m_usingHwEncoder = true;
                m_encoderInitTickMs = GetTickCount64();
                m_nalsSinceInit = 0;
                DbgLog("InitEncoder: HARDWARE confirmado y en uso (%s, async=%d)",
                    m_gpu.HasDedicatedGpu() ? "GPU dedicada" : "GPU no dedicada", (int)m_mftDriver.IsAsync());
                return true;
            }
            DbgLog("InitEncoder: ningun candidato de HARDWARE paso el smoke test, cayendo a SOFTWARE");
        }
        else {
            DbgLog("InitEncoder: sin D3D Manager disponible (fallo deteccion/creacion de GPU), se omite HARDWARE por completo");
        }


        if (InitEncoderSoftwareOnly()) return true;

        DbgLog("InitEncoder: NINGUN encoder H.264 disponible (ni HW ni SW). El stream no podra iniciar.");
        return false;
    }


    bool InitEncoderSoftwareOnly() {
        MFT_REGISTER_TYPE_INFO ti = { MFMediaType_Video,MFVideoFormat_NV12 };
        MFT_REGISTER_TYPE_INFO to = { MFMediaType_Video,MFVideoFormat_H264 };
        IMFActivate** acts = nullptr; UINT32 cnt = 0;
        MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &ti, &to, &acts, &cnt);
        bool ok = TryInitSoftwareFromCandidates(acts, cnt);
        for (UINT32 i = 0; i < cnt; i++) acts[i]->Release();
        if (acts) CoTaskMemFree(acts);
        if (ok) {
            m_usingHwEncoder = false;
            m_encoderInitTickMs = GetTickCount64();
            m_nalsSinceInit = 0;
        }
        return ok;
    }

    void CleanupEncoder() {
        ReleaseEncoderOnly();
        m_gpu.Shutdown();
        MFShutdown(); CoUninitialize();
    }


    bool CheckHwWatchdogAndMaybeFallback() {
        if (!m_usingHwEncoder || m_hwFallbackDone) return false;
        ULONGLONG elapsed = GetTickCount64() - m_encoderInitTickMs;
        if (elapsed < HW_WATCHDOG_MS) return false;
        if (m_nalsSinceInit.load() > 0) return false; // esta produciendo output, todo bien

        DbgLog("WATCHDOG: encoder HARDWARE no genero NINGUN NAL en %llums - forzando fallback a SOFTWARE",
            (unsigned long long)elapsed);
        ReleaseEncoderOnly();
        m_hwFallbackDone = true; // no reintentar HW de nuevo en esta sesion
        if (!InitEncoderSoftwareOnly()) {
            DbgLog("WATCHDOG: fallback a SOFTWARE tambien fallo. Sin encoder disponible.");
            return false;
        }
        DbgLog("WATCHDOG: fallback a SOFTWARE completado OK, forzando IDR nuevo");
        return true;
    }


    bool InitNetwork() {
        m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listenSock == INVALID_SOCKET) return false;
        int reuse = 1; setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));
        int sndbuf = 4 * 1024 * 1024; setsockopt(m_listenSock, SOL_SOCKET, SO_SNDBUF, (char*)&sndbuf, sizeof(sndbuf));
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(ResolveStreamPort()); a.sin_addr.s_addr = INADDR_ANY;
        if (bind(m_listenSock, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) { DbgLog("bind fallo err=%d", WSAGetLastError()); return false; }
        if (listen(m_listenSock, 1) == SOCKET_ERROR) { DbgLog("listen fallo err=%d", WSAGetLastError()); return false; }
        DWORD to = 1000; setsockopt(m_listenSock, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));
        return true;
    }
    void CleanupNetwork() {
        {
            std::lock_guard<std::mutex> lk(m_sockMtx);
            if (m_clientSock != INVALID_SOCKET) { closesocket(m_clientSock); m_clientSock = INVALID_SOCKET; }
        }
        if (m_listenSock != INVALID_SOCKET) { closesocket(m_listenSock); m_listenSock = INVALID_SOCKET; }
        if (m_qualitySock != INVALID_SOCKET) { closesocket(m_qualitySock); m_qualitySock = INVALID_SOCKET; }
    }
    bool SendAll(const uint8_t* d, int len) {
        std::lock_guard<std::mutex> lk(m_sockMtx);
        if (m_clientSock == INVALID_SOCKET) return false;
        int sent = 0;
        while (sent < len) {
            int n = send(m_clientSock, (const char*)d + sent, len - sent, 0);
            if (n <= 0) { DbgLog("send() fallo n=%d err=%d", n, WSAGetLastError()); return false; }
            sent += n;
        }
        return true;
    }
    bool SendNal(const uint8_t* d, int len) {
        uint8_t hdr[4]; BE32(hdr, (uint32_t)len);
        return SendAll(hdr, 4) && SendAll(d, len);
    }


    void AcceptLoop() {
        while (m_running.load()) {
            sockaddr_in cl{}; int cl_len = sizeof(cl);
            SOCKET s = accept(m_listenSock, (sockaddr*)&cl, &cl_len);
            if (s == INVALID_SOCKET) continue;
            int flag = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
            int sndbuf = 4 * 1024 * 1024; setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&sndbuf, sizeof(sndbuf));
            {
                std::lock_guard<std::mutex> lk(m_sockMtx);
                if (m_clientSock != INVALID_SOCKET) closesocket(m_clientSock);
                m_clientSock = s;
            }
            m_needIDR.store(true);
            DbgLog("Cliente stream conectado - forzando IDR");
        }
    }


    bool InitQualityListener() {
        m_qualitySock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_qualitySock == INVALID_SOCKET) return false;
        sockaddr_in a{}; a.sin_family = AF_INET;
        a.sin_port = htons(ResolveQualityPort()); a.sin_addr.s_addr = INADDR_ANY;
        if (bind(m_qualitySock, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
            DbgLog("Quality listener: bind fallo err=%d", WSAGetLastError());
            closesocket(m_qualitySock); m_qualitySock = INVALID_SOCKET;
            return false;
        }
        DWORD to = 500; setsockopt(m_qualitySock, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));
        m_qualityThread = std::thread([this] { QualityListenLoop(); });
        DbgLog("Quality listener activo puerto UDP:%u", ResolveQualityPort());
        return true;
    }

    void QualityListenLoop() {
        uint8_t buf[32];
        while (m_running.load()) {
            sockaddr_in from{}; int fl = sizeof(from);
            int n = recvfrom(m_qualitySock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);

            if (n != 13) continue;
            if (memcmp(buf, "QUAL", 4) != 0) continue;

            uint16_t w = (uint16_t)((buf[4] << 8) | buf[5]);
            uint16_t h = (uint16_t)((buf[6] << 8) | buf[7]);
            uint32_t br = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
                ((uint32_t)buf[10] << 8) | (uint32_t)buf[11];
            uint8_t  fps = buf[12];

            DbgLog("QUAL recibido: %ux%u @%ukbps %ufps", w, h, br / 1000, fps);

            std::lock_guard<std::mutex> lk(m_pendingQualityMtx);
            m_pendingW = w; m_pendingH = h;
            m_pendingBitrate = br; m_pendingFps = fps;
            m_hasPendingQuality = true;
        }
    }


    void ApplyQualityChange(int w, int h, uint32_t bitrate, uint32_t fps) {
        if (w <= 0 || h <= 0 || fps == 0) return;
        DbgLog("Aplicando cambio de calidad: %dx%d @%ukbps %ufps (encoder previo: %s)",
            w, h, bitrate / 1000, fps, m_usingHwEncoder ? "HARDWARE" : "SOFTWARE");

        CleanupGdi();
        ReleaseEncoderOnly();
        m_gpu.Shutdown();


        m_qualityOverrideActive = true;
        m_encW = w; m_encH = h; m_bitrate = bitrate; m_fps = fps; m_gopSize = fps;

        if (!InitEncoder()) {
            DbgLog("ApplyQualityChange: InitEncoder fallo tras cambio de calidad - "
                "el stream puede quedarse sin video hasta reconectar");
            return;
        }
        if (!InitGdi()) {
            DbgLog("ApplyQualityChange: InitGdi fallo tras cambio de calidad");
            return;
        }
        m_needIDR.store(true);
        DbgLog("Cambio de calidad aplicado OK: %dx%d @%ukbps %ufps (%s)",
            m_encW, m_encH, m_bitrate / 1000, m_fps, m_usingHwEncoder ? "HARDWARE" : "SOFTWARE");
    }

    void CaptureLoop() {
        HRESULT hrCoCapture = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hrCoCapture) && hrCoCapture != RPC_E_CHANGED_MODE) {
            DbgLog("CaptureLoop: CoInitializeEx FAILED 0x%08X", (unsigned)hrCoCapture);
        }

        bool timerHighRes = (timeBeginPeriod(1) == TIMERR_NOERROR);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

        LARGE_INTEGER qpcFreq{};
        QueryPerformanceFrequency(&qpcFreq);

        m_hwFallbackDone = false;

        if (!InitEncoder()) {
            DbgLog("InitEncoder fallo (HW y SW) - hilo termina. SIN ESTO NO HABRA VIDEO (pantalla negra).");
            if (timerHighRes) timeEndPeriod(1);
            CoUninitialize(); return;
        }

        if (!InitGdi()) {
            DbgLog("InitGdi fallo en hilo de captura - hilo termina. SIN ESTO NO HABRA VIDEO.");
            CleanupEncoder();
            if (timerHighRes) timeEndPeriod(1);
            CoUninitialize(); return;
        }

        DbgLog("CaptureLoop activo. Encoder en uso: %s (%dx%d @%ufps %ukbps) monitor_real=%d",
            m_usingHwEncoder ? "HARDWARE" : "SOFTWARE", m_encW, m_encH, m_fps, m_bitrate / 1000,
            (int)m_capturingRealMonitor);

        double frameMs = 1000.0 / (m_fps > 0 ? m_fps : 30);
        int frameCount = 0;
        int consecutiveCaptureFails = 0;

        while (m_running.load()) {
            LARGE_INTEGER t0; QueryPerformanceCounter(&t0);

            {
                std::lock_guard<std::mutex> lk(m_sockMtx);
                if (m_clientSock == INVALID_SOCKET) { Sleep(50); continue; }
            }


            RetryCaptureRegionIfNeeded();


            if (CheckHwWatchdogAndMaybeFallback()) {
                m_needIDR.store(true);
            }


            {
                bool applyNow = false;
                int newW = 0, newH = 0; uint32_t newBr = 0, newFps = 0;
                {
                    std::lock_guard<std::mutex> lk(m_pendingQualityMtx);
                    if (m_hasPendingQuality) {
                        applyNow = true;
                        newW = m_pendingW; newH = m_pendingH;
                        newBr = m_pendingBitrate; newFps = m_pendingFps;
                        m_hasPendingQuality = false;
                    }
                }
                if (applyNow) {
                    ApplyQualityChange(newW, newH, newBr, newFps);

                    frameMs = 1000.0 / (m_fps > 0 ? m_fps : 30);
                }
            }

            if (!CaptureFrame()) {
                consecutiveCaptureFails++;
                if (consecutiveCaptureFails == 1 || consecutiveCaptureFails % 30 == 0)
                    DbgLog("CaptureFrame fallando repetidamente (%d veces seguidas)", consecutiveCaptureFails);
                Sleep(5); continue;
            }
            consecutiveCaptureFails = 0;

            if (!EncodeAndSend()) {
                std::lock_guard<std::mutex> lk(m_sockMtx);
                if (m_clientSock != INVALID_SOCKET) {
                    closesocket(m_clientSock); m_clientSock = INVALID_SOCKET;
                    DbgLog("Cliente desconectado (fallo de envio)");
                }
                continue;
            }
            frameCount++;
            if (frameCount == 1) DbgLog("Primer frame codificado y enviado OK (%s)", m_usingHwEncoder ? "HARDWARE" : "SOFTWARE");
            if (frameCount % 60 == 0) DbgLog("Stream: %d frames (%s)", frameCount, m_usingHwEncoder ? "HARDWARE" : "SOFTWARE");


            LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
            double elapsedMs = (t1.QuadPart - t0.QuadPart) * 1000.0 / qpcFreq.QuadPart;
            if (elapsedMs < frameMs) {
                DWORD sleepMs = (DWORD)(frameMs - elapsedMs);
                if (sleepMs > 2) Sleep(sleepMs - 1);
                do {
                    QueryPerformanceCounter(&t1);
                    elapsedMs = (t1.QuadPart - t0.QuadPart) * 1000.0 / qpcFreq.QuadPart;
                } while (elapsedMs < frameMs);
            }
        }
        CleanupGdi();
        CleanupEncoder();
        if (timerHighRes) timeEndPeriod(1);
        CoUninitialize();
    }


    bool EncodeAndSend() {
        const int W = m_encW, H = m_encH;
        const uint8_t* bgra = m_bgraFrame.data();

        std::vector<uint8_t> nv12((size_t)W * H + (size_t)W * (H / 2), 0);
        uint8_t* pY = nv12.data();
        uint8_t* pUV = nv12.data() + (size_t)W * H;

        for (int row = 0; row < H; ++row) {
            const uint8_t* line = bgra + row * W * 4;
            uint8_t* dstY = pY + row * W;
            for (int col = 0; col < W; ++col) {
                uint8_t b = line[col * 4 + 0], g = line[col * 4 + 1], r = line[col * 4 + 2];
                dstY[col] = (uint8_t)(((77 * r + 150 * g + 29 * b + 128) >> 8));
            }
            if ((row & 1) == 0) {
                uint8_t* dstUV = pUV + (row / 2) * W;
                for (int col = 0; col < W; col += 2) {
                    uint8_t b = line[col * 4 + 0], g = line[col * 4 + 1], r = line[col * 4 + 2];
                    dstUV[col] = (uint8_t)(((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128);
                    dstUV[col + 1] = (uint8_t)(((128 * r - 107 * g - 21 * b + 128) >> 8) + 128);
                }
            }
        }

        if (m_needIDR.exchange(false)) {
            m_enc->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            m_enc->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            m_enc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            m_enc->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            m_pts = 0;
            DbgLog("IDR forzado OK");
        }

        IMFSample* sample = nullptr;
        IMFMediaBuffer* mfBuf = nullptr;
        MFCreateMemoryBuffer((DWORD)nv12.size(), &mfBuf);
        BYTE* pb = nullptr; mfBuf->Lock(&pb, nullptr, nullptr);
        memcpy(pb, nv12.data(), nv12.size());
        mfBuf->Unlock(); mfBuf->SetCurrentLength((DWORD)nv12.size());
        MFCreateSample(&sample); sample->AddBuffer(mfBuf); mfBuf->Release();
        const LONGLONG dur = 10000000LL / (m_fps > 0 ? m_fps : 30);
        sample->SetSampleTime(m_pts); sample->SetSampleDuration(dur); m_pts += dur;


        if (m_mftDriver.IsAsync() && !m_mftDriver.ReadyForInput()) {
            sample->Release();
            return true;
        }

        HRESULT hr = m_mftDriver.SubmitInput(sample);
        sample->Release();
        if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
            static int s_logCount = 0;
            if (s_logCount < 5) { DbgLog("SubmitInput FAILED 0x%08X", (unsigned)hr); s_logCount++; }
            return true;
        }

        bool ok = true;
        int outputsThisCall = 0;
        while (true) {
            ComPtr<IMFSample> outSample;
            HRESULT ohr = m_mftDriver.TryGetOutput(m_outSt, outSample);
            if (ohr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (FAILED(ohr)) {
                static int s_logCount2 = 0;
                if (s_logCount2 < 5) { DbgLog("TryGetOutput FAILED 0x%08X", (unsigned)ohr); s_logCount2++; }
                break;
            }
            if (!outSample) break;

            IMFMediaBuffer* outBuf = nullptr;
            outSample->ConvertToContiguousBuffer(&outBuf);
            BYTE* pOut = nullptr; DWORD curLen = 0;
            outBuf->Lock(&pOut, nullptr, &curLen);
            if (curLen > 0) {
                if (!SendNal(pOut, (int)curLen)) ok = false;
                else outputsThisCall++;
            }
            outBuf->Unlock(); outBuf->Release();
            if (!ok) break;
        }

        if (outputsThisCall > 0) {
            m_nalsSinceInit.fetch_add(outputsThisCall);
        }
        else if (ok) {
            static int s_zeroOutputLog = 0;
            if (s_zeroOutputLog < 5) {
                DbgLog("EncodeAndSend: 0 NALs generados en este frame (normal en los primeros por buffering interno del MFT)");
                s_zeroOutputLog++;
            }
        }

        return ok;
    }
};

static VrH264Streamer g_streamer;
static VrLowResPreview g_lowResPreview;


class VrAudioStreamer {
public:
    static constexpr uint16_t PORT = 47298;
    static constexpr int      OUT_SAMPLE_RATE = 48000;
    static constexpr int      OUT_CHANNELS = 2;

    bool Start() {
        if (m_running.exchange(true)) return true;
        if (!InitNetwork()) { m_running = false; return false; }
        m_acceptThread = std::thread([this] { AcceptLoop(); });
        m_captureThread = std::thread([this] { CaptureLoop(); });
        DbgLog("AudioStreamer iniciado puerto TCP:%u (WASAPI loopback -> PCM16 %dHz %dch)",
            PORT, OUT_SAMPLE_RATE, OUT_CHANNELS);
        return true;
    }
    void Stop() {
        if (!m_running.exchange(false)) return;
        CleanupNetwork();
        if (m_acceptThread.joinable())  m_acceptThread.join();
        if (m_captureThread.joinable()) m_captureThread.join();
    }
    ~VrAudioStreamer() { Stop(); }

private:
    SOCKET     m_listenSock = INVALID_SOCKET;
    SOCKET     m_clientSock = INVALID_SOCKET;
    std::mutex m_sockMtx;
    std::atomic<bool> m_running{ false };
    std::thread m_acceptThread, m_captureThread;

    static void BE32(uint8_t* p, uint32_t v) {
        p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
    }

    bool InitNetwork() {
        m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listenSock == INVALID_SOCKET) return false;
        int reuse = 1; setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = INADDR_ANY;
        if (bind(m_listenSock, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) { DbgLog("Audio: bind fallo err=%d", WSAGetLastError()); return false; }
        if (listen(m_listenSock, 1) == SOCKET_ERROR) { DbgLog("Audio: listen fallo err=%d", WSAGetLastError()); return false; }
        DWORD to = 1000; setsockopt(m_listenSock, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));
        return true;
    }
    void CleanupNetwork() {
        {
            std::lock_guard<std::mutex> lk(m_sockMtx);
            if (m_clientSock != INVALID_SOCKET) { closesocket(m_clientSock); m_clientSock = INVALID_SOCKET; }
        }
        if (m_listenSock != INVALID_SOCKET) { closesocket(m_listenSock); m_listenSock = INVALID_SOCKET; }
    }
    bool SendAll(const uint8_t* d, int len) {
        std::lock_guard<std::mutex> lk(m_sockMtx);
        if (m_clientSock == INVALID_SOCKET) return false;
        int sent = 0;
        while (sent < len) {
            int n = send(m_clientSock, (const char*)d + sent, len - sent, 0);
            if (n <= 0) { DbgLog("Audio: send() fallo n=%d err=%d", n, WSAGetLastError()); return false; }
            sent += n;
        }
        return true;
    }
    bool SendFramed(const uint8_t* d, int len) {
        if (len <= 0) return true;
        uint8_t hdr[4]; BE32(hdr, (uint32_t)len);
        return SendAll(hdr, 4) && SendAll(d, len);
    }

    void AcceptLoop() {
        while (m_running.load()) {
            sockaddr_in cl{}; int cl_len = sizeof(cl);
            SOCKET s = accept(m_listenSock, (sockaddr*)&cl, &cl_len);
            if (s == INVALID_SOCKET) continue;
            int flag = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
            {
                std::lock_guard<std::mutex> lk(m_sockMtx);
                if (m_clientSock != INVALID_SOCKET) closesocket(m_clientSock);
                m_clientSock = s;
            }
            DbgLog("Audio: cliente conectado");
        }
    }


    double m_resamplePos = 0.0;
    float  m_lastL = 0.f, m_lastR = 0.f;

    void ExtractStereoFloat(const BYTE* src, UINT32 frame, const WAVEFORMATEX* wfx, float& l, float& r) {
        const int ch = wfx->nChannels;
        const bool isFloat =
            wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        const int bytesPerSample = wfx->wBitsPerSample / 8;
        const BYTE* frameStart = src + (size_t)frame * wfx->nBlockAlign;

        auto sampleAt = [&](int chan) -> float {
            const BYTE* p = frameStart + (size_t)chan * bytesPerSample;
            if (isFloat)                       return *reinterpret_cast<const float*>(p);
            if (wfx->wBitsPerSample == 16)      return (*reinterpret_cast<const int16_t*>(p)) / 32768.0f;
            if (wfx->wBitsPerSample == 32)      return (*reinterpret_cast<const int32_t*>(p)) / 2147483648.0f;
            return 0.f;
            };

        if (ch <= 1) {
            l = r = sampleAt(0);
        }
        else {

            l = sampleAt(0);
            r = sampleAt(1);
            for (int c = 2; c < ch; ++c) {
                float extra = sampleAt(c);
                l += extra * 0.5f;
                r += extra * 0.5f;
            }
        }
    }


    void ConvertAndResample(const BYTE* src, UINT32 frames, const WAVEFORMATEX* wfx,
        bool silent, std::vector<int16_t>& out) {
        if (frames == 0) return;
        const double ratio = (double)wfx->nSamplesPerSec / (double)OUT_SAMPLE_RATE;

        auto getFrame = [&](int idx, float& l, float& r) {
            if (silent) { l = r = 0.f; return; }
            if (idx < 0) { l = m_lastL; r = m_lastR; return; }
            if ((UINT32)idx >= frames) idx = frames - 1;
            ExtractStereoFloat(src, (UINT32)idx, wfx, l, r);
            };

        double pos = m_resamplePos;
        while (pos < (double)frames) {
            int i0 = (int)floor(pos) - 1; // -1 -> usa m_lastL/R como muestra previa
            int i1 = i0 + 1;
            double frac = pos - floor(pos);

            float l0, r0, l1, r1;
            getFrame(i0, l0, r0);
            getFrame(i1, l1, r1);

            float l = l0 + (float)((l1 - l0) * frac);
            float r = r0 + (float)((r1 - r0) * frac);
            l = l < -1.f ? -1.f : (l > 1.f ? 1.f : l);
            r = r < -1.f ? -1.f : (r > 1.f ? 1.f : r);

            out.push_back((int16_t)lrintf(l * 32767.0f));
            out.push_back((int16_t)lrintf(r * 32767.0f));

            pos += ratio;
        }
        m_resamplePos = pos - (double)frames;

        if (silent) m_lastL = m_lastR = 0.f;
        else        ExtractStereoFloat(src, frames - 1, wfx, m_lastL, m_lastR);
    }

    // ---- Capture loop --------------------------------------------------
    void CaptureLoop() {
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE) {
            DbgLog("Audio: CoInitializeEx FAILED 0x%08X", (unsigned)hrCo);
            return;
        }

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* capture = nullptr;
        WAVEFORMATEX* wfx = nullptr;
        HANDLE                hTask = nullptr;

        auto cleanupAll = [&]() {
            if (client)     client->Stop();
            if (capture)    capture->Release();
            if (wfx)        CoTaskMemFree(wfx);
            if (client)     client->Release();
            if (device)     device->Release();
            if (enumerator) enumerator->Release();
            if (hTask)      AvRevertMmThreadCharacteristics(hTask);
            CoUninitialize();
            };

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
        if (FAILED(hr)) { DbgLog("Audio: CoCreateInstance(MMDeviceEnumerator) FAILED 0x%08X", (unsigned)hr); cleanupAll(); return; }

        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) { DbgLog("Audio: GetDefaultAudioEndpoint FAILED 0x%08X (sin dispositivo de salida predeterminado?)", (unsigned)hr); cleanupAll(); return; }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
        if (FAILED(hr)) { DbgLog("Audio: Activate(IAudioClient) FAILED 0x%08X", (unsigned)hr); cleanupAll(); return; }

        hr = client->GetMixFormat(&wfx);
        if (FAILED(hr)) { DbgLog("Audio: GetMixFormat FAILED 0x%08X", (unsigned)hr); cleanupAll(); return; }

        DbgLog("Audio: mix format nativo = %uHz %uch %ubit tag=%u",
            wfx->nSamplesPerSec, wfx->nChannels, wfx->wBitsPerSample, wfx->wFormatTag);

        const REFERENCE_TIME bufDur = 20 * 10000; // 20ms en unidades de 100ns
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
            bufDur, 0, wfx, nullptr);
        if (FAILED(hr)) { DbgLog("Audio: Initialize FAILED 0x%08X", (unsigned)hr); cleanupAll(); return; }

        hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
        if (FAILED(hr)) { DbgLog("Audio: GetService(IAudioCaptureClient) FAILED 0x%08X", (unsigned)hr); cleanupAll(); return; }

        DWORD taskIdx = 0;
        hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);

        hr = client->Start();
        if (FAILED(hr)) { DbgLog("Audio: client->Start() FAILED 0x%08X", (unsigned)hr); cleanupAll(); return; }

        DbgLog("Audio: captura WASAPI loopback iniciada OK");

        std::vector<int16_t> pcmOut;
        pcmOut.reserve(OUT_SAMPLE_RATE / 10 * OUT_CHANNELS); // ~100ms

        while (m_running.load()) {
            {
                std::lock_guard<std::mutex> lk(m_sockMtx);
                if (m_clientSock == INVALID_SOCKET) { Sleep(50); continue; }
            }

            UINT32 packetLength = 0;
            hr = capture->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) { DbgLog("Audio: GetNextPacketSize FAILED 0x%08X", (unsigned)hr); Sleep(10); continue; }
            if (packetLength == 0) { Sleep(5); continue; }

            pcmOut.clear();
            while (packetLength != 0) {
                BYTE* data = nullptr;
                UINT32 framesAvail = 0;
                DWORD flags = 0;
                hr = capture->GetBuffer(&data, &framesAvail, &flags, nullptr, nullptr);
                if (FAILED(hr)) { DbgLog("Audio: GetBuffer FAILED 0x%08X", (unsigned)hr); break; }

                bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                ConvertAndResample(data, framesAvail, wfx, silent, pcmOut);

                hr = capture->ReleaseBuffer(framesAvail);
                if (FAILED(hr)) { DbgLog("Audio: ReleaseBuffer FAILED 0x%08X", (unsigned)hr); break; }

                hr = capture->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) { DbgLog("Audio: GetNextPacketSize(loop) FAILED 0x%08X", (unsigned)hr); break; }
            }

            if (!pcmOut.empty()) {
                if (!SendFramed(reinterpret_cast<const uint8_t*>(pcmOut.data()),
                    (int)(pcmOut.size() * sizeof(int16_t)))) {
                    std::lock_guard<std::mutex> lk(m_sockMtx);
                    if (m_clientSock != INVALID_SOCKET) {
                        closesocket(m_clientSock); m_clientSock = INVALID_SOCKET;
                        DbgLog("Audio: cliente desconectado (fallo de envio)");
                    }
                }
            }
        }

        cleanupAll();
        DbgLog("Audio: hilo de captura terminado");
    }
};

static VrAudioStreamer g_audioStreamer;

static TrackingPacket         g_usbPkt = {};
static std::mutex             g_usbPktMtx;
static std::atomic<ULONGLONG> g_usbLastDataMs{ 0 };
static std::atomic<bool>      g_usbActive{ false };

static void OnUsbTrackingPacket(const void* data, size_t len) {
    if (len != (size_t)PACKET_BYTES) return;
    std::lock_guard<std::mutex> lk(g_usbPktMtx);
    memcpy(&g_usbPkt, data, PACKET_BYTES);
    g_usbLastDataMs.store(GetTickCount64());
    g_usbActive.store(true);
}
static void OnUsbTrackingQuality(const uint8_t* qual13) {
    g_streamer.QueueExternalQuality(qual13);
}
static UsbTrackingServer g_usbTracking;


static std::vector<sockaddr_in> GetSubnetBroadcastAddresses() {
    std::vector<sockaddr_in> result;

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

    if (ret != NO_ERROR) {
        DbgLog("GetSubnetBroadcastAddresses: GetAdaptersAddresses fallo err=%lu", ret);
        return result;
    }

    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;

            sockaddr_in* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            ULONG prefixLen = ua->OnLinkPrefixLength;
            if (prefixLen == 0 || prefixLen > 32) continue;

            uint32_t ip = sa->sin_addr.s_addr; // network byte order
            uint32_t mask = (prefixLen == 32) ? 0xFFFFFFFFu : htonl(~0u << (32 - prefixLen));
            uint32_t bcast = (ip & mask) | ~mask;

            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_port = htons(ResolveAnnouncePort());
            dest.sin_addr.s_addr = bcast;

            char ipStr[32] = {}, bStr[32] = {};
            InetNtopA(AF_INET, &sa->sin_addr, ipStr, sizeof(ipStr));
            InetNtopA(AF_INET, &dest.sin_addr, bStr, sizeof(bStr));
            DbgLog("GetSubnetBroadcastAddresses: interfaz %s/%lu -> broadcast %s",
                ipStr, prefixLen, bStr);

            result.push_back(dest);
        }
    }

    return result;
}


class CServerDriver : public IServerTrackedDeviceProvider {
    SOCKET m_ds = INVALID_SOCKET, m_as = INVALID_SOCKET;
    HANDLE m_annThread = nullptr; SOCKET m_annSock = INVALID_SOCKET; volatile bool m_annRunning = false;


    static DWORD WINAPI AnnounceProc(LPVOID p) {
        CServerDriver* self = static_cast<CServerDriver*>(p);
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); if (s == INVALID_SOCKET)return 0;
        self->m_annSock = s;
        BOOL bcast = TRUE; setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char*)&bcast, sizeof(bcast));
        char host[64] = "SteamVR-PC"; gethostname(host, sizeof(host) - 1);
        char msg[80]; int ml = _snprintf_s(msg, sizeof(msg), _TRUNCATE, "CVRANNOUNCE:%s", host);

        int cycle = 0;
        std::vector<sockaddr_in> targets;

        while (self->m_annRunning) {

            if ((cycle++ % 5) == 0) {
                targets = GetSubnetBroadcastAddresses();
                if (targets.empty()) {
                    DbgLog("AnnounceProc: no se detecto ninguna interfaz IPv4 activa, "
                        "usando INADDR_BROADCAST como fallback");
                    sockaddr_in dest{};
                    dest.sin_family = AF_INET;
                    dest.sin_port = htons(ResolveAnnouncePort());
                    dest.sin_addr.s_addr = INADDR_BROADCAST;
                    targets.push_back(dest);
                }
            }
            if (ml > 0) {
                for (auto& dest : targets) {
                    sendto(s, msg, ml, 0, (sockaddr*)&dest, sizeof(dest));
                }
            }
            for (int i = 0; i < 20 && self->m_annRunning; ++i)Sleep(100);
        }
        closesocket(s); self->m_annSock = INVALID_SOCKET; return 0;
    }
    void StartAnnounce() { m_annRunning = true; m_annThread = CreateThread(nullptr, 0, AnnounceProc, this, 0, nullptr); }
    void StopAnnounce() {
        m_annRunning = false; SOCKET s = m_annSock; if (s != INVALID_SOCKET)closesocket(s);
        if (m_annThread) { WaitForSingleObject(m_annThread, 3000); CloseHandle(m_annThread); m_annThread = nullptr; }
    }

    SOCKET MakeUDP(uint16_t port) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); if (s == INVALID_SOCKET)return s;
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = INADDR_ANY;
        bind(s, (sockaddr*)&a, sizeof(a)); u_long nb = 1; ioctlsocket(s, FIONBIO, &nb); return s;
    }

    void ProcessAuth() {
        char buf[64]; sockaddr_in from{}; int fl = sizeof(from); int n;
        uint8_t secret[16]; LoadSecret(secret);
        while ((n = recvfrom(m_as, buf, sizeof(buf), 0, (sockaddr*)&from, &fl)) > 0) {

            if (n == 8 && memcmp(buf, "CVRPING?", 8) == 0) {
                sendto(m_as, "CVRPONG!", 8, 0, (sockaddr*)&from, fl);
                continue;
            }

            ClientSession& sess = IsLoopbackAddr(from) ? g_kbdSession : g_phoneSession;
            if (n == 8 && memcmp(buf, "CVRHELLO", 8) == 0) {
                uint32_t r1 = CryptoRand32(), r2 = CryptoRand32();
                memcpy(sess.challenge, &r1, 4); memcpy(sess.challenge + 4, &r2, 4); memcpy(&sess.addr, &from, sizeof(from));
                char resp[12] = {}; memcpy(resp, "CHLG", 4); memcpy(resp + 4, sess.challenge, 8);
                sendto(m_as, resp, 12, 0, (sockaddr*)&from, sizeof(from));
            }
            else if (n == 12 && memcmp(buf, "RESP", 4) == 0) {
                uint8_t got[8]; memcpy(got, buf + 4, 8); uint8_t exp[8];
                if (ComputeHMAC(secret, 16, sess.challenge, 8, exp) && memcmp(got, exp, 8) == 0) {
                    sess.token = CryptoRand32(); sess.active = true; sess.lastDataMs = GetTickCount64();
                    memcpy(&sess.addr, &from, sizeof(from));
                    char tok[8] = {}; memcpy(tok, "TOKN", 4); memcpy(tok + 4, &sess.token, 4);
                    sendto(m_as, tok, 8, 0, (sockaddr*)&from, sizeof(from));
                }
                else { sess.active = false; sess.token = 0; }
            }
        }
    }

    void ProcessData() {
        char buf[PACKET_BYTES + 16]; sockaddr_in from{}; int fl = sizeof(from); int n;
        while ((n = recvfrom(m_ds, buf, sizeof(buf), 0, (sockaddr*)&from, &fl)) > 0) {
            if (n != PACKET_BYTES)continue;
            bool loop = IsLoopbackAddr(from);
            ClientSession& sess = loop ? g_kbdSession : g_phoneSession;
            if (!sess.active)continue;
            if (from.sin_addr.s_addr != sess.addr.sin_addr.s_addr)continue;
            uint32_t rt; memcpy(&rt, buf, 4); if (rt != sess.token)continue;
            sess.lastDataMs = GetTickCount64();
            memcpy(loop ? &g_kbdPkt : &g_phonePkt, buf, PACKET_BYTES);
        }
    }


    void MergeSessions() {
        ULONGLONG now = GetTickCount64();
        bool kbdAlive = g_kbdSession.active && (now - g_kbdSession.lastDataMs) < SESSION_TIMEOUT_MS;
        bool phoneWifiAlive = g_phoneSession.active && (now - g_phoneSession.lastDataMs) < SESSION_TIMEOUT_MS;
        bool usbAlive = g_usbActive.load() && (now - g_usbLastDataMs.load()) < SESSION_TIMEOUT_MS;

        // El telefono llega por WiFi O por cable/ADB. Si por algun
        // motivo llegaran ambos, se prioriza USB (mas estable).
        bool phoneAlive = phoneWifiAlive || usbAlive;

        // Copia local bajo lock: evita leer g_usbPkt a medio escribir
        // mientras OnUsbTrackingPacket() (otro hilo) lo esta actualizando.
        TrackingPacket usbPktSnapshot;
        if (usbAlive) {
            std::lock_guard<std::mutex> lk(g_usbPktMtx);
            usbPktSnapshot = g_usbPkt;
        }
        const TrackingPacket& phonePktRef = usbAlive ? usbPktSnapshot : g_phonePkt;

        TrackingPacket merged = {};
        if (kbdAlive) merged = g_kbdPkt;
        else if (phoneAlive) { merged.left = phonePktRef.left; merged.right = phonePktRef.right; }
        if (phoneAlive) {
            merged.hmd.x = phonePktRef.hmd.x;
            merged.hmd.y = phonePktRef.hmd.y;
            merged.hmd.z = phonePktRef.hmd.z;
            merged.hmd.qx = phonePktRef.hmd.qx; merged.hmd.qy = phonePktRef.hmd.qy;
            merged.hmd.qz = phonePktRef.hmd.qz; merged.hmd.qw = phonePktRef.hmd.qw;

            auto cH = [&](HandData& d, const HandData& s) {
                d.x = s.x; d.y = s.y; d.z = s.z;
                if (!kbdAlive) {
                    d.qx = s.qx; d.qy = s.qy; d.qz = s.qz; d.qw = s.qw;
                }
                d.isTracked = 1.f;
                };
            if (phonePktRef.left.isTracked > .5f)  cH(merged.left, phonePktRef.left);
            if (phonePktRef.right.isTracked > .5f) cH(merged.right, phonePktRef.right);

            if (!kbdAlive) {
                auto cB = [](HandData& d, const HandData& s) {
                    d.trigger = s.trigger; d.grip = s.grip; d.joyX = s.joyX; d.joyY = s.joyY;
                    d.sysBtn = s.sysBtn; d.appBtn = s.appBtn; d.clickBtn = s.clickBtn;
                    d.curlThumb = s.curlThumb; d.curlIndex = s.curlIndex; d.curlMiddle = s.curlMiddle;
                    d.curlRing = s.curlRing; d.curlPinky = s.curlPinky;
                    };
                cB(merged.left, phonePktRef.left); cB(merged.right, phonePktRef.right);
            }
        }
        merged.hmd.isTracked = (kbdAlive || phoneAlive) ? 1.f : 0.f;
        g_tgt = merged;
    }

public:
    EVRInitError Init(IVRDriverContext* ctx) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
        g_kbdSession = {}; g_phoneSession = {}; g_kbdPkt = {}; g_phonePkt = {};
        if (ResolveBaseCode() == 0u)return VRInitError_Driver_NotLoaded;
        VRServerDriverHost()->TrackedDeviceAdded("CamHMD", TrackedDeviceClass_HMD, &g_hmd);
        VRServerDriverHost()->TrackedDeviceAdded("CamCtrl_L", TrackedDeviceClass_Controller, &g_left);
        VRServerDriverHost()->TrackedDeviceAdded("CamCtrl_R", TrackedDeviceClass_Controller, &g_right);
        WSADATA wsa;
        int wsaErr = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (wsaErr != 0) { DbgLog("WSAStartup FAILED err=%d", wsaErr); return VRInitError_Driver_NotLoaded; }
        m_ds = MakeUDP(ResolveDataPort());
        m_as = MakeUDP(ResolveAuthPort());
        StartAnnounce();
        StartHotkeyListener();
        StartKeyboardHotkey();
        g_streamer.Start();
        g_audioStreamer.Start();
        g_lowResPreview.Start();

        // ── Modo 100% cable/ADB: no requiere WiFi ni tethering/hotspot ──
        {
            UsbTrackingCallbacks usbCb;
            usbCb.onPacket = &OnUsbTrackingPacket;
            usbCb.onQuality = &OnUsbTrackingQuality;
            if (!g_usbTracking.Start(usbCb)) {
                DbgLog("UsbTrackingServer: no se pudo iniciar (puerto %u) - "
                    "el modo cable/ADB no estara disponible, solo WiFi/RNDIS",
                    UsbTrackingServer::PORT);
            }
            // Mantiene vivos los tuneles 'adb reverse' para tracking
            // (47299) Y para video/audio (47295/47298) mientras el
            // telefono este enchufado por cable.
            UsbTrackingServer::EnsureAdbReverseTunnelsAsync(
                { ResolveStreamPort(), VrAudioStreamer::PORT });
        }
        return VRInitError_None;
    }

    void RunFrame() override {
        ProcessAuth(); ProcessData(); MergeSessions();

        // ── Captura del anchor de 6DoF para el flip (ver comentario
        //    junto a g_flipAnchorX/Y/Z mas arriba). Se hace ANTES del
        //    suavizado de este frame, comparando el estado actual del
        //    flip contra el del frame anterior: solo se recaptura el
        //    punto de referencia en la transicion desactivado->activado,
        //    no en cada frame mientras esta activo. ──
        bool flippedNow = g_hmdFlipped.load();
        if (flippedNow && !g_wasFlipped) {
            g_flipAnchorX = g_cur.hmd.x;
            g_flipAnchorY = g_cur.hmd.y;
            g_flipAnchorZ = g_cur.hmd.z;
            DbgLog("Flip ON: anchor de 6DoF capturado en (%.3f, %.3f, %.3f)",
                g_flipAnchorX, g_flipAnchorY, g_flipAnchorZ);
        }
        g_wasFlipped = flippedNow;

        const float L = 0.85f;
        auto lp = [&](float& c, float t) {c += (t - c) * L; };
        lp(g_cur.left.x, g_tgt.left.x); lp(g_cur.left.y, g_tgt.left.y); lp(g_cur.left.z, g_tgt.left.z);
        lp(g_cur.right.x, g_tgt.right.x); lp(g_cur.right.y, g_tgt.right.y); lp(g_cur.right.z, g_tgt.right.z);
        lp(g_cur.hmd.x, g_tgt.hmd.x); lp(g_cur.hmd.y, g_tgt.hmd.y); lp(g_cur.hmd.z, g_tgt.hmd.z);
        // ── NUEVO: suavizado del curl por dedo (mismo filtro exponencial
        //    que ya se usaba para posicion, asi los dedos no "saltan"). ──
        lp(g_cur.left.curlThumb, g_tgt.left.curlThumb);
        lp(g_cur.left.curlIndex, g_tgt.left.curlIndex);
        lp(g_cur.left.curlMiddle, g_tgt.left.curlMiddle);
        lp(g_cur.left.curlRing, g_tgt.left.curlRing);
        lp(g_cur.left.curlPinky, g_tgt.left.curlPinky);
        lp(g_cur.right.curlThumb, g_tgt.right.curlThumb);
        lp(g_cur.right.curlIndex, g_tgt.right.curlIndex);
        lp(g_cur.right.curlMiddle, g_tgt.right.curlMiddle);
        lp(g_cur.right.curlRing, g_tgt.right.curlRing);
        lp(g_cur.right.curlPinky, g_tgt.right.curlPinky);
        auto cp = [](HandData& c, const HandData& t) {
            c.qx = t.qx; c.qy = t.qy; c.qz = t.qz; c.qw = t.qw; c.trigger = t.trigger; c.grip = t.grip;
            c.joyX = t.joyX; c.joyY = t.joyY; c.sysBtn = t.sysBtn; c.appBtn = t.appBtn; c.clickBtn = t.clickBtn; c.isTracked = t.isTracked; };
        cp(g_cur.left, g_tgt.left); cp(g_cur.right, g_tgt.right);
        g_cur.hmd.qx = g_tgt.hmd.qx; g_cur.hmd.qy = g_tgt.hmd.qy;
        g_cur.hmd.qz = g_tgt.hmd.qz; g_cur.hmd.qw = g_tgt.hmd.qw;
        g_cur.hmd.isTracked = g_tgt.hmd.isTracked;
        if (g_hmd.m_id != k_unTrackedDeviceIndexInvalid)
            VRServerDriverHost()->TrackedDevicePoseUpdated(g_hmd.m_id, g_hmd.GetPose(), sizeof(DriverPose_t));
        if (g_left.m_id != k_unTrackedDeviceIndexInvalid) {
            VRServerDriverHost()->TrackedDevicePoseUpdated(g_left.m_id, g_left.GetPose(), sizeof(DriverPose_t));
            g_left.UpdateInputs();
        }
        if (g_right.m_id != k_unTrackedDeviceIndexInvalid) {
            VRServerDriverHost()->TrackedDevicePoseUpdated(g_right.m_id, g_right.GetPose(), sizeof(DriverPose_t));
            g_right.UpdateInputs();
        }
    }

    void Cleanup() override {
        g_streamer.Stop(); StopAnnounce();
        StopHotkeyListener();
        StopKeyboardHotkey();
        g_audioStreamer.Stop();
        g_lowResPreview.Stop();
        g_usbTracking.Stop();
        if (m_ds != INVALID_SOCKET) { closesocket(m_ds); m_ds = INVALID_SOCKET; }
        if (m_as != INVALID_SOCKET) { closesocket(m_as); m_as = INVALID_SOCKET; }
        WSACleanup();
    }
    const char* const* GetInterfaceVersions() override { return k_InterfaceVersions; }
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {} void LeaveStandby() override {}
};

static CServerDriver g_server;

extern "C" __declspec(dllexport)
void* HmdDriverFactory(const char* iface, int* ret) {
    if (strcmp(IServerTrackedDeviceProvider_Version, iface) == 0)return &g_server;
    if (ret)*ret = VRInitError_Init_InterfaceNotFound;
    return nullptr;
}