#pragma once
//
// GpuEncodePipeline.h
//
// Declaraciones para la deteccion de GPU dedicada, el wrapper D3D11 +
// IMFDXGIDeviceManager, el pool de texturas GPU y el driver que
// abstrae el patron sincrono/asincrono de los MFT de video encoder
// (software vs hardware: NVENC/QuickSync/VCE).
//
// Reconstruido a partir del uso real en GpuEncodePipeline.cpp y
// dllmain.cpp (VrH264Streamer). Si tu header original difiere en
// firmas, ajusta aqui para que coincida.
//

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

// ==================================================================
//  Perfil de encoding (resuelto en runtime segun la GPU detectada)
// ==================================================================
struct EncodeProfile {
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t fps = 30;
    uint32_t bitrate = 6000000;
    bool     dedicatedGpu = false;
};

// ==================================================================
//  Info de un adaptador DXGI enumerado
// ==================================================================
struct GpuAdapterInfo {
    std::wstring        description;
    UINT                vendorId = 0;
    UINT                deviceId = 0;
    SIZE_T              dedicatedVideoMemory = 0;
    bool                isSoftware = false;
    ComPtr<IDXGIAdapter1> adapter;
};

// ==================================================================
//  GpuCapabilities — enumeracion y seleccion de la mejor GPU dedicada
// ==================================================================
class GpuCapabilities {
public:
    // Vendor ID que DXGI reporta para el adaptador WARP (software) de Microsoft.
    static constexpr UINT VENDOR_ID_MS_WARP = 0x1414;

    // Umbral minimo de VRAM dedicada para considerar un adaptador como
    // "GPU dedicada real" y no una iGPU/placeholder. 512MB es un
    // margen conservador que descarta iGPUs viejas sin excluir GPUs
    // dedicadas legitimas de gama baja.
    static constexpr SIZE_T MIN_DEDICATED_VRAM_BYTES = 512ull * 1024ull * 1024ull;

    static std::vector<GpuAdapterInfo> EnumerateAllAdapters();

    // Devuelve true y rellena 'out' si encuentra una GPU dedicada real
    // (no WARP, no software, con VRAM >= MIN_DEDICATED_VRAM_BYTES).
    static bool DetectBestDedicatedAdapter(GpuAdapterInfo& out);
};

// ==================================================================
//  MftDriver — abstrae el patron sincrono (SW) vs asincrono (HW real)
//  de IMFTransform, para que el codigo de streaming no tenga que
//  distinguir manualmente entre ambos.
// ==================================================================
class MftDriver {
public:
    MftDriver() = default;
    ~MftDriver() = default;

    // No copiable (posee el estado de eventos de un IMFTransform concreto).
    MftDriver(const MftDriver&) = delete;
    MftDriver& operator=(const MftDriver&) = delete;

    // Movible: se usa "m_mftDriver = std::move(hwDriver);" en dllmain.cpp.
    MftDriver(MftDriver&&) = default;
    MftDriver& operator=(MftDriver&&) = default;

    // Asocia el driver a un IMFTransform ya creado (no toma ownership,
    // el llamador sigue siendo responsable del Release()). Detecta si
    // es asincrono (MF_TRANSFORM_ASYNC) y hace el unlock necesario.
    bool Attach(IMFTransform* enc);
    void Detach();

    bool IsAsync() const { return m_isAsync; }

    // Procesa la cola de eventos del MFT asincrono sin bloquear.
    void DrainEventsNonBlocking();

    // Para MFT asincronos: true solo si hay al menos un
    // METransformNeedInput pendiente sin consumir. Para MFT
    // sincronos: siempre true.
    bool ReadyForInput();

    HRESULT SubmitInput(IMFSample* sample);
    HRESULT TryGetOutput(DWORD outStreamId, ComPtr<IMFSample>& outSample);

private:
    IMFTransform* m_enc = nullptr; // no ownership
    bool m_isAsync = false;
    ComPtr<IMFMediaEventGenerator> m_events;
    int m_pendingNeedInput = 0;
    int m_pendingHaveOutput = 0;
};

