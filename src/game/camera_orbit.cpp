#include "game/camera_orbit.h"

#include <windows.h>
#include <cctype>
#include <cstring>
#include <string>

#include "minhook/MinHook.h"
#include "game/race_mode.h"
#include "log.h"

namespace CameraOrbit
{
    namespace
    {
        // Verified NFS ProStreet v1.1 (imagebase 0x00400000) RVAs:
        const uintptr_t DRIVING_CAMERA_UPDATE_RVA = 0x0024FFD0; // DrivingCameraMover::Update(float dt)
        const uintptr_t BUILD_LOOK_AT_MATRIX_RVA  = 0x002FF060; // BuildLookAtMatrix(mat, eye, target, up)
        const uintptr_t DRIVING_CAMERA_LOOK_AT_RET_RVA = 0x00250AA4; // Return address of the mover's BuildLookAtMatrix call
        const uintptr_t SHIFT_BY_DELTA_RVA       = 0x0001C670; // Player input right-stick/action shift override
        const uintptr_t LOCAL_PLAYER_LIST_RVA     = 0x006CE444; // Game::LocalPlayerList
        const uintptr_t LOCAL_PLAYER_COUNT_RVA    = 0x006CE44C;
        const uintptr_t COM_FIND_INTERFACE_RVA    = 0x000614B0; // COM::Object::FindInterface(typeFn)
        const uintptr_t IINPUT_TYPE_ID_FN_RVA     = 0x00002AC0; // IInput interface type-key getter
        const uintptr_t CUTSCENE_OVERRIDE_RVA     = 0x006B4960; // Nonzero during camera cutscenes/overrides
        const uintptr_t SIM_SYSTEM_PTR_RVA        = 0x007FB400; // SimSystem*, +0x2C paused byte

        struct XInputGamepadLocal
        {
            uint16_t wButtons;
            uint8_t bLeftTrigger;
            uint8_t bRightTrigger;
            int16_t sThumbLX;
            int16_t sThumbLY;
            int16_t sThumbRX;
            int16_t sThumbRY;
        };

        struct XInputStateLocal
        {
            uint32_t dwPacketNumber;
            XInputGamepadLocal Gamepad;
        };

        typedef uint32_t (WINAPI *XInputGetStateFn)(uint32_t userIndex, XInputStateLocal* state);
        typedef void (__attribute__((thiscall)) *CameraMoverUpdateFn)(void* mover, float dt);
        typedef void (__cdecl *BuildLookAtMatrixFn)(float* outMat, const float* eye, const float* target, const float* up);
        typedef uint8_t (__stdcall *ShiftByDeltaFn)(void* transObj, int delta);
        typedef void* (__attribute__((thiscall)) *ComFindInterfaceFn)(uintptr_t comObj, uintptr_t typeIdFn);
        typedef uint8_t (__attribute__((thiscall)) *InputIsAutoTransFn)(void* input);

        uintptr_t g_base = 0;
        Config g_cfg;
        Controller g_ctrl;

        CameraMoverUpdateFn g_origCameraUpdate = nullptr;
        BuildLookAtMatrixFn g_origBuildLookAt  = nullptr;
        ShiftByDeltaFn      g_origShiftByDelta = nullptr;
        XInputGetStateFn    g_xinputGetState   = nullptr;

        void* g_activeOrbitMover = nullptr;
        void* g_lastMover        = nullptr;

        bool SafeReadPtr(uintptr_t addr, uintptr_t& outVal)
        {
            if (addr == 0 || IsBadReadPtr(reinterpret_cast<const void*>(addr), sizeof(uintptr_t)) != 0) {
                return false;
            }
            outVal = *reinterpret_cast<const uintptr_t*>(addr);
            return outVal != 0;
        }

        bool HasGameFocus()
        {
            HWND fg = GetForegroundWindow();
            if (!fg) return false;
            DWORD fgPid = 0;
            GetWindowThreadProcessId(fg, &fgPid);
            return fgPid == GetCurrentProcessId();
        }

