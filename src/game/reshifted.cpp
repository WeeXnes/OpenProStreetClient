#include "game/reshifted.h"

#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "minhook/MinHook.h"
#include "game/race_mode.h"
#include "log.h"

// How the game's automatic gearbox works (NFS ProStreet v1.1, imagebase 0x00400000):
//
//   Engine simulate (0x00489590) asks vtable+0x48 = GetShiftPotential(gear, engineRPM, wheelRPM)
//   every step and stores the answer at engine+0x168 (0 = stay, 1 = down, 2 = up).
//   vtable+0x44 = Update(dt) (0x00489350) runs the automatic (0x00487A50), which reads
//   +0x168 and performs the shift (0x004881D0), one gear at a time.
//   0x004881D0 goes through vtable+0x30 = ChangeGear(gear) (0x00487DF0), which accepts any
//   gear, sets engine+0x84 to it at once and starts the shift timer at engine+0x88 from the
//   car's shift times (data+0x14 up, data+0x98 down). Power is cut while that timer runs.
//   The automatic also waits 0.5 s between an upshift and a downshift (+0x1DC / +0x1E0).
//
// So replacing the answer of GetShiftPotential is enough to drive the gearbox. The stock
// answer comes from two per-gear RPM tables in the engine object (+0xA0 up, +0xC8 down)
// that the game computes per car from its torque curve and gear ratios (0x00719550).
namespace ReShifted
{
    namespace
    {
        const uintptr_t ENGINE_UPDATE_RVA          = 0x00089350; // Engine vtable+0x44: Update(float dt)
        const uintptr_t ENGINE_SHIFT_POTENTIAL_RVA = 0x00087D40; // Engine vtable+0x48: GetShiftPotential(gear, rpm, wheelRPM)
        const uintptr_t ENGINE_CHANGE_GEAR_RVA     = 0x00087DF0; // Engine vtable+0x30: ChangeGear(gear)
        const uintptr_t LOCAL_PLAYER_LIST_RVA      = 0x006CE444;
        const uintptr_t LOCAL_PLAYER_COUNT_RVA     = 0x006CE44C;

        // Engine object field offsets:
        const uintptr_t ENGINE_OWNER_SIMABLE_OFF   = 0x2C;
        const uintptr_t ENGINE_ITRANS_OFF          = 0x44;
        const uintptr_t ENGINE_GEAR_OFF            = 0x84;
        const uintptr_t ENGINE_SHIFT_TIMER_OFF     = 0x88;  // Seconds left of the running gear change
        const uintptr_t ENGINE_SINCE_DOWNSHIFT_OFF = 0x1DC; // Automatic: counts to 0.5 s, then upshifts are allowed
        const uintptr_t ENGINE_SINCE_UPSHIFT_OFF   = 0x1E0; // Automatic: counts to 0.5 s, then downshifts are allowed
        const uintptr_t ENGINE_SHIFT_UP_RPM_OFF    = 0xA0;  // float[10], index = game gear
        const uintptr_t ENGINE_SHIFT_DOWN_RPM_OFF  = 0xC8;  // float[10], index = game gear
        const uintptr_t ENGINE_MANUAL_FLAG_OFF     = 0x11C;
        const uintptr_t ENGINE_IINPUT_OFF          = 0x120;
        const uintptr_t ENGINE_DATA_OFF            = 0x150;

        // Engine data (+0x150) field offsets:
        const uintptr_t DATA_REDLINE_RPM_OFF       = 0x21C;
        const uintptr_t DATA_WHEEL_GUARD_FIRST_OFF = 0x224; // Wheelspin guard factor used in 1st gear (0x00487C20)
        const uintptr_t DATA_WHEEL_GUARD_OTHER_OFF = 0x228; // Same, other gears
        const uintptr_t DATA_IDLE_RPM_OFF          = 0x23C;
        const size_t    DATA_READ_SIZE             = 0x240;

        // IInput vtable slots. The pedals sit at +0x184C (gas) and +0x1850 (brake) of the input
        // object. Slot +0x08 is the BRAKE (the stock gearbox refuses to shift up unless it is 0)
        // and slot +0x0C is the GAS. The first version of this file had the two swapped.
        const uintptr_t INPUT_VT_BRAKE             = 0x08;
        const uintptr_t INPUT_VT_GAS               = 0x0C;
        const uintptr_t INPUT_VT_IS_AUTOMATIC      = 0x58;