// ==================================================================
//  GpuEncodePipeline
// ==================================================================
class GpuEncodePipeline {
public:
    static constexpr uint32_t MAX_TEX_POOL = 4;

    GpuEncodePipeline() = default;
    ~GpuEncodePipeline() { Shutdown(); }

    // No copiable/movible: posee recursos D3D/MF con estado en arrays fijos.
    GpuEncodePipeline(const GpuEncodePipeline&) = delete;
    GpuEncodePipeline& operator=(const GpuEncodePipeline&) = delete;

    // Detecta la mejor GPU dedicada disponible, crea el ID3D11Device
    // correspondiente (o deja que D3D11 elija si no hay dedicada) y
    // el IMFDXGIDeviceManager asociado. Tambien fija m_profile segun
    // el resultado. Devuelve false si ni siquiera se pudo crear un
    // ID3D11Device (en ese caso no hay HW encoder posible).
    bool DetectAndCreateDevice();

    bool HasDedicatedGpu() const { return m_profile.dedicatedGpu; }
    const EncodeProfile& Profile() const { return m_profile; }

    // Permite forzar manualmente resolución/fps/bitrate del perfil de
    // encoding (p.ej. cuando el usuario cambia la calidad desde el
    // teléfono), en vez de usar el valor auto-detectado según la GPU.
    // Necesario porque TryCandidate()/SmokeTestEncoder() negocian los
    // tipos H.264/NV12 usando m_profile, no los miembros de
    // VrH264Streamer — sin esto, el hardware encoder seguiría usando
    // la resolución auto-detectada aunque VrH264Streamer cambiara la suya.
    void OverrideProfile(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate) {
        m_profile.width = w;
        m_profile.height = h;
        m_profile.fps = fps;
        m_profile.bitrate = bitrate;
    }

    // Crea un pool de texturas D3D11 (BGRA, RENDER_TARGET|SHADER_RESOURCE)
    // del tamano del perfil actual, usado por el smoke test de seleccion
    // de candidato de hardware.
    bool CreateTexturePool(uint32_t poolSize);

    // Devuelve la siguiente textura libre del anillo (round-robin) y
    // su indice en outIndex. No bloquea aunque la textura siguiente
    // siga "en vuelo" en el encoder (ver comentario en el .cpp).
    ID3D11Texture2D* AcquireWriteTexture(uint32_t& outIndex);

    // Envuelve la textura del pool en 'index' como IMFSample respaldado
    // por DXGI surface buffer, listo para enviarse a un MFT de hardware.
    ComPtr<IMFSample> WrapTextureAsSample(uint32_t index, LONGLONG pts, LONGLONG dur);

    // Aplica ajustes de codificacion de baja latencia (CBR, sin
    // B-frames, 1 ref frame, GOP, CABAC, etc.) via ICodecAPI si el MFT
    // lo soporta. No falla si el MFT no expone ICodecAPI.
    static void ApplyLowLatencyCodecSettings(IMFTransform* enc, uint32_t bitrate, uint32_t gop);

    // Codifica unos pocos frames negros y confirma que el MFT produce
    // al menos un NAL real. Usa el pool de texturas + WrapTextureAsSample.
    bool SmokeTestEncoder(IMFTransform* enc, MftDriver& driver, DWORD inSt, DWORD outSt);

    // Activa un candidato de MFT de hardware concreto (via IMFActivate),
    // le entrega el D3D Manager real, negocia tipos, aplica ajustes y
    // corre el smoke test. Devuelve true solo si paso todo, dejando
    // outEnc/outDriver listos para usarse en produccion.
    bool TryCandidate(IMFActivate* activate, ComPtr<IMFTransform>& outEnc,
        MftDriver& outDriver, DWORD& inSt, DWORD& outSt);

    // Enumera todos los MFT de HARDWARE registrados para NV12->H264,
    // los prueba en orden de merito con TryCandidate, y devuelve el
    // primero que pasa el smoke test.
    bool SelectAndInitHardwareEncoder(ComPtr<IMFTransform>& outEnc,
        MftDriver& outDriver, DWORD& inSt, DWORD& outSt);

    // Libera pool de texturas, device manager, contexto y device D3D11.
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