        void ResolveXInput()
        {
            static bool attempted = false;
            if (attempted) return;
            attempted = true;

            const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
            for (const char* name : dlls) {
                HMODULE mod = LoadLibraryA(name);
                if (mod) {
                    g_xinputGetState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(mod, "XInputGetState"));
                    if (g_xinputGetState) {
                        Log::Debug("[CameraOrbit] Loaded %s for right-stick polling.\n", name);
                        return;
                    }
                }
            }
            Log::Warn("[CameraOrbit] No XInput DLL available; right-stick camera orbit will stay centered.\n");
        }

        void PollRightStick(float& outNormX, float& outNormY)
        {
            outNormX = 0.0f;
            outNormY = 0.0f;
            if (!g_xinputGetState || !HasGameFocus()) {
                return;
            }

            float bestMagSq = -1.0f;
            for (uint32_t i = 0; i < 4; ++i) {
                XInputStateLocal state{};
                if (g_xinputGetState(i, &state) != ERROR_SUCCESS) {
                    continue;
                }
                const float nx = (state.Gamepad.sThumbRX >= 0)
                    ? (state.Gamepad.sThumbRX / 32767.0f)
                    : (state.Gamepad.sThumbRX / 32768.0f);
                const float ny = (state.Gamepad.sThumbRY >= 0)
                    ? (state.Gamepad.sThumbRY / 32767.0f)
                    : (state.Gamepad.sThumbRY / 32768.0f);
                const float magSq = nx * nx + ny * ny;
                if (magSq > bestMagSq) {
                    bestMagSq = magSq;
                    outNormX = nx;
                    outNormY = ny;
                }
            }
        }

        bool IsSimPausedOrCutscene()
        {
            const uint8_t* cutsceneFlag = reinterpret_cast<const uint8_t*>(g_base + CUTSCENE_OVERRIDE_RVA);
            if (!IsBadReadPtr(cutsceneFlag, sizeof(uint8_t)) && *cutsceneFlag != 0) {
                return true;
            }
            uintptr_t sim = 0;
            if (!SafeReadPtr(g_base + SIM_SYSTEM_PTR_RVA, sim)) {
                return true;
            }
            const uint8_t* paused = reinterpret_cast<const uint8_t*>(sim + 0x2C);
            return !IsBadReadPtr(paused, sizeof(uint8_t)) && *paused != 0;
        }

        bool IsDragMode()
        {
            std::string mode = ReadRaceMode(g_base);
            for (char& ch : mode) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return mode.find("drag") != std::string::npos;
        }

        bool IsLocalPlayerAutoTransmission()
        {
            uintptr_t listPtr = 0;
            if (!SafeReadPtr(g_base + LOCAL_PLAYER_LIST_RVA, listPtr)) {
                return false;
            }
            const int* countPtr = reinterpret_cast<const int*>(g_base + LOCAL_PLAYER_COUNT_RVA);
            if (IsBadReadPtr(countPtr, sizeof(int)) || *countPtr <= 0) {
                return false;
            }
            uintptr_t entry = 0;
            if (!SafeReadPtr(listPtr, entry) || entry <= 8) {
                return false;
            }
            uintptr_t simable = 0;
            if (!SafeReadPtr(entry - 8, simable)) {
                return false;
            }
            uintptr_t comObj = 0;
            if (!SafeReadPtr(simable + 0x04, comObj)) {
                return false;
            }

            auto findIface = reinterpret_cast<ComFindInterfaceFn>(g_base + COM_FIND_INTERFACE_RVA);
            void* input = findIface(comObj, g_base + IINPUT_TYPE_ID_FN_RVA);
            if (!input) {
                return false;
            }

            uintptr_t vtable = 0;
            if (!SafeReadPtr(reinterpret_cast<uintptr_t>(input), vtable)) {
                return false;
            }
            uintptr_t fnAddr = 0;
            if (!SafeReadPtr(vtable + 0x58, fnAddr)) {
                return false;
            }
            return reinterpret_cast<InputIsAutoTransFn>(fnAddr)(input) != 0;
        }

