#pragma once

#ifndef _WIN32

#include "Platform.h"
#include <x264.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <vector>
#include <cstdint>

inline int SilentX11ErrorHandler(Display*, XErrorEvent*) {
    return 0;
}

class LinuxVideoEncoder {
public:
    x264_t* m_encoder = nullptr;
    x264_picture_t m_picIn{};
    x264_picture_t m_picOut{};
    int m_width = 1280;
    int m_height = 720;
    int m_fps = 30;
    int m_bitrate = 6000000;
    int m_gopSize = 30;

    Display* m_display = nullptr;
    Window m_rootWindow = 0;

    bool Init(int w, int h, int fps, int bitrate, int gopSize) {
        if (w <= 0 || h <= 0) {
            DbgLog("[MiDriverVR] Dimensiones invalidas (%dx%d), usando 1280x720 por defecto", w, h);
            w = 1280; h = 720;
        }
        if (w % 2 != 0) w &= ~1;
        if (h % 2 != 0) h &= ~1;
        if (fps <= 0) fps = 30;
        if (bitrate <= 0) bitrate = 6000000;
        if (gopSize <= 0) gopSize = fps;

        m_width = w; m_height = h; m_fps = fps; m_bitrate = bitrate; m_gopSize = gopSize;

        DbgLog("[MiDriverVR] LinuxVideoEncoder Init: %dx%d @%dfps, bitrate=%d, GOP=%d",
               m_width, m_height, m_fps, m_bitrate, m_gopSize);

        x264_param_t param;
        if (x264_param_default_preset(&param, "ultrafast", "zerolatency") < 0) {
            DbgLog("[MiDriverVR] x264_param_default_preset fallo");
            return false;
        }

        param.i_width = m_width;
        param.i_height = m_height;
        param.i_fps_num = m_fps;
        param.i_fps_den = 1;
        param.i_keyint_max = m_gopSize;
        param.i_keyint_min = m_gopSize;
        param.b_intra_refresh = 1;
        param.rc.i_rc_method = X264_RC_ABR;
        param.rc.i_bitrate = m_bitrate / 1000;
        param.i_csp = X264_CSP_NV12;
        param.b_vfr_input = 0;
        param.b_repeat_headers = 1;
        param.i_threads = 0;

        if (x264_picture_alloc(&m_picIn, X264_CSP_NV12, m_width, m_height) < 0) {
            DbgLog("[MiDriverVR] x264_picture_alloc fallo");
            return false;
        }

        m_encoder = x264_encoder_open(&param);
        if (!m_encoder) {
            DbgLog("[MiDriverVR] x264_encoder_open fallo");
            x264_picture_clean(&m_picIn);
            return false;
        }

        XSetErrorHandler(SilentX11ErrorHandler);
        const char* dispEnv = getenv("DISPLAY");
        if (dispEnv && *dispEnv != '\0') {
            m_display = XOpenDisplay(NULL);
            if (m_display) {
                m_rootWindow = DefaultRootWindow(m_display);
                DbgLog("[MiDriverVR] X11 Display (%s) abierto OK para captura video", dispEnv);
            } else {
                DbgLog("[MiDriverVR] Aviso: XOpenDisplay fallo en %s, usando patron de prueba para streaming", dispEnv);
            }
        } else {
            DbgLog("[MiDriverVR] Aviso: DISPLAY no configurado (Wayland/headless), usando patron de prueba para streaming");
            m_display = nullptr;
            m_rootWindow = 0;
        }
        return true;
    }

    void Cleanup() {
        if (m_encoder) {
            x264_encoder_close(m_encoder);
            m_encoder = nullptr;
        }
        x264_picture_clean(&m_picIn);
        if (m_display) {
            XCloseDisplay(m_display);
            m_display = nullptr;
        }
    }

