#pragma once

#include <cmath>
#include <cstdint>

namespace ReShifted
{
    static constexpr int kMaxGears = 10; // Game gear index: 0=R, 1=N, 2=1st, ... (the game's own tables hold 10)

    struct Config
    {
        bool enabled = false;
        float shortShiftFraction = 0.28f;  // Gentle-driving shift point inside (redline - idle)
        float sportRedlineSafety = 0.97f;  // Highest shift point as a share of the redline
        float kickdownAggression = 1.00f;  // Scales how eagerly high throttle downshifts
        bool logShifts = false;
    };

    // Per-car shift data, built from what the game itself computed for the car.
    struct ShiftProfile
    {
        bool valid = false;
        bool fromGameTables = false;   // false = the game's tables were unusable and fallbacks are in use
        int topGear = 6;               // Highest game gear index
        float idleRPM = 900.0f;
        float redlineRPM = 8000.0f;
        float sportRPM[kMaxGears]{};   // Full-throttle upshift RPM in gear g
        float drop[kMaxGears]{};       // RPM multiplier when shifting g -> g+1 (ratio[g+1] / ratio[g])
        float wheelGuardFirst = 0.0f;  // Game's wheelspin guard factor in 1st gear (0 = unknown, guard off)
        float wheelGuardOther = 0.0f;  // Same for the other gears

        static float Clamp(float v, float lo, float hi)
        {
            return (v < lo) ? lo : ((v > hi) ? hi : v);
        }

        static float Lerp(float a, float b, float t)
        {
            return a + (b - a) * t;
        }

        float Span() const
        {
            return (redlineRPM - idleRPM > 500.0f) ? (redlineRPM - idleRPM) : 500.0f;
        }

        // gameUp / gameDown are the game's own per-gear automatic shift tables (10 entries each).
        // The game fills them like this (0x00719550):
        //   up[g]     = RPM where the next gear starts to pull harder (g = 2 .. top-1), else redline - 100
        //   down[g+1] = up[g] * ratio[g+1] / ratio[g] * 0.85
        // so the gear spacing can be read back out of them without touching the car's attribute data.
        void Build(float idle, float redline, int top, const float* gameUp, const float* gameDown, const Config& cfg)
        {
            *this = ShiftProfile{};
            if (!(idle >= 300.0f && idle <= 4000.0f)) idle = 900.0f;
            if (!(redline >= idle + 1200.0f && redline <= 16000.0f)) redline = 7500.0f;
            idleRPM = idle;
            redlineRPM = redline;
            topGear = (top >= 3 && top < kMaxGears) ? top : 6;

            const float span = Span();
            const float ceiling = redline * Clamp(cfg.sportRedlineSafety, 0.88f, 0.99f);
            const float floorRPM = idle + span * 0.60f;
            bool allFromGame = true;

            for (int g = 2; g < kMaxGears; ++g) {
                sportRPM[g] = ceiling;
                drop[g] = (g == 2) ? 0.62f : 0.75f;
                if (g >= topGear) continue;

                const float up = gameUp ? gameUp[g] : 0.0f;
                const float down = gameDown ? gameDown[g + 1] : 0.0f;
                const bool upOk = (up >= idle + span * 0.30f && up <= redline + 1.0f);
                if (upOk) {
                    sportRPM[g] = Clamp(up, floorRPM, ceiling);
                    const float d = down / (0.85f * up);
                    if (d > 0.40f && d < 0.97f) {
                        drop[g] = d;
                    } else {
                        allFromGame = false;
                    }
                } else {
                    allFromGame = false;
                }
            }
            fromGameTables = allFromGame;
            valid = true;
        }
    };

    // Shift decision state machine. No game access, so it can be reasoned about on its own.
    class Controller
    {
    public:
        // Time after a gear change in which the opposite shift is refused. The engine RPM
        // is still settling then, which is what caused the old up-then-straight-down pair.
        static constexpr float kShiftLockSec = 0.8f;

        void Reset()
        {
            demand = 0.0f;
            sportiness = 0.0f;
            prevGas = 0.0f;
            holdTimer = 0.0f;
            lastGear = -1;
            lastShiftDir = 0;
            sinceShift = 10.0f;
            lastUpshiftRPM = 0.0f;
            lastDownshiftRPM = 0.0f;
        }

