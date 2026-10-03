#pragma once

#ifdef _WIN32

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <icodecapi.h>
#include <codecapi.h>
#include <wrl/client.h>

#include <vector>
#include <string>
#include <atomic>
#include <cstdint>

using Microsoft::WRL::ComPtr;

struct EncodeProfile {
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t fps = 30;
    uint32_t bitrate = 6000000;
    bool     dedicatedGpu = false;
};

struct GpuAdapterInfo {
    std::wstring        description;
    UINT                vendorId = 0;
    UINT                deviceId = 0;
    SIZE_T              dedicatedVideoMemory = 0;
    bool                isSoftware = false;
    ComPtr<IDXGIAdapter1> adapter;
};

class GpuCapabilities {
public:
    static constexpr UINT VENDOR_ID_MS_WARP = 0x1414;
    static constexpr SIZE_T MIN_DEDICATED_VRAM_BYTES = 512ull * 1024ull * 1024ull;

    static std::vector<GpuAdapterInfo> EnumerateAllAdapters();
    static bool DetectBestDedicatedAdapter(GpuAdapterInfo& out);
};

class MftDriver {
public:
    MftDriver() = default;
    ~MftDriver() = default;

    MftDriver(const MftDriver&) = delete;
    MftDriver& operator=(const MftDriver&) = delete;

    MftDriver(MftDriver&&) = default;
    MftDriver& operator=(MftDriver&&) = default;

    bool Attach(IMFTransform* enc);
    void Detach();

    bool IsAsync() const { return m_isAsync; }
    void DrainEventsNonBlocking();
    bool ReadyForInput();

    HRESULT SubmitInput(IMFSample* sample);
    HRESULT TryGetOutput(DWORD outStreamId, ComPtr<IMFSample>& outSample);

private:
    IMFTransform* m_enc = nullptr;
    bool m_isAsync = false;
    ComPtr<IMFMediaEventGenerator> m_events;
    int m_pendingNeedInput = 0;
    int m_pendingHaveOutput = 0;
};

class GpuEncodePipeline {
public:
    static constexpr uint32_t MAX_TEX_POOL = 4;

    GpuEncodePipeline() = default;
    ~GpuEncodePipeline() { Shutdown(); }

    GpuEncodePipeline(const GpuEncodePipeline&) = delete;
    GpuEncodePipeline& operator=(const GpuEncodePipeline&) = delete;

    bool DetectAndCreateDevice();

    bool HasDedicatedGpu() const { return m_profile.dedicatedGpu; }
    const EncodeProfile& Profile() const { return m_profile; }

    void OverrideProfile(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate) {
        m_profile.width = w;
        m_profile.height = h;
        m_profile.fps = fps;
        m_profile.bitrate = bitrate;
    }

    bool CreateTexturePool(uint32_t poolSize);
    ID3D11Texture2D* AcquireWriteTexture(uint32_t& outIndex);
    ComPtr<IMFSample> WrapTextureAsSample(uint32_t index, LONGLONG pts, LONGLONG dur);

    static void ApplyLowLatencyCodecSettings(IMFTransform* enc, uint32_t bitrate, uint32_t gop);
    bool SmokeTestEncoder(IMFTransform* enc, MftDriver& driver, DWORD inSt, DWORD outSt);
    bool TryCandidate(IMFActivate* activate, ComPtr<IMFTransform>& outEnc,
        MftDriver& outDriver, DWORD& inSt, DWORD& outSt);
    bool SelectAndInitHardwareEncoder(ComPtr<IMFTransform>& outEnc,
        MftDriver& outDriver, DWORD& inSt, DWORD& outSt);

    void Shutdown();

private:
    GpuAdapterInfo m_adapterInfo;
    ComPtr<ID3D11Device>        m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IMFDXGIDeviceManager> m_dxgiMgr;
    UINT                          m_dxgiResetToken = 0;
    EncodeProfile m_profile;

    ComPtr<ID3D11Texture2D> m_texPool[MAX_TEX_POOL];
    std::atomic<bool>       m_inFlight[MAX_TEX_POOL] = {};
    uint32_t                m_poolSize = 0;
    uint32_t                m_writeCursor = 0;
};

#endif