        typedef void (__attribute__((thiscall)) *EngineUpdateFn)(void* engine, float dt);
        typedef int (__attribute__((thiscall)) *EngineGetShiftPotentialFn)(void* engine, int gear, float rpm, float wheelRPM);
        typedef int (__attribute__((thiscall)) *EngineChangeGearFn)(void* engine, int gear);
        typedef int (__attribute__((thiscall)) *TransGetTopGearFn)(void* iTrans);
        typedef float (__attribute__((thiscall)) *InputGetFloatFn)(void* iInput);
        typedef uint8_t (__attribute__((thiscall)) *InputIsAutoFn)(void* iInput);

        uintptr_t g_base = 0;
        Config g_cfg;
        Controller g_ctrl;
        ShiftProfile g_profile;

        void* g_cachedEngine = nullptr;
        uintptr_t g_cachedEngineData = 0;
        int g_cachedTopGear = 0;
        float g_cachedTables[2 * kMaxGears]{};
        int g_lastLoggedPotential = -1;
        volatile DWORD g_lastActiveTick = 0; // GetTickCount() of the last shift decision, for the overlay
        float g_lastRPM = 0.0f;

        EngineUpdateFn            g_origEngineUpdate = nullptr;
        EngineGetShiftPotentialFn g_origGetShiftPotential = nullptr;
        EngineChangeGearFn        g_origChangeGear = nullptr;

        bool g_inAutoUpdate = false; // True while the game's automatic runs for the local player's engine
        int g_skipFromGear = -1;     // Gear the pending downshift was decided in
        int g_skipTarget = -1;       // Gear that downshift should end in (may be more than one below)

        bool SafeReadPtr(uintptr_t addr, uintptr_t& outVal)
        {
            if (addr == 0 || IsBadReadPtr(reinterpret_cast<const void*>(addr), sizeof(uintptr_t)) != 0) {
                return false;
            }
            outVal = *reinterpret_cast<const uintptr_t*>(addr);
            return outVal != 0;
        }

        uintptr_t GetLocalPlayerSimable()
        {
            uintptr_t listPtr = 0;
            if (!SafeReadPtr(g_base + LOCAL_PLAYER_LIST_RVA, listPtr)) {
                return 0;
            }
            const int* countPtr = reinterpret_cast<const int*>(g_base + LOCAL_PLAYER_COUNT_RVA);
            if (IsBadReadPtr(countPtr, sizeof(int)) || *countPtr <= 0) {
                return 0;
            }
            uintptr_t entry = 0;
            if (!SafeReadPtr(listPtr, entry) || entry <= 8) {
                return 0;
            }
            uintptr_t simable = 0;
            if (!SafeReadPtr(entry - 8, simable)) {
                return 0;
            }
            return simable;
        }

        bool IsDragMode()
        {
            std::string mode = ReadRaceMode(g_base);
            for (char& ch : mode) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return mode.find("drag") != std::string::npos;
        }

        bool IsLocalPlayerAutoEngine(void* engine, void** outInput = nullptr)
        {
            if (!g_cfg.enabled || !engine || !IsRaceActive(g_base) || IsDragMode()) {
                return false;
            }
            const uintptr_t eng = reinterpret_cast<uintptr_t>(engine);
            if (IsBadReadPtr(engine, 0x1E8) != 0) {
                return false;
            }

            const uintptr_t localSimable = GetLocalPlayerSimable();
            const uintptr_t ownerSimable = *reinterpret_cast<const uintptr_t*>(eng + ENGINE_OWNER_SIMABLE_OFF);
            if (localSimable == 0 || ownerSimable != localSimable) {
                return false;
            }

            const int manualOverride = *reinterpret_cast<const int*>(eng + ENGINE_MANUAL_FLAG_OFF);
            if (manualOverride == 1) {
                return false;
            }

            void* input = *reinterpret_cast<void* const*>(eng + ENGINE_IINPUT_OFF);
            if (!input) return false;

            uintptr_t inputVt = 0;
            if (!SafeReadPtr(reinterpret_cast<uintptr_t>(input), inputVt)) return false;
            uintptr_t isAutoAddr = 0;
            if (!SafeReadPtr(inputVt + INPUT_VT_IS_AUTOMATIC, isAutoAddr)) return false;
            if (reinterpret_cast<InputIsAutoFn>(isAutoAddr)(input) == 0) {
                return false;
            }

            if (outInput) *outInput = input;
            return true;
        }