        // Call once per simulation step with the real throttle and brake (0..1) and the current gear.
        // Returns the gear change that just happened: +1 up, -1 down, 0 none.
        int Tick(float dt, int gear, float rawGas, float rawBrake)
        {
            if (dt <= 0.0f) return 0;
            if (dt > 0.05f) dt = 0.05f;

            const float gas = ShiftProfile::Clamp(rawGas, 0.0f, 1.0f);
            const float brake = ShiftProfile::Clamp(rawBrake, 0.0f, 1.0f);
            const float dGasDt = (gas - prevGas) / dt;

            // Demand follows the throttle up quickly and falls back slowly, so lifting for a
            // moment in a hard-driven section does not drop straight into short-shifting.
            const float tau = (gas >= demand) ? 0.08f : 0.90f;
            demand += (gas - demand) * (1.0f - expf(-dt / tau));

            // Sportiness is the slow memory of how hard the car has been driven lately:
            // about 2 s of hard driving to build up, about 8 s of calm driving to fade.
            const float sportTau = (demand >= sportiness) ? 2.0f : 8.0f;
            sportiness += (demand - sportiness) * (1.0f - expf(-dt / sportTau));

            // Braking, or snapping the throttle shut from hard driving, means a corner is coming:
            // hold the gear for a moment instead of short-shifting into it.
            if (brake > 0.12f) {
                holdTimer = 1.0f;
            } else if (demand > 0.60f && gas < 0.15f && dGasDt < -1.5f) {
                holdTimer = 0.8f;
            } else if (holdTimer > 0.0f) {
                holdTimer -= dt;
                if (holdTimer < 0.0f) holdTimer = 0.0f;
            }
            prevGas = gas;

            int changed = 0;
            if (gear != lastGear) {
                if (lastGear >= 2 && gear >= 2) {
                    changed = (gear > lastGear) ? 1 : -1;
                    lastShiftDir = changed;
                    sinceShift = 0.0f;
                }
                lastGear = gear;
            } else if (sinceShift < 10.0f) {
                sinceShift += dt;
            }
            return changed;
        }

        // RPM at which gear g shifts up: low when driving gently, the car's sport point when driving hard.
        float ComputeUpshiftRPM(const ShiftProfile& p, const Config& cfg, int gear, float rawGas) const
        {
            if (gear < 2 || gear >= kMaxGears) return p.redlineRPM;
            const float span = p.Span();
            const float sport = p.sportRPM[gear];
            if (holdTimer > 0.0f) return sport;

            // Short-shift point. The next gear must still land clearly above idle.
            float shortRPM = p.idleRPM + span * ShiftProfile::Clamp(cfg.shortShiftFraction, 0.10f, 0.55f);
            const float minLanding = p.idleRPM + span * 0.10f;
            if (shortRPM * p.drop[gear] < minLanding) shortRPM = minLanding / p.drop[gear];
            if (shortRPM > sport * 0.80f) shortRPM = sport * 0.80f;

            // Up to 25% throttle: short-shift. From 85%: full sport point. Smooth blend between.
            // Recent hard driving keeps the shift point up even while the throttle is closed,
            // so a gear picked while coasting into a corner is not thrown away on the way out.
            const float gas = ShiftProfile::Clamp(rawGas, 0.0f, 1.0f);
            float eff = (gas > demand) ? gas : demand;
            if (sportiness > eff) eff = sportiness;
            const float t = ShiftProfile::Clamp((eff - 0.25f) / 0.60f, 0.0f, 1.0f);
            return ShiftProfile::Lerp(shortRPM, sport, t * t * (3.0f - 2.0f * t));
        }