    bool CaptureBGRA(std::vector<uint8_t>& bgraFrame) {
        bgraFrame.resize((size_t)m_width * m_height * 4);
        bool captured = false;

        if (m_display && m_rootWindow) {
            XWindowAttributes gattr{};
            if (XGetWindowAttributes(m_display, m_rootWindow, &gattr) != 0) {
                int capW = (m_width < gattr.width) ? m_width : gattr.width;
                int capH = (m_height < gattr.height) ? m_height : gattr.height;
                if (capW > 0 && capH > 0) {
                    XImage* image = XGetImage(m_display, m_rootWindow, 0, 0, capW, capH, AllPlanes, ZPixmap);
                    if (image && image->data) {
                        if (image->bits_per_pixel == 32) {
                            for (int y = 0; y < capH && y < m_height; ++y) {
                                const uint8_t* srcLine = reinterpret_cast<const uint8_t*>(image->data) + y * image->bytes_per_line;
                                uint8_t* dstLine = bgraFrame.data() + y * m_width * 4;
                                memcpy(dstLine, srcLine, (size_t)capW * 4);
                            }
                        } else {
                            for (int y = 0; y < capH && y < m_height; ++y) {
                                for (int x = 0; x < capW && x < m_width; ++x) {
                                    unsigned long pixel = XGetPixel(image, x, y);
                                    size_t idx = (size_t)(y * m_width + x) * 4;
                                    bgraFrame[idx + 0] = (pixel) & 0xFF;
                                    bgraFrame[idx + 1] = (pixel >> 8) & 0xFF;
                                    bgraFrame[idx + 2] = (pixel >> 16) & 0xFF;
                                    bgraFrame[idx + 3] = 0xFF;
                                }
                            }
                        }
                        XDestroyImage(image);
                        captured = true;
                    }
                }
            }
        }

        if (!captured) {
            static uint8_t color = 0;
            color += 2;
            for (size_t i = 0; i < bgraFrame.size(); i += 4) {
                bgraFrame[i + 0] = color;
                bgraFrame[i + 1] = 128;
                bgraFrame[i + 2] = 255 - color;
                bgraFrame[i + 3] = 255;
            }
        }
        return true;
    }

    bool Encode(const uint8_t* bgra, std::vector<std::vector<uint8_t>>& nalUnits, bool forceIDR) {
        if (!bgra || !m_encoder) return false;

        uint8_t* pY = m_picIn.img.plane[0];
        uint8_t* pUV = m_picIn.img.plane[1];
        if (!pY || !pUV) return false;

        int strideY = m_picIn.img.i_stride[0];
        int strideUV = m_picIn.img.i_stride[1];

        for (int r = 0; r < m_height; ++r) {
            const uint8_t* line = bgra + r * m_width * 4;
            uint8_t* dstY = pY + r * strideY;
            for (int c = 0; c < m_width; ++c) {
                uint8_t b = line[c * 4 + 0], g = line[c * 4 + 1], rVal = line[c * 4 + 2];
                dstY[c] = (uint8_t)(((77 * rVal + 150 * g + 29 * b + 128) >> 8));
            }
            if ((r & 1) == 0) {
                uint8_t* dstUV = pUV + (r / 2) * strideUV;
                for (int c = 0; c < m_width; c += 2) {
                    uint8_t b = line[c * 4 + 0], g = line[c * 4 + 1], rVal = line[c * 4 + 2];
                    dstUV[c] = (uint8_t)(((-43 * rVal - 85 * g + 128 * b + 128) >> 8) + 128);
                    dstUV[c + 1] = (uint8_t)(((128 * rVal - 107 * g - 21 * b + 128) >> 8) + 128);
                }
            }
        }

        if (forceIDR) {
            m_picIn.i_type = X264_TYPE_IDR;
        } else {
            m_picIn.i_type = X264_TYPE_AUTO;
        }

        x264_nal_t* nals = nullptr;
        int iNal = 0;
        int frameSize = x264_encoder_encode(m_encoder, &nals, &iNal, &m_picIn, &m_picOut);
        if (frameSize < 0) {
            DbgLog("[MiDriverVR] x264_encoder_encode fallo (%d)", frameSize);
            return false;
        }

        nalUnits.clear();
        for (int i = 0; i < iNal; ++i) {
            if (nals[i].p_payload && nals[i].i_payload > 0) {
                std::vector<uint8_t> nalData(nals[i].p_payload, nals[i].p_payload + nals[i].i_payload);
                nalUnits.push_back(std::move(nalData));
            }
        }
        return !nalUnits.empty();
    }
};

#endif
