#include <openvr_driver.h>
#include "Platform.h"
#include "HandSkeleton.h"
#include "Vrlowrespreview.h"
#include "Usbtrackingserver.h"

#ifdef _WIN32
  #include "GpuEncodePipeline.h"
#else
  #include "LinuxVideoEncoder.h"
  #include "LinuxAudioCapture.h"
#endif

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
        sockaddr_in from{}; socklen_t fl = sizeof(from);
        int n = recvfrom(g_flipSock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue;
        if (n != 4 || memcmp(buf, "FLIP", 4) != 0) continue;
        if ((ntohl(from.sin_addr.s_addr) >> 24) != 127u) continue;

        g_hmdFlipped = !g_hmdFlipped.load();
        DbgLog("FLIP recibido: HMD flip %s", g_hmdFlipped.load() ? "ACTIVADO" : "desactivado");
    }
}

static void StartHotkeyListener() {
    g_flipSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_flipSock == INVALID_SOCKET) { DbgLog("Flip listener: socket() fallo"); return; }
    SetSocketReuseAddr(g_flipSock);

    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_port = htons(ResolveFlipPort()); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_flipSock, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        DbgLog("Flip listener: bind fallo err=%d", WSAGetLastError());
        closesocket(g_flipSock); g_flipSock = INVALID_SOCKET;
        return;
    }
    SetSocketRecvTimeout(g_flipSock, 500);
    g_flipRunning = true;
    g_flipThread = std::thread(FlipListenLoop);
    DbgLog("Flip listener activo puerto UDP:%u", ResolveFlipPort());
}

static void StopHotkeyListener() {
    g_flipRunning = false;
    if (g_flipSock != INVALID_SOCKET) { closesocket(g_flipSock); g_flipSock = INVALID_SOCKET; }
    if (g_flipThread.joinable()) g_flipThread.join();
}

#ifdef _WIN32
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
#else
static void StartKeyboardHotkey() {}
static void StopKeyboardHotkey() {}
#endif

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
    q.x = copysign(q.x, m.m[2][1] - m.m[1][2]); q.y = copysign(q.y, m.m[0][2] - m.m[2][0]);
    q.z = copysign(q.z, m.m[1][0] - m.m[0][1]); return q;
}