        void ReadPedals(void* input, float& outGas, float& outBrake)
        {
            outGas = 0.0f;
            outBrake = 0.0f;

            uintptr_t vt = 0;
            if (!SafeReadPtr(reinterpret_cast<uintptr_t>(input), vt)) return;

            uintptr_t fnGas = 0, fnBrake = 0;
            if (SafeReadPtr(vt + INPUT_VT_GAS, fnGas))     outGas   = reinterpret_cast<InputGetFloatFn>(fnGas)(input);
            if (SafeReadPtr(vt + INPUT_VT_BRAKE, fnBrake)) outBrake = reinterpret_cast<InputGetFloatFn>(fnBrake)(input);
        }

        int ReadTopGear(void* engine)
        {
            const uintptr_t iTrans = reinterpret_cast<uintptr_t>(engine) + ENGINE_ITRANS_OFF;
            uintptr_t vt = 0;
            if (!SafeReadPtr(iTrans, vt)) return 0;
            uintptr_t fnAddr = 0;
            if (!SafeReadPtr(vt + 0x08, fnAddr)) return 0;
            return reinterpret_cast<TransGetTopGearFn>(fnAddr)(reinterpret_cast<void*>(iTrans));
        }

        // Copies the game's two shift tables (up[10] then down[10]) out of the engine object.
        void ReadGameTables(void* engine, float out[2 * kMaxGears])
        {
            const uintptr_t eng = reinterpret_cast<uintptr_t>(engine);
            std::memcpy(out, reinterpret_cast<const void*>(eng + ENGINE_SHIFT_UP_RPM_OFF), kMaxGears * sizeof(float));
            std::memcpy(out + kMaxGears, reinterpret_cast<const void*>(eng + ENGINE_SHIFT_DOWN_RPM_OFF), kMaxGears * sizeof(float));
        }

        void RebuildProfile(void* engine, uintptr_t engData, int topGear, const float tables[2 * kMaxGears])
        {
            if (IsBadReadPtr(reinterpret_cast<const void*>(engData), DATA_READ_SIZE) != 0) {
                return;
            }
            const float idle = *reinterpret_cast<const float*>(engData + DATA_IDLE_RPM_OFF);
            const float redline = *reinterpret_cast<const float*>(engData + DATA_REDLINE_RPM_OFF);

            g_profile.Build(idle, redline, topGear, tables, tables + kMaxGears, g_cfg);
            g_profile.wheelGuardFirst = *reinterpret_cast<const float*>(engData + DATA_WHEEL_GUARD_FIRST_OFF);
            g_profile.wheelGuardOther = *reinterpret_cast<const float*>(engData + DATA_WHEEL_GUARD_OTHER_OFF);

            g_cachedEngine = engine;
            g_cachedEngineData = engData;
            g_cachedTopGear = topGear;
            std::memcpy(g_cachedTables, tables, sizeof(g_cachedTables));

            char sport[160] = "";
            char drops[160] = "";
            size_t sLen = 0, dLen = 0;
            for (int g = 2; g < g_profile.topGear; ++g) {
                sLen += std::snprintf(sport + sLen, sizeof(sport) - sLen, "%s%.0f", g > 2 ? "/" : "", g_profile.sportRPM[g]);
                dLen += std::snprintf(drops + dLen, sizeof(drops) - dLen, "%s%.2f", g > 2 ? "/" : "", g_profile.drop[g]);
                if (sLen >= sizeof(sport) - 8 || dLen >= sizeof(drops) - 8) break;
            }

            if (g_cfg.logShifts) {
                Log::Event(Log::Tag::Game,
                           "[ReShifted] Car: gears=%d idle=%.0f redline=%.0f sportShift=%s rpmDrop=%s wheelGuard=%.2f/%.2f source=%s\n",
                           g_profile.topGear - 1, g_profile.idleRPM, g_profile.redlineRPM, sport, drops,
                           g_profile.wheelGuardFirst, g_profile.wheelGuardOther,
                           g_profile.fromGameTables ? "game" : "FALLBACK");
            }
        }

