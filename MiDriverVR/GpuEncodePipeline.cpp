#ifdef _WIN32

#include "GpuEncodePipeline.h"
#include <mfreadwrite.h>
#include <cstdio>
#include <cstdarg>
#include <windows.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static void GpuLog(const char* fmt, ...) {
    char buf[320]; va_list va; va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va); va_end(va);
    OutputDebugStringA("[CamVR][GPU] "); OutputDebugStringA(buf); OutputDebugStringA("\n");
}

std::vector<GpuAdapterInfo> GpuCapabilities::EnumerateAllAdapters() {
    std::vector<GpuAdapterInfo> result;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;

        GpuAdapterInfo info;
        info.description = desc.Description;
        info.vendorId = desc.VendorId;
        info.deviceId = desc.DeviceId;
        info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
        info.isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        info.adapter = adapter;
        result.push_back(std::move(info));
    }
    return result;
}

bool GpuCapabilities::DetectBestDedicatedAdapter(GpuAdapterInfo& out) {
    auto all = EnumerateAllAdapters();

    GpuLog("Adaptadores DXGI encontrados: %zu", all.size());
    for (auto& a : all) {
        char narrow[256]; wcstombs_s(nullptr, narrow, sizeof(narrow), a.description.c_str(), _TRUNCATE);
        GpuLog("  - \"%s\" vendor=0x%04X dedVRAM=%.0fMB software=%d",
            narrow, a.vendorId, a.dedicatedVideoMemory / (1024.0 * 1024.0), (int)a.isSoftware);
    }

    const GpuAdapterInfo* best = nullptr;
    for (auto& a : all) {
        if (a.isSoftware) continue;
        if (a.vendorId == VENDOR_ID_MS_WARP) continue;
        if (a.dedicatedVideoMemory < MIN_DEDICATED_VRAM_BYTES) continue;

        if (!best || a.dedicatedVideoMemory > best->dedicatedVideoMemory) {
            best = &a;
        }
    }

    if (!best) {
        GpuLog("No se encontro GPU dedicada real (>= %.0fMB VRAM, no WARP). Se usara perfil SOFTWARE.",
            MIN_DEDICATED_VRAM_BYTES / (1024.0 * 1024.0));
        return false;
    }

    out = *best;
    char narrow[256]; wcstombs_s(nullptr, narrow, sizeof(narrow), out.description.c_str(), _TRUNCATE);
    GpuLog("GPU dedicada elegida: \"%s\" (%.0fMB VRAM dedicada)",
        narrow, out.dedicatedVideoMemory / (1024.0 * 1024.0));
    return true;
}

bool MftDriver::Attach(IMFTransform* enc) {
    m_enc = enc;
    m_isAsync = false;
    m_events.Reset();
    m_pendingNeedInput = 0;
    m_pendingHaveOutput = 0;

    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(enc->GetAttributes(&attrs))) {
        UINT32 isAsync = 0;
        attrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
        if (isAsync) {
            HRESULT hr = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            if (FAILED(hr)) return false;
            hr = enc->QueryInterface(IID_PPV_ARGS(&m_events));
            if (FAILED(hr)) return false;
            m_isAsync = true;
        }
    }
    return true;
}

void MftDriver::Detach() {
    m_enc = nullptr;
    m_events.Reset();
    m_isAsync = false;
}

void MftDriver::DrainEventsNonBlocking() {
    if (!m_isAsync || !m_events) return;
    for (;;) {
        ComPtr<IMFMediaEvent> ev;
        HRESULT hr = m_events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
        if (hr == E_UNEXPECTED || FAILED(hr)) break;
        MediaEventType met = MEUnknown;
        ev->GetType(&met);
        if (met == METransformNeedInput) m_pendingNeedInput++;
        else if (met == METransformHaveOutput) m_pendingHaveOutput++;
    }
}

bool MftDriver::ReadyForInput() {
    if (!m_isAsync) return true;
    DrainEventsNonBlocking();
    return m_pendingNeedInput > 0;
}

HRESULT MftDriver::SubmitInput(IMFSample* sample) {
    if (!m_enc) return E_POINTER;
    HRESULT hr = m_enc->ProcessInput(0, sample, 0);
    if (m_isAsync && SUCCEEDED(hr)) {
        if (m_pendingNeedInput > 0) m_pendingNeedInput--;
    }
    return hr;
}