        bool IsChaseCamera(void* mover)
        {
            if (!mover) return false;
            const uintptr_t m = reinterpret_cast<uintptr_t>(mover);
            if (IsBadReadPtr(mover, 0xF8) != 0) return false;

            const uint8_t isLookBack = *reinterpret_cast<const uint8_t*>(m + 0xBD);
            if (isLookBack != 0) return false;

            uintptr_t camParamsPtr = 0;
            if (!SafeReadPtr(m + 0xF4, camParamsPtr)) return false;
            if (IsBadReadPtr(reinterpret_cast<const void*>(camParamsPtr), 0x40) != 0) return false;

            const uint8_t* params = reinterpret_cast<const uint8_t*>(camParamsPtr);
            // Mirrors DrivingCameraMover::IsChaseCameraView (0x0063EC60): Bumper/Hood cams set +0x3D or +0x3F.
            return params[0x3D] == 0 && params[0x3F] == 0;
        }

        bool CanUseCameraOrbit()
        {
            return g_cfg.enabled && IsRaceActive(g_base) && !IsDragMode() && IsLocalPlayerAutoTransmission();
        }

        void __attribute__((thiscall)) Hook_DrivingCameraMover_Update(void* mover, float dt)
        {
            if (!g_cfg.enabled) {
                g_ctrl.Reset();
                g_lastMover = mover;
                g_activeOrbitMover = nullptr;
                g_origCameraUpdate(mover, dt);
                return;
            }
            if (mover != g_lastMover) {
                g_ctrl.Reset();
                g_lastMover = mover;
            }

            const bool pausedOrCutscene = IsSimPausedOrCutscene();
            const bool raceOk = CanUseCameraOrbit();
            const bool viewOk = IsChaseCamera(mover);

            if (!pausedOrCutscene) {
                if (raceOk && viewOk) {
                    float rx = 0.0f, ry = 0.0f;
                    PollRightStick(rx, ry);
                    g_ctrl.Update(dt, true, rx, ry, g_cfg);
                } else {
                    g_ctrl.Reset();
                }
            }

            g_activeOrbitMover = (raceOk && viewOk && !pausedOrCutscene && g_ctrl.HasOffset()) ? mover : nullptr;
            g_origCameraUpdate(mover, dt);
            g_activeOrbitMover = nullptr;
        }

        void __cdecl Hook_BuildLookAtMatrix(float* outMat, const float* eye, const float* target, const float* up)
        {
            // Only the camera mover's own call (0x00650A9F) gets the orbit. The game rebuilds the
            // matrix again further down inside the same update from the already orbited eye;
            // orbiting that second call too doubled the angle (stick at the side = camera in front).
            const uintptr_t returnAddr = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
            const bool moverCall = (returnAddr == g_base + DRIVING_CAMERA_LOOK_AT_RET_RVA);
            if (g_cfg.enabled && g_activeOrbitMover && moverCall && eye && target && up) {
                float orbitedEye[3] = { eye[0], eye[1], eye[2] };
                float orbitedUp[3]  = { up[0], up[1], up[2] };
                if (g_ctrl.ComputeOrbitEye(eye, target, up, g_cfg, orbitedEye, orbitedUp)) {
                    g_origBuildLookAt(outMat, orbitedEye, target, orbitedUp);
                    return;
                }
            }
            g_origBuildLookAt(outMat, eye, target, up);
        }

        uint8_t __stdcall Hook_ShiftByDelta(void* transObj, int delta)
        {
            if (g_cfg.enabled && g_cfg.blockShift && CanUseCameraOrbit()) {
                return 0;
            }
            return g_origShiftByDelta(transObj, delta);
        }