static const uint32_t WINDOW_W = 1920, WINDOW_H = 1080;
static const int32_t  WINDOW_X = 0, WINDOW_Y = 0;
class CMyDisplayComponent : public IVRDisplayComponent {
public:
    void GetWindowBounds(int32_t* x, int32_t* y, uint32_t* w, uint32_t* h) override {
        *x = WINDOW_X; *y = WINDOW_Y; *w = WINDOW_W; *h = WINDOW_H;
    }
    bool IsDisplayOnDesktop()   override { return true; }
    bool IsDisplayRealDisplay() override { return false; }
    void GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) override { *w = WINDOW_W; *h = WINDOW_H; }
    void GetEyeOutputViewport(EVREye e, uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h) override {
        *y = 0; *w = WINDOW_W / 2; *h = WINDOW_H; *x = (e == Eye_Left) ? 0 : (WINDOW_W / 2);
    }
    void GetProjectionRaw(EVREye, float* l, float* r, float* t, float* b) override {
        *l = -1.f; *r = 1.f; *t = -1.f; *b = 1.f;
    }
    DistortionCoordinates_t ComputeDistortion(EVREye, float u, float v) override {
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
        }

        pose.vecPosition[0] = px; pose.vecPosition[1] = py; pose.vecPosition[2] = pz;
        pose.qRotation = q;
        return pose;
    }
    void Deactivate() override { m_id = k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {} void DebugRequest(const char*, char*, uint32_t) override {}
    void* GetComponent(const char* i) override {
        if (strcmp(IVRDisplayComponent_Version, i) == 0) return &g_displayComponent; return nullptr;
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
        VRDriverInput()->CreateSkeletonComponent(
            p, compName, skeletonPath, "/pose/raw",
            VRSkeletalTracking_Partial, nullptr, 0, &hSkeleton);

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
        } else {
            pose.vecPosition[0] = h->x + offX; pose.vecPosition[1] = h->y + offY; pose.vecPosition[2] = h->z + offZ;
            pose.qRotation = handQ;
        }
        return pose;
    }
    void UpdateInputs() {
        if (m_id == k_unTrackedDeviceIndexInvalid) return;
        HandData* h = (m_role == TrackedControllerRole_LeftHand) ? &g_cur.left : &g_cur.right;
        VRDriverInput()->UpdateScalarComponent(hTrig, h->trigger, 0.);
        VRDriverInput()->UpdateScalarComponent(hGrip, h->grip, 0.);
        VRDriverInput()->UpdateScalarComponent(hPX, h->joyX, 0.);
        VRDriverInput()->UpdateScalarComponent(hPY, h->joyY, 0.);
        VRDriverInput()->UpdateBooleanComponent(hSys, h->sysBtn > .5f, 0.);
        VRDriverInput()->UpdateBooleanComponent(hApp, h->appBtn > .5f, 0.);
        VRDriverInput()->UpdateBooleanComponent(hPC, h->clickBtn > .5f, 0.);
        VRDriverInput()->UpdateBooleanComponent(hPT, fabsf(h->joyX) > .1f || fabsf(h->joyY) > .1f || h->clickBtn > .5f, 0.);

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

    bool Start() {
        if (m_running.exchange(true)) return true;
        if (!InitNetwork()) { m_running = false; return false; }
        if (!InitQualityListener()) {
            DbgLog("Aviso: no se pudo iniciar listener de calidad (puerto %u)", ResolveQualityPort());
        }
        m_acceptThread = std::thread([this] { AcceptLoop(); });
        m_captureThread = std::thread([this] { CaptureLoop(); });
        DbgLog("Streamer iniciado puerto TCP:%u", ResolveStreamPort());
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
#ifndef _WIN32
    LinuxVideoEncoder m_linuxEncoder;
#endif

    std::mutex        m_pendingQualityMtx;
    bool              m_hasPendingQuality = false;
    int               m_pendingW = 0, m_pendingH = 0;
    uint32_t          m_pendingBitrate = 0;
    uint32_t          m_pendingFps = 0;

    SOCKET      m_qualitySock = INVALID_SOCKET;
    std::thread m_qualityThread;

    static uint16_t ResolveQualityPort() { return 47296; }

    SOCKET     m_listenSock = INVALID_SOCKET;
    SOCKET     m_clientSock = INVALID_SOCKET;
    std::mutex m_sockMtx;
    std::atomic<bool> m_needIDR{ false };

    std::atomic<bool> m_running{ false };
    std::thread m_acceptThread, m_captureThread;

    static void BE32(uint8_t* p, uint32_t v) {
        p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
    }

    bool InitNetwork() {
        m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listenSock == INVALID_SOCKET) return false;
        SetSocketReuseAddr(m_listenSock);
        SetSocketSendBuffer(m_listenSock, 4 * 1024 * 1024);

        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(ResolveStreamPort()); a.sin_addr.s_addr = INADDR_ANY;
        if (bind(m_listenSock, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) return false;
        if (listen(m_listenSock, 1) == SOCKET_ERROR) return false;
        SetSocketRecvTimeout(m_listenSock, 1000);
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
            if (n <= 0) return false;
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
            sockaddr_in cl{}; socklen_t cl_len = sizeof(cl);
            SOCKET s = accept(m_listenSock, (sockaddr*)&cl, &cl_len);
            if (s == INVALID_SOCKET) continue;
            SetSocketNoDelay(s);
            SetSocketSendBuffer(s, 4 * 1024 * 1024);
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
        SetSocketReuseAddr(m_qualitySock);
        sockaddr_in a{}; a.sin_family = AF_INET;
        a.sin_port = htons(ResolveQualityPort()); a.sin_addr.s_addr = INADDR_ANY;
        if (bind(m_qualitySock, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
            closesocket(m_qualitySock); m_qualitySock = INVALID_SOCKET;
            return false;
        }
        SetSocketRecvTimeout(m_qualitySock, 500);
        m_qualityThread = std::thread([this] { QualityListenLoop(); });
        DbgLog("Quality listener activo puerto UDP:%u", ResolveQualityPort());
        return true;
    }

    void QualityListenLoop() {
        uint8_t buf[32];
        while (m_running.load()) {
            sockaddr_in from{}; socklen_t fl = sizeof(from);
            int n = recvfrom(m_qualitySock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (n != 13 || memcmp(buf, "QUAL", 4) != 0) continue;

            uint16_t w = (uint16_t)((buf[4] << 8) | buf[5]);
            uint16_t h = (uint16_t)((buf[6] << 8) | buf[7]);
            uint32_t br = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
                ((uint32_t)buf[10] << 8) | (uint32_t)buf[11];
            uint8_t  fps = buf[12];

            std::lock_guard<std::mutex> lk(m_pendingQualityMtx);
            m_pendingW = w; m_pendingH = h;
            m_pendingBitrate = br; m_pendingFps = fps;
            m_hasPendingQuality = true;
        }
    }

    void CaptureLoop() {
#ifndef _WIN32
        if (!m_linuxEncoder.Init(m_encW, m_encH, m_fps, m_bitrate, m_gopSize)) {
            DbgLog("LinuxVideoEncoder.Init fallo - streaming finalizado");
            return;
        }

        double frameMs = 1000.0 / (m_fps > 0 ? m_fps : 30);
        std::vector<uint8_t> bgraFrame;
        std::vector<std::vector<uint8_t>> nalUnits;

        while (m_running.load()) {
            ULONGLONG t0 = GetTickCount64();
            {
                std::lock_guard<std::mutex> lk(m_sockMtx);
                if (m_clientSock == INVALID_SOCKET) { Sleep(50); continue; }
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
                if (applyNow && newW > 0 && newH > 0 && newFps > 0) {
                    m_encW = newW; m_encH = newH; m_bitrate = newBr; m_fps = newFps; m_gopSize = newFps;
                    m_linuxEncoder.Cleanup();
                    m_linuxEncoder.Init(m_encW, m_encH, m_fps, m_bitrate, m_gopSize);
                    frameMs = 1000.0 / (m_fps > 0 ? m_fps : 30);
                    m_needIDR.store(true);
                }
            }

            m_linuxEncoder.CaptureBGRA(bgraFrame);
            bool forceIDR = m_needIDR.exchange(false);

            if (m_linuxEncoder.Encode(bgraFrame.data(), nalUnits, forceIDR)) {
                bool ok = true;
                for (auto& nal : nalUnits) {
                    if (!SendNal(nal.data(), (int)nal.size())) { ok = false; break; }
                }
                if (!ok) {
                    std::lock_guard<std::mutex> lk(m_sockMtx);
                    if (m_clientSock != INVALID_SOCKET) {
                        closesocket(m_clientSock); m_clientSock = INVALID_SOCKET;
                    }
                }
            }

            ULONGLONG elapsed = GetTickCount64() - t0;
            if (elapsed < (ULONGLONG)frameMs) {
                Sleep((DWORD)(frameMs - elapsed));
            }
        }
        m_linuxEncoder.Cleanup();
#endif
    }
};

static VrH264Streamer g_streamer;
static VrLowResPreview g_lowResPreview;

class VrAudioStreamer {
public:
    static constexpr uint16_t PORT = 47298;

    bool Start() {
        if (m_running.exchange(true)) return true;
        if (!InitNetwork()) { m_running = false; return false; }
        m_acceptThread = std::thread([this] { AcceptLoop(); });
        m_captureThread = std::thread([this] { CaptureLoop(); });
        DbgLog("AudioStreamer iniciado puerto TCP:%u", PORT);
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

#ifndef _WIN32
    LinuxAudioCapture m_linuxAudio;
#endif

    static void BE32(uint8_t* p, uint32_t v) {
        p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
    }

    bool InitNetwork() {
        m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listenSock == INVALID_SOCKET) return false;
        SetSocketReuseAddr(m_listenSock);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = INADDR_ANY;
        if (bind(m_listenSock, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) return false;
        if (listen(m_listenSock, 1) == SOCKET_ERROR) return false;
        SetSocketRecvTimeout(m_listenSock, 1000);
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
            if (n <= 0) return false;
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
            sockaddr_in cl{}; socklen_t cl_len = sizeof(cl);
            SOCKET s = accept(m_listenSock, (sockaddr*)&cl, &cl_len);
            if (s == INVALID_SOCKET) continue;
            SetSocketNoDelay(s);
            {
                std::lock_guard<std::mutex> lk(m_sockMtx);
                if (m_clientSock != INVALID_SOCKET) closesocket(m_clientSock);
                m_clientSock = s;
            }
            DbgLog("Audio: cliente conectado");
        }
    }

    void CaptureLoop() {
#ifndef _WIN32
        m_linuxAudio.Init();
        std::vector<int16_t> pcmBuf;

        while (m_running.load()) {
            {
                std::lock_guard<std::mutex> lk(m_sockMtx);
                if (m_clientSock == INVALID_SOCKET) { Sleep(50); continue; }
            }

            m_linuxAudio.ReadFrames(pcmBuf, 960); // 20ms @ 48kHz
            if (!pcmBuf.empty()) {
                if (!SendFramed(reinterpret_cast<const uint8_t*>(pcmBuf.data()),
                               (int)(pcmBuf.size() * sizeof(int16_t)))) {
                    std::lock_guard<std::mutex> lk(m_sockMtx);
                    if (m_clientSock != INVALID_SOCKET) {
                        closesocket(m_clientSock); m_clientSock = INVALID_SOCKET;
                    }
                }
            }
        }
        m_linuxAudio.Cleanup();
#endif
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

class CServerDriver : public IServerTrackedDeviceProvider {
    SOCKET m_ds = INVALID_SOCKET, m_as = INVALID_SOCKET;
    std::thread m_annThread; SOCKET m_annSock = INVALID_SOCKET; std::atomic<bool> m_annRunning{ false };

    void AnnounceProc() {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); if (s == INVALID_SOCKET) return;
        m_annSock = s;
        int bcast = 1; setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&bcast, sizeof(bcast));
        char host[64] = "SteamVR-PC"; gethostname(host, sizeof(host) - 1);
        char msg[80]; snprintf(msg, sizeof(msg), "CVRANNOUNCE:%s", host);
        int ml = (int)strlen(msg);

        int cycle = 0;
        std::vector<sockaddr_in> targets;

        while (m_annRunning.load()) {
            if ((cycle++ % 5) == 0) {
                targets = GetSubnetBroadcastAddressesPort(ResolveAnnouncePort());
                if (targets.empty()) {
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
            for (int i = 0; i < 20 && m_annRunning.load(); ++i) Sleep(100);
        }
        closesocket(s); m_annSock = INVALID_SOCKET;
    }
    void StartAnnounce() {
        m_annRunning = true;
        m_annThread = std::thread([this] { AnnounceProc(); });
    }
    void StopAnnounce() {
        m_annRunning = false;
        SOCKET s = m_annSock; if (s != INVALID_SOCKET) closesocket(s);
        if (m_annThread.joinable()) m_annThread.join();
    }

    SOCKET MakeUDP(uint16_t port) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); if (s == INVALID_SOCKET) return s;
        SetSocketReuseAddr(s);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = INADDR_ANY;
        bind(s, (sockaddr*)&a, sizeof(a));
#ifdef _WIN32
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
#else
        int flags = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
        return s;
    }

    void ProcessAuth() {
        char buf[64]; sockaddr_in from{}; socklen_t fl = sizeof(from); int n;
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
                } else { sess.active = false; sess.token = 0; }
            }
        }
    }

    void ProcessData() {
        char buf[PACKET_BYTES + 16]; sockaddr_in from{}; socklen_t fl = sizeof(from); int n;
        while ((n = recvfrom(m_ds, buf, sizeof(buf), 0, (sockaddr*)&from, &fl)) > 0) {
            if (n != PACKET_BYTES) continue;
            bool loop = IsLoopbackAddr(from);
            ClientSession& sess = loop ? g_kbdSession : g_phoneSession;
            if (!sess.active) continue;
            if (from.sin_addr.s_addr != sess.addr.sin_addr.s_addr) continue;
            uint32_t rt; memcpy(&rt, buf, 4); if (rt != sess.token) continue;
            sess.lastDataMs = GetTickCount64();
            memcpy(loop ? &g_kbdPkt : &g_phonePkt, buf, PACKET_BYTES);
        }
    }

    void MergeSessions() {
        ULONGLONG now = GetTickCount64();
        bool kbdAlive = g_kbdSession.active && (now - g_kbdSession.lastDataMs) < SESSION_TIMEOUT_MS;
        bool phoneWifiAlive = g_phoneSession.active && (now - g_phoneSession.lastDataMs) < SESSION_TIMEOUT_MS;
        bool usbAlive = g_usbActive.load() && (now - g_usbLastDataMs.load()) < SESSION_TIMEOUT_MS;

        bool phoneAlive = phoneWifiAlive || usbAlive;

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
        if (ResolveBaseCode() == 0u) return VRInitError_Driver_NotLoaded;
        VRServerDriverHost()->TrackedDeviceAdded("CamHMD", TrackedDeviceClass_HMD, &g_hmd);
        VRServerDriverHost()->TrackedDeviceAdded("CamCtrl_L", TrackedDeviceClass_Controller, &g_left);
        VRServerDriverHost()->TrackedDeviceAdded("CamCtrl_R", TrackedDeviceClass_Controller, &g_right);

#ifdef _WIN32
        WSADATA wsa;
        int wsaErr = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (wsaErr != 0) { DbgLog("WSAStartup FAILED err=%d", wsaErr); return VRInitError_Driver_NotLoaded; }
#endif

        m_ds = MakeUDP(ResolveDataPort());
        m_as = MakeUDP(ResolveAuthPort());
        StartAnnounce();
        StartHotkeyListener();
        StartKeyboardHotkey();
        g_streamer.Start();
        g_audioStreamer.Start();
        g_lowResPreview.Start();

        {
            UsbTrackingCallbacks usbCb;
            usbCb.onPacket = &OnUsbTrackingPacket;
            usbCb.onQuality = &OnUsbTrackingQuality;
            if (!g_usbTracking.Start(usbCb)) {
                DbgLog("UsbTrackingServer: no se pudo iniciar (puerto %u)", UsbTrackingServer::PORT);
            }
            UsbTrackingServer::EnsureAdbReverseTunnelsAsync({ ResolveStreamPort(), VrAudioStreamer::PORT });
        }
        return VRInitError_None;
    }

    void RunFrame() override {
        ProcessAuth(); ProcessData(); MergeSessions();

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
        auto lp = [&](float& c, float t) { c += (t - c) * L; };
        lp(g_cur.left.x, g_tgt.left.x); lp(g_cur.left.y, g_tgt.left.y); lp(g_cur.left.z, g_tgt.left.z);
        lp(g_cur.right.x, g_tgt.right.x); lp(g_cur.right.y, g_tgt.right.y); lp(g_cur.right.z, g_tgt.right.z);
        lp(g_cur.hmd.x, g_tgt.hmd.x); lp(g_cur.hmd.y, g_tgt.hmd.y); lp(g_cur.hmd.z, g_tgt.hmd.z);

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
            c.joyX = t.joyX; c.joyY = t.joyY; c.sysBtn = t.sysBtn; c.appBtn = t.appBtn; c.clickBtn = t.clickBtn; c.isTracked = t.isTracked;
        };
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
#ifdef _WIN32
        WSACleanup();
#endif
    }
    const char* const* GetInterfaceVersions() override { return k_InterfaceVersions; }
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {} void LeaveStandby() override {}
};

static CServerDriver g_server;

extern "C" DRIVER_EXPORT
void* HmdDriverFactory(const char* iface, int* ret) {
    if (strcmp(IServerTrackedDeviceProvider_Version, iface) == 0) return &g_server;
    if (ret) *ret = VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
