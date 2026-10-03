#pragma once

#ifndef _WIN32

#include "Platform.h"
#include <pulse/simple.h>
#include <pulse/error.h>
#include <vector>
#include <cstdint>

class LinuxAudioCapture {
public:
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int CHANNELS = 2;

    pa_simple* m_pa = nullptr;
    bool m_connected = false;

    bool Init() {
        pa_sample_spec ss;
        ss.format = PA_SAMPLE_S16LE;
        ss.rate = SAMPLE_RATE;
        ss.channels = CHANNELS;

        int error = 0;
        m_pa = pa_simple_new(
            nullptr,                // Use default server
            "MiDriverVR",          // Application name
            PA_STREAM_RECORD,      // Stream direction
            nullptr,                // Default device / monitor
            "SteamVR Audio Stream",// Stream description
            &ss,                    // Sample spec
            nullptr,                // Channel map
            nullptr,                // Buffer attributes
            &error
        );

        if (!m_pa) {
            DbgLog("Aviso: pa_simple_new fallo (error=%d: %s). Se usara generador de silencio para el streaming de audio.",
                   error, pa_strerror(error));
            m_connected = false;
        } else {
            DbgLog("PulseAudio captura iniciada OK (%dHz %dch PCM16)", SAMPLE_RATE, CHANNELS);
            m_connected = true;
        }
        return true;
    }

    void Cleanup() {
        if (m_pa) {
            pa_simple_free(m_pa);
            m_pa = nullptr;
        }
        m_connected = false;
    }

    bool ReadFrames(std::vector<int16_t>& outPcm, size_t frameCount) {
        size_t byteCount = frameCount * CHANNELS * sizeof(int16_t);
        outPcm.resize(frameCount * CHANNELS);

        if (m_connected && m_pa) {
            int error = 0;
            if (pa_simple_read(m_pa, outPcm.data(), byteCount, &error) < 0) {
                DbgLog("pa_simple_read fallo (error=%d: %s)", error, pa_strerror(error));
                std::fill(outPcm.begin(), outPcm.end(), 0);
            }
        } else {
            std::fill(outPcm.begin(), outPcm.end(), 0);
            Sleep(20);
        }
        return true;
    }
};

#endif