        void LoadSettings()
        {
            const std::string& settings = Log::SettingsPath();
            if (settings.empty()) return;

            g_cfg.enabled = GetPrivateProfileIntA("Camera", "RightStickOrbit", 0, settings.c_str()) != 0;
            g_cfg.blockShift = GetPrivateProfileIntA("Camera", "OrbitBlockShift", 1, settings.c_str()) != 0;

            const int dzPercent = GetPrivateProfileIntA("Camera", "OrbitDeadzone", 30, settings.c_str());
            g_cfg.deadzone = Controller::Clamp(dzPercent / 100.0f, 0.05f, 0.90f);

            // New key names on purpose: the old OrbitSmoothTime/OrbitReturnTime defaults (160/950 ms)
            // may still sit in a user ini and would bring the slow "feathered" movement back.
            const int snapMs = GetPrivateProfileIntA("Camera", "OrbitSnapTime", 50, settings.c_str());
            g_cfg.smoothTimeSec = Controller::Clamp(snapMs / 1000.0f, 0.0f, 1.0f);

            const int backMs = GetPrivateProfileIntA("Camera", "OrbitBackTime", 250, settings.c_str());
            g_cfg.returnTimeSec = Controller::Clamp(backMs / 1000.0f, 0.0f, 5.0f);

            g_cfg.invertX = GetPrivateProfileIntA("Camera", "OrbitInvertX", 0, settings.c_str()) != 0;
            g_cfg.invertY = GetPrivateProfileIntA("Camera", "OrbitInvertY", 0, settings.c_str()) != 0;
        }
    }

    static bool g_hooksInstalled = false;

    static bool EnsureHooks()
    {
        if (g_hooksInstalled) return true;
        if (g_base == 0) return false;
        ResolveXInput();

        const MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
            Log::Warn("[CameraOrbit] MH_Initialize failed (%d)\n", static_cast<int>(initStatus));
            return false;
        }

        void* camUpdateTarget = reinterpret_cast<void*>(g_base + DRIVING_CAMERA_UPDATE_RVA);
        void* lookAtTarget = reinterpret_cast<void*>(g_base + BUILD_LOOK_AT_MATRIX_RVA);
        void* shiftDeltaTarget = reinterpret_cast<void*>(g_base + SHIFT_BY_DELTA_RVA);

        const bool ok1 = MH_CreateHook(camUpdateTarget, reinterpret_cast<void*>(Hook_DrivingCameraMover_Update),
                                       reinterpret_cast<void**>(&g_origCameraUpdate)) == MH_OK &&
                         MH_EnableHook(camUpdateTarget) == MH_OK;
        const bool ok2 = MH_CreateHook(lookAtTarget, reinterpret_cast<void*>(Hook_BuildLookAtMatrix),
                                       reinterpret_cast<void**>(&g_origBuildLookAt)) == MH_OK &&
                         MH_EnableHook(lookAtTarget) == MH_OK;
        const bool ok3 = MH_CreateHook(shiftDeltaTarget, reinterpret_cast<void*>(Hook_ShiftByDelta),
                                       reinterpret_cast<void**>(&g_origShiftByDelta)) == MH_OK &&
                         MH_EnableHook(shiftDeltaTarget) == MH_OK;

        if (!ok1 || !ok2 || !ok3) {
            Log::Warn("[CameraOrbit] Failed to install hooks (update=%d lookat=%d shift=%d)\n",
                      ok1 ? 1 : 0, ok2 ? 1 : 0, ok3 ? 1 : 0);
            return false;
        }
        g_hooksInstalled = true;
        return true;
    }

    Config GetConfig()
    {
        return g_cfg;
    }

    void SetConfig(const Config& cfg)
    {
        g_cfg = cfg;
        g_cfg.deadzone = Controller::Clamp(g_cfg.deadzone, 0.05f, 0.90f);
        g_cfg.smoothTimeSec = Controller::Clamp(g_cfg.smoothTimeSec, 0.0f, 1.0f);
        g_cfg.returnTimeSec = Controller::Clamp(g_cfg.returnTimeSec, 0.0f, 5.0f);
        if (!g_cfg.enabled) {
            g_ctrl.Reset();
            g_activeOrbitMover = nullptr;
            return;
        }
        EnsureHooks();
    }

    void Init(uintptr_t baseModule)
    {
        g_base = baseModule;
        LoadSettings();

        if (!g_cfg.enabled) {
            Log::Event(Log::Tag::Game, "Right-stick camera orbit: off (set [Camera] RightStickOrbit=1 to turn it on)\n");
            return;
        }

        if (!EnsureHooks()) return;

        Log::Event(Log::Tag::Game,
                   "Right-stick camera orbit: active in non-drag auto races (stick direction picks the side, deadzone=%.0f%%, snap=%.0fms, back=%.0fms)\n",
                   g_cfg.deadzone * 100.0f, g_cfg.smoothTimeSec * 1000.0f, g_cfg.returnTimeSec * 1000.0f);
    }
}
