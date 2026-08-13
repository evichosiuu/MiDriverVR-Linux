#pragma once
#include <openvr_driver.h>
#include <cmath>


namespace HandSkeleton {

constexpr uint32_t kBoneCount = 31;

enum BoneIndex : int {
    Root = 0, Wrist,
    Thumb0, Thumb1, Thumb2, Thumb3,
    Index0, Index1, Index2, Index3, Index4,
    Middle0, Middle1, Middle2, Middle3, Middle4,
    Ring0, Ring1, Ring2, Ring3, Ring4,
    Pinky0, Pinky1, Pinky2, Pinky3, Pinky4,
    AuxThumb, AuxIndex, AuxMiddle, AuxRing, AuxPinky
};

// Eje local sobre el que se dobla cada falange al aumentar el curl.
// Probar {1,0,0} o {0,1,0} si en juego el dedo se curva "de costado".
static const float BEND_AXIS[3] = { 0.f, 0.f, 1.f };

inline vr::VRBoneTransform_t MakeBone(float px, float py, float pz,
    float qx, float qy, float qz, float qw) {
    vr::VRBoneTransform_t b;
    b.position = { px, py, pz, 1.f };
    b.orientation = { qw, qx, qy, qz }; // HmdQuaternionf_t es (w,x,y,z)
    return b;
}

inline vr::VRBoneTransform_t IdentityBone(float px, float py, float pz) {
    return MakeBone(px, py, pz, 0.f, 0.f, 0.f, 1.f);
}

// Quaternion = rotacion de 'angleRad' alrededor de BEND_AXIS.
inline vr::VRBoneTransform_t BentBone(float segLen, float curl01, float maxAngleRad) {
    float angle = curl01 * maxAngleRad;
    float half = angle * 0.5f;
    float s = sinf(half), c = cosf(half);
    // El hueso apunta a lo largo de +X local hacia el siguiente hueso.
    return MakeBone(segLen, 0.f, 0.f,
        BEND_AXIS[0] * s, BEND_AXIS[1] * s, BEND_AXIS[2] * s, c);
}

// Longitudes de falange aproximadas (metros) y angulo maximo de
// flexion por articulacion (radianes). Ajustables a gusto.
struct FingerSpec { float lenProx, lenMid, lenDist, lenTip; float maxProx, maxMid, maxDist; };

static const FingerSpec kThumb  = { 0.040f, 0.030f, 0.020f, 0.010f, 0.85f, 0.70f, 0.0f };
static const FingerSpec kIndex  = { 0.040f, 0.025f, 0.020f, 0.010f, 1.55f, 1.75f, 1.45f };
static const FingerSpec kMiddle = { 0.045f, 0.028f, 0.022f, 0.010f, 1.55f, 1.75f, 1.45f };
static const FingerSpec kRing   = { 0.042f, 0.026f, 0.020f, 0.010f, 1.55f, 1.75f, 1.45f };
static const FingerSpec kPinky  = { 0.035f, 0.020f, 0.018f, 0.010f, 1.55f, 1.75f, 1.45f };

// curl[5] = {thumb, index, middle, ring, pinky}, cada uno 0..1
// isRight = true para mano derecha (espeja X de los metacarpianos)
inline void BuildHandSkeleton(bool isRight, const float curl[5], vr::VRBoneTransform_t* out) {
    const float sgn = isRight ? 1.f : -1.f;

    // Root y Wrist: identidad. La orientacion global de la mano ya la
    // aporta la pose del "controller" (CMyController::GetPose()); acá
    // solo animamos la forma interna del esqueleto.
    out[Root]  = IdentityBone(0.f, 0.f, 0.f);
    out[Wrist] = IdentityBone(0.f, 0.f, 0.f);

    // Metacarpianos: posicion fija (splay de la mano), sin curl.
    out[Thumb0]  = IdentityBone(0.020f * sgn, 0.010f, 0.020f);
    out[Index0]  = IdentityBone(0.010f * sgn, 0.005f, 0.080f);
    out[Middle0] = IdentityBone(0.000f,       0.005f, 0.085f);
    out[Ring0]   = IdentityBone(-0.010f * sgn,0.005f, 0.080f);
    out[Pinky0]  = IdentityBone(-0.020f * sgn,0.000f, 0.072f);

    // Pulgar: 2 articulaciones activas (Thumb1, Thumb2) + tip fijo.
    out[Thumb1] = BentBone(kThumb.lenProx, curl[0], kThumb.maxProx);
    out[Thumb2] = BentBone(kThumb.lenMid,  curl[0], kThumb.maxMid);
    out[Thumb3] = IdentityBone(kThumb.lenDist, 0.f, 0.f);

    auto buildLongFinger = [&](BoneIndex b0, BoneIndex b1, BoneIndex b2, BoneIndex b3,
                                const FingerSpec& spec, float c) {
        out[b0] = BentBone(spec.lenProx, c, spec.maxProx);
        out[b1] = BentBone(spec.lenMid,  c, spec.maxMid);
        out[b2] = BentBone(spec.lenDist, c, spec.maxDist);
        out[b3] = IdentityBone(spec.lenTip, 0.f, 0.f); // tip
    };

    buildLongFinger(Index1, Index2, Index3, Index4, kIndex, curl[1]);
    buildLongFinger(Middle1, Middle2, Middle3, Middle4, kMiddle, curl[2]);
    buildLongFinger(Ring1, Ring2, Ring3, Ring4, kRing, curl[3]);
    buildLongFinger(Pinky1, Pinky2, Pinky3, Pinky4, kPinky, curl[4]);

    // Huesos auxiliares (splay/normal de punta): identidad, no son
    // criticos para que el curl se vea/funcione.
    out[AuxThumb]  = IdentityBone(0.f, 0.f, 0.f);
    out[AuxIndex]  = IdentityBone(0.f, 0.f, 0.f);
    out[AuxMiddle] = IdentityBone(0.f, 0.f, 0.f);
    out[AuxRing]   = IdentityBone(0.f, 0.f, 0.f);
    out[AuxPinky]  = IdentityBone(0.f, 0.f, 0.f);
}

} // namespace HandSkeleton