        void __attribute__((thiscall)) Hook_Engine_Update(void* engine, float dt)
        {
            void* input = nullptr;
            if (IsLocalPlayerAutoEngine(engine, &input)) {
                if (engine != g_cachedEngine) {
                    g_ctrl.Reset();
                    g_lastLoggedPotential = -1;
                    g_skipTarget = -1;
                }

                float gas = 0.0f, brake = 0.0f;
                ReadPedals(input, gas, brake);
                const uintptr_t eng = reinterpret_cast<uintptr_t>(engine);
                const int gear = *reinterpret_cast<const int*>(eng + ENGINE_GEAR_OFF);
                const int changed = g_ctrl.Tick(dt, gear, gas, brake, g_cfg);

                // Driven hard, the automatic's 0.5 s wait between an upshift and a downshift
                // shrinks to as little as 0.25 s. The counters are simply moved to "done".
                if (g_cfg.quickShiftTime < 0.999f) {
                    const float wait = ShiftProfile::Lerp(0.50f, 0.25f, ShiftProfile::Clamp(g_ctrl.sportiness, 0.0f, 1.0f));
                    float* sinceDown = reinterpret_cast<float*>(eng + ENGINE_SINCE_DOWNSHIFT_OFF);
                    float* sinceUp = reinterpret_cast<float*>(eng + ENGINE_SINCE_UPSHIFT_OFF);
                    if (*sinceDown >= wait && *sinceDown < 0.5f) *sinceDown = 0.5f;
                    if (*sinceUp >= wait && *sinceUp < 0.5f) *sinceUp = 0.5f;
                }

                if (changed != 0 && g_cfg.logShifts) {
                    Log::Event(Log::Tag::Game, "[ReShifted] Gear %d -> %d (rpm before=%.0f gas=%.2f brake=%.2f)\n",
                               gear - 1 - changed, gear - 1, g_lastRPM, gas, brake);
                }

                g_inAutoUpdate = true;
                g_origEngineUpdate(engine, dt);
                g_inAutoUpdate = false;
                return;
            }

            g_origEngineUpdate(engine, dt);
        }

        // Every gear change of the engine goes through here. Only the local player's automatic
        // is touched: a kickdown may go more than one gear down, and the change itself is
        // quicker the harder the car has been driven.
        int __attribute__((thiscall)) Hook_Engine_ChangeGear(void* engine, int gear)
        {
            if (!g_inAutoUpdate || engine != g_cachedEngine || !g_profile.valid) {
                return g_origChangeGear(engine, gear);
            }

            const uintptr_t eng = reinterpret_cast<uintptr_t>(engine);
            const int current = *reinterpret_cast<const int*>(eng + ENGINE_GEAR_OFF);

            int target = gear;
            if (g_cfg.skipGears && gear == current - 1 && g_skipFromGear == current &&
                g_skipTarget >= 2 && g_skipTarget < gear) {
                target = g_skipTarget;
            }
            g_skipTarget = -1;

            const int done = g_origChangeGear(engine, target);
            if (done != 0 && current >= 2 && target >= 2) {
                float* timer = reinterpret_cast<float*>(eng + ENGINE_SHIFT_TIMER_OFF);
                const float stock = *timer;
                const float scale = ShiftProfile::Lerp(1.0f, g_cfg.quickShiftTime,
                                                       ShiftProfile::Clamp(g_ctrl.sportiness, 0.0f, 1.0f));
                if (stock > 0.0f && stock < 5.0f && scale < 0.999f) {
                    *timer = stock * scale;
                }
                if (g_cfg.logShifts) {
                    Log::Event(Log::Tag::Game, "[ReShifted] Change %d -> %d%s: shift time %.2fs -> %.2fs (sporty=%.2f)\n",
                               current - 1, target - 1, target != gear ? " (skipped gears)" : "",
                               stock, *timer, g_ctrl.sportiness);
                }
            }
            return done;
        }