HRESULT MftDriver::TryGetOutput(DWORD outStreamId, ComPtr<IMFSample>& outSample) {
    if (!m_enc) return E_POINTER;
    if (m_isAsync) {
        DrainEventsNonBlocking();
        if (m_pendingHaveOutput <= 0) return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }

    MFT_OUTPUT_STREAM_INFO si = {};
    m_enc->GetOutputStreamInfo(outStreamId, &si);
    MFT_OUTPUT_DATA_BUFFER ob = {};
    bool needAlloc = !(si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));
    ComPtr<IMFSample> outSmp;
    if (needAlloc) {
        ComPtr<IMFMediaBuffer> outBuf;
        MFCreateMemoryBuffer(si.cbSize > 0 ? si.cbSize : 512 * 1024, &outBuf);
        MFCreateSample(&outSmp);
        outSmp->AddBuffer(outBuf.Get());
        ob.pSample = outSmp.Get();
    }

    DWORD status = 0;
    HRESULT hr = m_enc->ProcessOutput(0, 1, &ob, &status);
    if (m_isAsync && hr != MF_E_TRANSFORM_NEED_MORE_INPUT) {
        if (m_pendingHaveOutput > 0) m_pendingHaveOutput--;
    }

    if (FAILED(hr)) {
        if (ob.pSample && ob.pSample != outSmp.Get()) ob.pSample->Release();
        return hr;
    }
    if (!ob.pSample) return MF_E_TRANSFORM_NEED_MORE_INPUT;

    if (needAlloc) outSample = outSmp;
    else outSample.Attach(ob.pSample);
    return S_OK;
}

bool GpuEncodePipeline::DetectAndCreateDevice() {
    bool hasDedicated = GpuCapabilities::DetectBestDedicatedAdapter(m_adapterInfo);

    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL flOut{};
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    HRESULT hr;
    if (hasDedicated) {
        hr = D3D11CreateDevice(m_adapterInfo.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            flags, fl, ARRAYSIZE(fl), D3D11_SDK_VERSION, &m_device, &flOut, &m_context);
    } else {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            flags, fl, ARRAYSIZE(fl), D3D11_SDK_VERSION, &m_device, &flOut, &m_context);
    }
    if (FAILED(hr)) return false;

    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m_device.As(&mt))) mt->SetMultithreadProtected(TRUE);

    hr = MFCreateDXGIDeviceManager(&m_dxgiResetToken, &m_dxgiMgr);
    if (FAILED(hr)) return false;
    hr = m_dxgiMgr->ResetDevice(m_device.Get(), m_dxgiResetToken);
    if (FAILED(hr)) return false;

    if (hasDedicated) {
        m_profile = { 1920, 1080, 60, 12000000, true };
    } else {
        m_profile = { 1280, 720, 30, 6000000, false };
    }
    return true;
}

bool GpuEncodePipeline::CreateTexturePool(uint32_t poolSize) {
    if (!m_device) return false;
    poolSize = (poolSize < 2) ? 2 : (poolSize > MAX_TEX_POOL ? MAX_TEX_POOL : poolSize);
    m_poolSize = poolSize;
    m_writeCursor = 0;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = m_profile.width;
    td.Height = m_profile.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;

    for (uint32_t i = 0; i < poolSize; ++i) {
        HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &m_texPool[i]);
        if (FAILED(hr)) return false;
        m_inFlight[i] = false;
    }
    return true;
}

ID3D11Texture2D* GpuEncodePipeline::AcquireWriteTexture(uint32_t& outIndex) {
    if (m_poolSize == 0) { outIndex = 0; return nullptr; }
    uint32_t idx = m_writeCursor;
    m_writeCursor = (m_writeCursor + 1) % m_poolSize;
    outIndex = idx;
    return m_texPool[idx].Get();
}

ComPtr<IMFSample> GpuEncodePipeline::WrapTextureAsSample(uint32_t index, LONGLONG pts, LONGLONG dur) {
    ComPtr<IMFSample> sample;
    if (index >= m_poolSize || !m_texPool[index]) return sample;

    ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), m_texPool[index].Get(), 0, FALSE, &buf);
    if (FAILED(hr)) return sample;
    DWORD len = m_profile.width * m_profile.height * 4;
    buf->SetCurrentLength(len);

    MFCreateSample(&sample);
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(pts);
    sample->SetSampleDuration(dur);
    return sample;
}

void GpuEncodePipeline::ApplyLowLatencyCodecSettings(IMFTransform* enc, uint32_t bitrate, uint32_t gop) {
    ComPtr<ICodecAPI> codec;
    if (FAILED(enc->QueryInterface(IID_PPV_ARGS(&codec)))) return;
    VARIANT v;

    VariantInit(&v); v.vt = VT_UI4;
    v.ulVal = eAVEncCommonRateControlMode_UnconstrainedVBR;
    if (FAILED(codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v))) {
        VariantInit(&v); v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_Quality;
        if (FAILED(codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v))) {
            VariantInit(&v); v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_CBR;
            codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        }
    }

    VariantInit(&v); v.vt = VT_UI4; v.ulVal = (uint32_t)(bitrate * 1.5);
    codec->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &v);

    VariantInit(&v); v.vt = VT_UI4; v.ulVal = bitrate;
    codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);

    VariantInit(&v); v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
    codec->SetValue(&CODECAPI_AVEncCommonLowLatency, &v);
    codec->SetValue(&CODECAPI_AVEncCommonRealTime, &v);

    VariantInit(&v); v.vt = VT_UI4; v.ulVal = 0;
    codec->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);

    VariantInit(&v); v.vt = VT_UI4; v.ulVal = gop;
    codec->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);

    VariantInit(&v); v.vt = VT_UI4; v.ulVal = 2;
    codec->SetValue(&CODECAPI_AVEncVideoMaxNumRefFrame, &v);

    VariantInit(&v); v.vt = VT_UI4; v.ulVal = 0;
    codec->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &v);

    VariantInit(&v); v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
    codec->SetValue(&CODECAPI_AVEncH264CABACEnable, &v);
}