        // RPM below which gear g shifts down.
        float ComputeDownshiftRPM(const ShiftProfile& p, const Config& cfg, int gear,
                                  float rawGas, float rawBrake) const
        {
            if (gear <= 2 || gear >= kMaxGears) return 0.0f;

            const float span = p.Span();
            const float d = p.drop[gear - 1];           // After the downshift the RPM is (rpm / d)
            const float lowerSport = p.sportRPM[gear - 1];
            const float gas = ShiftProfile::Clamp(rawGas, 0.0f, 1.0f);
            const float brake = ShiftProfile::Clamp(rawBrake, 0.0f, 1.0f);
            const float eff = (gas > demand) ? gas : demand;

            // Coasting or light throttle: only drop a gear when the engine is close to lugging.
            const float lug = p.idleRPM + span * 0.06f;

            // Kickdown: with a lot of throttle, drop a gear as soon as the lower gear would
            // land at or below ~80% of its own sport shift point.
            const float kickLanding = ShiftProfile::Clamp(0.80f * cfg.kickdownAggression, 0.55f, 0.90f);
            const float k = ShiftProfile::Clamp((eff - 0.45f) / 0.45f, 0.0f, 1.0f);
            float target = ShiftProfile::Lerp(lug, lowerSport * kickLanding * d, k * k);

            // Coasting (off the throttle, slowing down): shift down as the revs fall instead of
            // holding the gear to near idle. After calm driving the lower gear lands low in the
            // rev range; after hard driving it lands at up to 60% of its sport point, so the
            // car is in the right gear when the throttle comes back.
            const float coast = ShiftProfile::Lerp(p.idleRPM + span * 0.16f, lowerSport * 0.60f * d, sportiness);
            if (coast > target) target = coast;

            // Hysteresis: never drop a gear if the lower gear would be at (or within 18% of)
            // its own upshift point, because it would shift straight back up. The same 18%
            // is the gap between where an upshift lands and where this threshold sits.
            const float huntCap = ComputeUpshiftRPM(p, cfg, gear - 1, gas) * 0.82f * d;
            if (target > huntCap) target = huntCap;

            // Braking: shift down for engine braking, landing at up to 70% of the lower gear's
            // sport point. Braking also starts the hold above, so no upshift follows it.
            if (brake > 0.15f) {
                const float brakeTarget = ShiftProfile::Lerp(lug, lowerSport * 0.70f * d, brake);
                if (brakeTarget > target) target = brakeTarget;
            }

            return (target > lug) ? target : lug;
        }

        // Returns 0 = stay, 1 = shift down, 2 = shift up (the game's own shift-potential values).
        // wheelRPM is the game's speed-based RPM; wheelGuard is the game's wheelspin factor
        // for this gear (0 = unknown, guard skipped).
        int EvaluateShift(const ShiftProfile& p, const Config& cfg, int gear, float currentRPM,
                          float wheelRPM, float wheelGuard, float rawGas, float rawBrake)
        {
            if (!p.valid || gear <= 1 || gear >= kMaxGears) {
                return 0;
            }

            const float upshiftRPM = ComputeUpshiftRPM(p, cfg, gear, rawGas);
            const float downshiftRPM = ComputeDownshiftRPM(p, cfg, gear, rawGas, rawBrake);
            lastUpshiftRPM = upshiftRPM;
            lastDownshiftRPM = downshiftRPM;

            const float limiterRPM = p.redlineRPM * ShiftProfile::Clamp(cfg.sportRedlineSafety, 0.88f, 0.99f);
            const bool atLimiter = currentRPM >= limiterRPM;

            if (gear < p.topGear && rawBrake < 0.02f && currentRPM >= upshiftRPM) {
                const bool pedalOk = rawGas > 0.03f || atLimiter;
                const bool lockedOut = (lastShiftDir < 0 && sinceShift < kShiftLockSec && !atLimiter);
                // Same idea as the stock gearbox: no upshift while the wheels spin far ahead of the road speed.
                const bool gripOk = !(wheelGuard > 0.0f && wheelGuard <= 1.0f) ||
                                    wheelRPM >= wheelGuard * upshiftRPM;
                if (pedalOk && !lockedOut && gripOk) {
                    return 2;
                }
            }

            if (gear > 2 && currentRPM <= downshiftRPM) {
                const bool lockedOut = (lastShiftDir > 0 && sinceShift < kShiftLockSec);
                if (!lockedOut) {
                    return 1;
                }
            }

            return 0;
        }

        float demand = 0.0f;
        float sportiness = 0.0f;
        float prevGas = 0.0f;
        float holdTimer = 0.0f;
        int lastGear = -1;
        int lastShiftDir = 0;
        float sinceShift = 10.0f;
        float lastUpshiftRPM = 0.0f;
        float lastDownshiftRPM = 0.0f;
    };

    void Init(uintptr_t baseModule);
}