        int __attribute__((thiscall)) Hook_Engine_GetShiftPotential(void* engine, int gear, float rpm, float wheelRPM)
        {
            void* input = nullptr;
            if (!IsLocalPlayerAutoEngine(engine, &input) || gear <= 1 || gear >= kMaxGears) {
                return g_origGetShiftPotential(engine, gear, rpm, wheelRPM);
            }

            const uintptr_t eng = reinterpret_cast<uintptr_t>(engine);
            uintptr_t engData = 0;
            if (!SafeReadPtr(eng + ENGINE_DATA_OFF, engData)) {
                return g_origGetShiftPotential(engine, gear, rpm, wheelRPM);
            }
            const int topGear = ReadTopGear(engine);

            // The profile is rebuilt only when the car or its data changes (the game refills
            // its tables on reset and after tuning), never from live driving values.
            float tables[2 * kMaxGears];
            ReadGameTables(engine, tables);
            if (!g_profile.valid || engine != g_cachedEngine || engData != g_cachedEngineData ||
                topGear != g_cachedTopGear || std::memcmp(tables, g_cachedTables, sizeof(tables)) != 0) {
                RebuildProfile(engine, engData, topGear, tables);
            }

            if (!g_profile.valid) {
                return g_origGetShiftPotential(engine, gear, rpm, wheelRPM);
            }

            float gas = 0.0f, brake = 0.0f;
            ReadPedals(input, gas, brake);

            const float wheelGuard = (gear == 2) ? g_profile.wheelGuardFirst : g_profile.wheelGuardOther;
            const int decision = g_ctrl.EvaluateShift(g_profile, g_cfg, gear, rpm, wheelRPM, wheelGuard, gas, brake);
            g_lastRPM = rpm;
            g_lastActiveTick = GetTickCount();

            if (decision == 1) {
                g_skipFromGear = gear;
                g_skipTarget = g_ctrl.ComputeDownshiftTarget(g_profile, g_cfg, gear, rpm, gas, brake);
            } else {
                g_skipTarget = -1;
            }

            if (g_cfg.logShifts && decision != 0 && decision != g_lastLoggedPotential) {
                Log::Event(Log::Tag::Game,
                           "[ReShifted] Ask %s in gear %d (to %d): rpm=%.0f wheelRpm=%.0f gas=%.2f brake=%.2f demand=%.2f sporty=%.2f upAt=%.0f downAt=%.0f\n",
                           decision == 2 ? "UP" : "DOWN",
                           gear - 1, decision == 2 ? gear : g_skipTarget - 1,
                           rpm, wheelRPM, gas, brake, g_ctrl.demand, g_ctrl.sportiness,
                           g_ctrl.lastUpshiftRPM, g_ctrl.lastDownshiftRPM);
            }
            g_lastLoggedPotential = decision;

            return decision;
        }

        void LoadSettings()
        {
            const std::string& settings = Log::SettingsPath();
            if (settings.empty()) return;

            g_cfg.enabled = GetPrivateProfileIntA("Transmission", "ReShifted", 0, settings.c_str()) != 0;

            const int shortPct = GetPrivateProfileIntA("ReShifted", "ShortShiftFraction", 28, settings.c_str());
            g_cfg.shortShiftFraction = ShiftProfile::Clamp(shortPct / 100.0f, 0.10f, 0.55f);

            const int safetyPct = GetPrivateProfileIntA("ReShifted", "SportRedlineSafety", 97, settings.c_str());
            g_cfg.sportRedlineSafety = ShiftProfile::Clamp(safetyPct / 100.0f, 0.88f, 0.99f);

            const int kickPct = GetPrivateProfileIntA("ReShifted", "KickdownAggression", 100, settings.c_str());
            g_cfg.kickdownAggression = ShiftProfile::Clamp(kickPct / 100.0f, 0.60f, 1.25f);

            const int ecoPct = GetPrivateProfileIntA("ReShifted", "EcoThrottle", 25, settings.c_str());
            g_cfg.ecoThrottle = ShiftProfile::Clamp(ecoPct / 100.0f, 0.0f, 0.80f);

            const int sportPct = GetPrivateProfileIntA("ReShifted", "SportThrottle", 85, settings.c_str());
            g_cfg.sportThrottle = ShiftProfile::Clamp(sportPct / 100.0f, g_cfg.ecoThrottle + 0.10f, 1.0f);

            g_cfg.style = (GetPrivateProfileIntA("ReShifted", "Style", 2, settings.c_str()) == 1) ? 1 : 2;
            g_cfg.coastDownshift = GetPrivateProfileIntA("ReShifted", "CoastDownshift", 1, settings.c_str()) != 0;
            g_cfg.skipGears = GetPrivateProfileIntA("ReShifted", "SkipGears", 1, settings.c_str()) != 0;

            const bool quickShift = GetPrivateProfileIntA("ReShifted", "QuickShift", 1, settings.c_str()) != 0;
            const int quickPct = GetPrivateProfileIntA("ReShifted", "QuickShiftTime", 60, settings.c_str());
            g_cfg.quickShiftTime = quickShift ? ShiftProfile::Clamp(quickPct / 100.0f, 0.30f, 1.00f) : 1.0f;

            g_cfg.sportMemory = true;
            if (g_cfg.style == 1) {
                // Classic is the first version: throttle only, one gear at a time, stock shift speed.
                g_cfg.sportMemory = false;
                g_cfg.coastDownshift = false;
                g_cfg.skipGears = false;
                g_cfg.quickShiftTime = 1.0f;
            }

            g_cfg.indicator = GetPrivateProfileIntA("ReShifted", "Indicator", 1, settings.c_str()) != 0;

            g_cfg.logShifts = GetPrivateProfileIntA("ReShifted", "Log", 0, settings.c_str()) != 0;
        }
    }