bool GpuEncodePipeline::SmokeTestEncoder(IMFTransform* enc, MftDriver& driver, DWORD inSt, DWORD outSt) {
    const uint32_t W = m_profile.width, H = m_profile.height;
    std::vector<uint8_t> black(W * H * 4, 0);

    uint32_t idx; ID3D11Texture2D* tex = AcquireWriteTexture(idx);
    if (!tex) return false;

    m_context->UpdateSubresource(tex, 0, nullptr, black.data(), W * 4, 0);
    LONGLONG dur = 10000000LL / (m_profile.fps > 0 ? m_profile.fps : 30);
    bool gotOutput = false;

    for (int frame = 0; frame < 3 && !gotOutput; ++frame) {
        auto sample = WrapTextureAsSample(idx, (LONGLONG)frame * dur, dur);
        if (!sample) return false;

        ULONGLONG waitStart = GetTickCount64();
        while (driver.IsAsync() && !driver.ReadyForInput()) {
            if (GetTickCount64() - waitStart > 200) break;
            Sleep(2);
        }

        HRESULT hr = driver.SubmitInput(sample.Get());
        if (FAILED(hr) && hr != MF_E_NOTACCEPTING) return false;

        ULONGLONG outWaitStart = GetTickCount64();
        while (GetTickCount64() - outWaitStart < 300) {
            ComPtr<IMFSample> outSmp;
            HRESULT ohr = driver.TryGetOutput(outSt, outSmp);
            if (ohr == S_OK && outSmp) { gotOutput = true; break; }
            if (ohr != MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            Sleep(2);
        }
    }
    return gotOutput;
}

bool GpuEncodePipeline::TryCandidate(IMFActivate* activate, ComPtr<IMFTransform>& outEnc,
    MftDriver& outDriver, DWORD& inSt, DWORD& outSt) {
    ComPtr<IMFTransform> enc;
    HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(&enc));
    if (FAILED(hr)) return false;

    hr = enc->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)m_dxgiMgr.Get());
    if (FAILED(hr)) return false;

    MftDriver driver;
    if (!driver.Attach(enc.Get())) return false;

    IMFAttributes* attrObj = nullptr;
    if (SUCCEEDED(enc->GetAttributes(&attrObj)) && attrObj) { attrObj->SetUINT32(MF_LOW_LATENCY, TRUE); attrObj->Release(); }

    ComPtr<IMFMediaType> omt; MFCreateMediaType(&omt);
    omt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    omt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(omt.Get(), MF_MT_FRAME_SIZE, m_profile.width, m_profile.height);
    MFSetAttributeRatio(omt.Get(), MF_MT_FRAME_RATE, m_profile.fps, 1);
    omt->SetUINT32(MF_MT_AVG_BITRATE, m_profile.bitrate);
    omt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    omt->SetUINT32(MF_MT_MPEG2_PROFILE, 100);
    hr = enc->SetOutputType(0, omt.Get(), 0);
    if (FAILED(hr)) return false;

    ComPtr<IMFMediaType> imt; MFCreateMediaType(&imt);
    imt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    imt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(imt.Get(), MF_MT_FRAME_SIZE, m_profile.width, m_profile.height);
    MFSetAttributeRatio(imt.Get(), MF_MT_FRAME_RATE, m_profile.fps, 1);
    imt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = enc->SetInputType(0, imt.Get(), 0);
    if (FAILED(hr)) return false;

    ApplyLowLatencyCodecSettings(enc.Get(), m_profile.bitrate, m_profile.fps);

    enc->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    if (!SmokeTestEncoder(enc.Get(), driver, 0, 0)) {
        enc->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        return false;
    }

    enc->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    outEnc = enc;
    outDriver = std::move(driver);
    inSt = 0; outSt = 0;
    return true;
}

bool GpuEncodePipeline::SelectAndInitHardwareEncoder(ComPtr<IMFTransform>& outEnc,
    MftDriver& outDriver, DWORD& inSt, DWORD& outSt) {
    if (!m_dxgiMgr) return false;

    MFT_REGISTER_TYPE_INFO ti = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO to = { MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** acts = nullptr; UINT32 cnt = 0;
    MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &ti, &to, &acts, &cnt);

    bool found = false;
    for (UINT32 i = 0; i < cnt && !found; ++i) {
        if (TryCandidate(acts[i], outEnc, outDriver, inSt, outSt)) {
            found = true;
        }
    }

    for (UINT32 i = 0; i < cnt; i++) acts[i]->Release();
    if (acts) CoTaskMemFree(acts);

    return found;
}

void GpuEncodePipeline::Shutdown() {
    for (uint32_t i = 0; i < MAX_TEX_POOL; ++i) { m_texPool[i].Reset(); m_inFlight[i] = false; }
    m_poolSize = 0;
    m_dxgiMgr.Reset();
    m_context.Reset();
    m_device.Reset();
}

#endif
