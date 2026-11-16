#pragma once

#include <cmath>
#include <cstdint>

namespace CameraOrbit
{
    struct Config
    {
        bool enabled = true;
        bool blockShift = true;
        float deadzone = 0.30f;         // Stick must be pushed past this before the camera moves
        float smoothTimeSec = 0.05f;    // Time to reach a new angle (0 = instant)
        float returnTimeSec = 0.25f;    // Time to swing back behind the car after release
        bool invertX = false;           // Swap the left/right sides
        float minElevRad = 0.035f;      // Keep camera above road plane (~2 deg above target)
        float maxElevRad = 0.88f;       // Cap vertical tilt below zenith (~50.4 deg)
    };

    // Pure framerate-independent controller math (no game access).
    //
    // The stick DIRECTION picks where the camera sits around the car; how far it is
    // pushed does not matter once it is past the deadzone:
    //   down  -> behind the car (the normal view, yaw 0)
    //   up    -> in front of the car, looking back at it (yaw 180)
    //   right -> on the car's left side  (yaw +90)
    //   left  -> on the car's right side (yaw -90)
    class Controller
    {
    public:
        static constexpr float kPi = 3.14159265f;

        static float Clamp(float v, float lo, float hi)
        {
            return (v < lo) ? lo : ((v > hi) ? hi : v);
        }

        static float Lerp(float a, float b, float t)
        {
            return a + (b - a) * t;
        }

        static float WrapPi(float a)
        {
            while (a > kPi) a -= 2.0f * kPi;
            while (a < -kPi) a += 2.0f * kPi;
            return a;
        }

        // Returns false inside the deadzone. Otherwise outYaw is the orbit angle in radians.
        static bool StickToYaw(float normX, float normY, float deadzone, bool invertX, float& outYaw)
        {
            const float dz = Clamp(deadzone, 0.05f, 0.90f);
            const float mag = sqrtf(normX * normX + normY * normY);
            if (mag <= dz) {
                outYaw = 0.0f;
                return false;
            }
            outYaw = atan2f(invertX ? -normX : normX, -normY);
            return true;
        }

        void Reset()
        {
            yaw = 0.0f;
        }

        void Update(float dt, bool active, float normStickX, float normStickY, const Config& cfg)
        {
            if (!active) {
                Reset();
                return;
            }
            if (dt <= 0.0f) {
                return;
            }
            if (dt > 0.05f) {
                dt = 0.05f;
            }

            float targetYaw = 0.0f;
            const bool stickActive = StickToYaw(normStickX, normStickY, cfg.deadzone, cfg.invertX, targetYaw);
            const float timeToReach = stickActive ? cfg.smoothTimeSec : cfg.returnTimeSec;

            // Always turn the short way round, also across the 180 degree seam at the front.
            const float diff = WrapPi(targetYaw - yaw);
            if (timeToReach <= 0.001f) {
                yaw = targetYaw;
            } else {
                // Plain exponential approach: about 95% of the way there after timeToReach.
                const float alpha = 1.0f - expf(-3.0f * dt / timeToReach);
                yaw = WrapPi(yaw + diff * alpha);
            }
            if (!stickActive && fabsf(yaw) < 0.002f) {
                yaw = 0.0f;
            }
        }

        bool HasOffset() const
        {
            return fabsf(yaw) > 1e-4f;
        }

        bool ComputeOrbitEye(const float eye[3], const float target[3], const float up[3],
                             const Config& cfg, float outEye[3], float outUp[3] = nullptr) const
        {
            const float vx = eye[0] - target[0];
            const float vy = eye[1] - target[1];
            const float vz = eye[2] - target[2];
            const float dist = sqrtf(vx * vx + vy * vy + vz * vz);
            if (dist < 0.05f) return false;

            const float uLen = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
            if (uLen < 1e-4f) return false;
            const float carUx = up[0] / uLen;
            const float carUy = up[1] / uLen;
            const float carUz = up[2] / uLen;

            // NFS ProStreet uses +Z as world vertical up. Blend away car-chassis pitch/roll
            // as soon as the camera orbits so swinging sideways never rolls the horizon.
            const float horizonBlend = Clamp(fabsf(yaw) / 0.12f, 0.0f, 1.0f);
            float ux = carUx * (1.0f - horizonBlend);
            float uy = carUy * (1.0f - horizonBlend);
            float uz = carUz * (1.0f - horizonBlend) + horizonBlend;
            const float eULen = sqrtf(ux * ux + uy * uy + uz * uz);
            if (eULen < 1e-4f) {
                ux = 0.0f;
                uy = 0.0f;
                uz = 1.0f;
            } else {
                ux /= eULen;
                uy /= eULen;
                uz /= eULen;
            }

            if (outUp) {
                outUp[0] = ux;
                outUp[1] = uy;
                outUp[2] = uz;
            }

            const float dotVU = vx * ux + vy * uy + vz * uz;
            const float baseElev = asinf(Clamp(dotVU / dist, -0.95f, 0.95f));

            float rx = vx - dotVU * ux;
            float ry = vy - dotVU * uy;
            float rz = vz - dotVU * uz;
            const float rLen = sqrtf(rx * rx + ry * ry + rz * rz);
            if (rLen < 1e-4f) return false;
            rx /= rLen;
            ry /= rLen;
            rz /= rLen;

            // Camera looks from eye toward target (fwd = -r), so right = cross(fwd, up) = cross(up, r).
            const float rightX = uy * rz - uz * ry;
            const float rightY = uz * rx - ux * rz;
            const float rightZ = ux * ry - uy * rx;

            // Positive yaw swings the eye toward the car's left side.
            const float cy = cosf(yaw);
            const float sy = sinf(yaw);
            const float hx = cy * rx - sy * rightX;
            const float hy = cy * ry - sy * rightY;
            const float hz = cy * rz - sy * rightZ;

            // Guard against dipping under uphill road surfaces when orbited around to the front.
            const float frontWeight = sinf(Clamp(fabsf(yaw) * 0.5f, 0.0f, 1.5707963f));
            const float elev = Lerp(baseElev, Clamp(baseElev, cfg.minElevRad, cfg.maxElevRad), frontWeight);
            const float ce = cosf(elev);
            const float se = sinf(elev);
            outEye[0] = target[0] + (hx * ce + ux * se) * dist;
            outEye[1] = target[1] + (hy * ce + uy * se) * dist;
            outEye[2] = target[2] + (hz * ce + uz * se) * dist;
            return true;
        }

        float yaw = 0.0f; // Current orbit angle, radians, kept in [-pi, pi]
    };

    void Init(uintptr_t baseModule);
}