    Indicator GetIndicator()
    {
        Indicator out;
        if (!g_cfg.enabled || !g_cfg.indicator || g_lastActiveTick == 0) return out;
        if (GetTickCount() - g_lastActiveTick > 400) return out;
        out.shown = true;
        out.sport = g_ctrl.sportMode;
        out.level = g_ctrl.level;
        return out;
    }

    void Init(uintptr_t baseModule)
    {
        g_base = baseModule;
        LoadSettings();

        if (!g_cfg.enabled) {
            Log::Event(Log::Tag::Game, "ReShifted dynamic automatic: off (set [Transmission] ReShifted=1 to turn it on)\n");
            return;
        }

        const MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
            Log::Warn("[ReShifted] MH_Initialize failed (%d)\n", static_cast<int>(initStatus));
            return;
        }

        void* updateTarget = reinterpret_cast<void*>(baseModule + ENGINE_UPDATE_RVA);
        void* shiftPotTarget = reinterpret_cast<void*>(baseModule + ENGINE_SHIFT_POTENTIAL_RVA);

        const bool ok1 = MH_CreateHook(updateTarget, reinterpret_cast<void*>(Hook_Engine_Update),
                                       reinterpret_cast<void**>(&g_origEngineUpdate)) == MH_OK &&
                         MH_EnableHook(updateTarget) == MH_OK;
        const bool ok2 = MH_CreateHook(shiftPotTarget, reinterpret_cast<void*>(Hook_Engine_GetShiftPotential),
                                       reinterpret_cast<void**>(&g_origGetShiftPotential)) == MH_OK &&
                         MH_EnableHook(shiftPotTarget) == MH_OK;

        if (!ok1 || !ok2) {
            Log::Warn("[ReShifted] Failed to install hooks (update=%d shiftPot=%d)\n",
                      ok1 ? 1 : 0, ok2 ? 1 : 0);
            return;
        }

        // Optional extra: without this hook the gearbox still works, one gear at a time at stock speed.
        void* changeGearTarget = reinterpret_cast<void*>(baseModule + ENGINE_CHANGE_GEAR_RVA);
        const bool ok3 = MH_CreateHook(changeGearTarget, reinterpret_cast<void*>(Hook_Engine_ChangeGear),
                                       reinterpret_cast<void**>(&g_origChangeGear)) == MH_OK &&
                         MH_EnableHook(changeGearTarget) == MH_OK;
        if (!ok3) {
            Log::Warn("[ReShifted] Gear change hook failed; skip-gear kickdown and quick shifts are off\n");
        }

        Log::Event(Log::Tag::Game,
                   "ReShifted dynamic automatic: active (style=%s, shortShift=%.0f%%, sportSafety=%.0f%%, eco<=%.0f%%, sport>=%.0f%%, coast=%d, skipGears=%d, shiftTime=%.0f%%, indicator=%d)\n",
                   g_cfg.style == 1 ? "Classic" : "Adaptive",
                   g_cfg.shortShiftFraction * 100.0f, g_cfg.sportRedlineSafety * 100.0f,
                   g_cfg.ecoThrottle * 100.0f, g_cfg.sportThrottle * 100.0f, g_cfg.coastDownshift ? 1 : 0,
                   (ok3 && g_cfg.skipGears) ? 1 : 0, ok3 ? g_cfg.quickShiftTime * 100.0f : 100.0f,
                   g_cfg.indicator ? 1 : 0);
    }
}
