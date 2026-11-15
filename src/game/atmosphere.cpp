#include "game/atmosphere.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/d3d_hooks.h"
#include "game/overlay.h"
#include "log.h"
#include "minhook/MinHook.h"

namespace Atmosphere
{
    // ---- What D3DX 9 needs, without its headers ----

    typedef const char* Handle; // D3DXHANDLE

    struct ParameterDesc // D3DXPARAMETER_DESC
    {
        LPCSTR Name;
        LPCSTR Semantic;
        DWORD  Class;  // D3DXPARAMETER_CLASS
        DWORD  Type;   // D3DXPARAMETER_TYPE
        UINT   Rows;
        UINT   Columns;
        UINT   Elements;
        UINT   Annotations;
        UINT   StructMembers;
        DWORD  Flags;
        UINT   Bytes;
    };
    static const DWORD CLASS_SCALAR = 0, CLASS_VECTOR = 1; // D3DXPC_SCALAR, D3DXPC_VECTOR
    static const DWORD TYPE_FLOAT = 3;                      // D3DXPT_FLOAT

    // ID3DXEffect's vtable, from ID3DXBaseEffect in d3dx9effect.h. The same since D3DX 9 of 2004.
    static const size_t EFFECT_RELEASE = 2;
    static const size_t EFFECT_GET_PARAMETER_DESC = 4;
    static const size_t EFFECT_SET_VALUE = 20;
    static const size_t EFFECT_SET_FLOAT = 30;
    static const size_t EFFECT_SET_FLOAT_ARRAY = 32;
    static const size_t EFFECT_SET_VECTOR = 34;
    static const size_t EFFECT_SET_VECTOR_ARRAY = 36;
    // ID3DXEffect's own, after ID3DXBaseEffect's 57. nfs.exe sets nearly every shader value with it
    // (call [vtable+138h], as at 0x7017FB), not with the typed setters above.
    static const size_t EFFECT_SET_RAW_VALUE = 78;

    typedef ULONG   (WINAPI *ReleaseFn)(void* effect);
    typedef HRESULT (WINAPI *GetParameterDescFn)(void* effect, Handle parameter, ParameterDesc* desc);
    typedef HRESULT (WINAPI *SetValueFn)(void* effect, Handle parameter, const void* data, UINT bytes);
    typedef HRESULT (WINAPI *SetFloatFn)(void* effect, Handle parameter, float value);
    typedef HRESULT (WINAPI *SetFloatArrayFn)(void* effect, Handle parameter, const float* values, UINT count);
    typedef HRESULT (WINAPI *SetVectorFn)(void* effect, Handle parameter, const float* vector4);
    typedef HRESULT (WINAPI *SetVectorArrayFn)(void* effect, Handle parameter, const float* vector4s, UINT count);
    typedef HRESULT (WINAPI *SetRawValueFn)(void* effect, Handle parameter, const void* data, UINT byteOffset, UINT bytes);

    // D3DXCreateEffectFromResourceA and D3DXCreateEffect take the same nine arguments, the effect
    // eighth. D3DXCreateEffectEx has a tenth, so its effect is ninth.
    typedef HRESULT (WINAPI *CreateEffect9Fn)(void*, void*, void*, void*, void*, DWORD, void*, void** effect, void**);
    typedef HRESULT (WINAPI *CreateEffectExFn)(void*, void*, void*, void*, void*, void*, DWORD, void*, void** effect, void**);

    static CreateEffect9Fn  g_realFromResource = nullptr;
    static CreateEffect9Fn  g_realCreate = nullptr;
    static CreateEffectExFn g_realCreateEx = nullptr;
    static ReleaseFn          g_realRelease = nullptr;
    static GetParameterDescFn g_getParameterDesc = nullptr;
    static SetValueFn         g_realSetValue = nullptr;
    static SetFloatFn         g_realSetFloat = nullptr;
    static SetFloatArrayFn    g_realSetFloatArray = nullptr;
    static SetVectorFn        g_realSetVector = nullptr;
    static SetVectorArrayFn   g_realSetVectorArray = nullptr;
    static SetRawValueFn      g_realSetRawValue = nullptr;

    // ---- Presets ----

    struct Tint
    {
        bool  set = false;
        float value[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    };

    static const char* const PRESET_NAMES[3] = {"Day", "Dusk", "Night"};
    static std::vector<std::string> g_names;  // every parameter any preset changes
    static std::vector<Tint> g_tints[3];       // by preset, then by g_names index
    static std::atomic<int> g_preset(Day);
    static bool g_enabled = true;
    static bool g_log = false;
    static int  g_key = VK_F8;
    static bool g_effectHooked = false;

    // Which g_names entry each effect's parameter handle is, or -1: asking the effect for a name on
    // every call would cost too much, as the game sets thousands of values a frame.
    static std::unordered_map<void*, std::unordered_map<Handle, int>> g_slots;
    static std::set<std::string> g_logged; // effect|parameter, each listed once
    static std::unordered_map<void*, std::string> g_effectNames;
    static CRITICAL_SECTION g_lock; // the game may free effects off its render thread

    struct Guard
    {
        Guard() { EnterCriticalSection(&g_lock); }
        ~Guard() { LeaveCriticalSection(&g_lock); }
    };

    struct Default
    {
        const char* name;
        float dusk[4];
        float night[4];
    };

    // Tuned from the first test in the game: warm and low at dusk, dim and blue at night. The world's
    // baked light isn't touched, as its parameters' meaning isn't known yet. The car has lights of its
    // own: cmLocalColourMatrix holds their colours, a row each, and cvLightColour and cvSpecularColour
    // light its paint.
    static const Default DEFAULTS[] = {
        {"cvWorldSunColour",         {1.00f, 0.62f, 0.40f, 1.0f}, {0.09f, 0.12f, 0.24f, 1.0f}},
        {"cvInstanceWorldSunColour", {1.00f, 0.62f, 0.40f, 1.0f}, {0.09f, 0.12f, 0.24f, 1.0f}},
        {"cvWorldAmbientColour",     {0.80f, 0.66f, 0.62f, 1.0f}, {0.20f, 0.24f, 0.38f, 1.0f}},
        {"cvAmbientColour",          {0.80f, 0.66f, 0.62f, 1.0f}, {0.20f, 0.24f, 0.38f, 1.0f}},
        {"cvFogColour",              {0.95f, 0.60f, 0.45f, 1.0f}, {0.12f, 0.14f, 0.24f, 1.0f}},
        {"FogColor",                 {0.95f, 0.60f, 0.45f, 1.0f}, {0.12f, 0.14f, 0.24f, 1.0f}},
        {"FFSKYCOLOR",               {0.95f, 0.62f, 0.48f, 1.0f}, {0.15f, 0.18f, 0.30f, 1.0f}},
        {"cfBloomScale",             {1.00f, 1.00f, 1.00f, 1.0f}, {0.60f, 1.00f, 1.00f, 1.0f}},
        {"cmLocalColourMatrix",      {1.00f, 0.70f, 0.50f, 1.0f}, {0.20f, 0.24f, 0.40f, 1.0f}},
        {"cvLightColour",            {1.00f, 0.70f, 0.50f, 1.0f}, {0.20f, 0.24f, 0.40f, 1.0f}},
        {"cvSpecularColour",         {1.00f, 0.75f, 0.60f, 1.0f}, {0.35f, 0.40f, 0.60f, 1.0f}},
        {"cfBrightness",             {0.85f, 1.00f, 1.00f, 1.0f}, {0.55f, 1.00f, 1.00f, 1.0f}},
        // The sky (fx\sky.efx) is its texture brightened by its alpha, times cvVertexPowerBrightness.y,
        // then tone-mapped as c / (c + 1). Only y matters there.
        {"IDI_SKY_FX:cvVertexPowerBrightness", {1.00f, 0.70f, 1.00f, 1.0f}, {1.00f, 0.07f, 1.00f, 1.0f}},
    };

    static int NameIndex(const std::string& name)
    {
        for (size_t i = 0; i < g_names.size(); ++i)
            if (g_names[i] == name) return static_cast<int>(i);
        g_names.push_back(name);
        for (auto& tints : g_tints) tints.resize(g_names.size());
        return static_cast<int>(g_names.size() - 1);
    }

    static void SetTint(int preset, const std::string& name, const float value[4])
    {
        Tint& tint = g_tints[preset][NameIndex(name)];
        tint.set = true;
        memcpy(tint.value, value, sizeof(tint.value));
    }

    // "name=x,y,z[,w]" lines of an [Atmosphere.Dusk] or [Atmosphere.Night] section. An empty value
    // turns the default off.
    static void ReadPresetSection(int preset, const std::string& settings)
    {
        char buffer[8192] = {};
        const std::string section = std::string("Atmosphere.") + PRESET_NAMES[preset];
        const DWORD length = GetPrivateProfileSectionA(section.c_str(), buffer, sizeof(buffer), settings.c_str());
        for (const char* line = buffer; line < buffer + length && *line; line += strlen(line) + 1) {
            const char* equals = strchr(line, '=');
            if (!equals || equals == line || _strnicmp(line, "SkyStyle=", 9) == 0) continue;
            std::string name(line, equals - line);
            while (!name.empty() && name.back() == ' ') name.pop_back();
            float value[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            int read = sscanf(equals + 1, " %f , %f , %f , %f", &value[0], &value[1], &value[2], &value[3]);
            if (read <= 0) {
                g_tints[preset][NameIndex(name)].set = false;
                continue;
            }
            // One number tints every component alike.
            if (read == 1) value[1] = value[2] = value[0];
            SetTint(preset, name, value);
        }
    }

    static int ParseKey(const char* text, int fallback)
    {
        if ((text[0] == 'F' || text[0] == 'f') && text[1]) {
            const int number = atoi(text + 1);
            if (number >= 1 && number <= 12) return VK_F1 + number - 1;
        }
        return fallback;
    }

    // ---- The effect hooks ----

    // -1 for a parameter no preset changes.
    static int SlotOf(void* effect, Handle parameter, const float* values, UINT count, const char* how)
    {
        Guard guard;
        auto& slots = g_slots[effect];
        auto found = slots.find(parameter);
        if (found != slots.end()) return found->second;

        int slot = -1;
        ParameterDesc desc = {};
        if (g_getParameterDesc && SUCCEEDED(g_getParameterDesc(effect, parameter, &desc)) && desc.Name &&
            desc.Type == TYPE_FLOAT && (desc.Class == CLASS_SCALAR || desc.Class == CLASS_VECTOR)) {
            const std::string name = desc.Name;
            const auto effectName = g_effectNames.find(effect);
            const std::string where = effectName != g_effectNames.end() ? effectName->second : std::string("an effect");
            // "EFFECT:parameter" applies in that effect only, and wins over a rule for the parameter
            // everywhere. The sky's brightness is cvVertexPowerBrightness, which the world uses too.
            const std::string scoped = where + ":" + name;
            for (size_t i = 0; i < g_names.size(); ++i)
                if (g_names[i] == name && slot < 0) slot = static_cast<int>(i);
            for (size_t i = 0; i < g_names.size(); ++i)
                if (g_names[i] == scoped) slot = static_cast<int>(i);
            if (g_log && values && g_logged.insert(where + "|" + name).second) {
                char text[160] = {};
                int used = 0;
                for (UINT i = 0; i < count && i < 8 && used < static_cast<int>(sizeof(text)) - 16; ++i)
                    used += snprintf(text + used, sizeof(text) - used, i ? ",%.3f" : "%.3f", values[i]);
                Log::Event(Log::Tag::Client, "Atmosphere: %s in %s = %s (%s, %u value(s))%s\n", name.c_str(), where.c_str(), text, how, count,
                           slot >= 0 ? " [tinted]" : "");
            }
        }
        slots[parameter] = slot;
        return slot;
    }

    // Tints count floats in place, four components to a vector: x, y, z and w.
    static void Apply(int slot, float* values, UINT count, bool scalar)
    {
        const Tint& tint = g_tints[g_preset.load()][slot];
        if (!tint.set) return;
        for (UINT i = 0; i < count; ++i) values[i] *= scalar ? tint.value[0] : tint.value[i % 4];
    }

    static const UINT MAX_FLOATS = 64;

    static HRESULT WINAPI SetValueHook(void* effect, Handle parameter, const void* data, UINT bytes)
    {
        const UINT count = bytes / sizeof(float);
        const int slot = data && bytes % sizeof(float) == 0 && count <= MAX_FLOATS
            ? SlotOf(effect, parameter, static_cast<const float*>(data), count, "SetValue") : -1;
        if (slot < 0 || g_preset.load() == Day) return g_realSetValue(effect, parameter, data, bytes);
        float values[MAX_FLOATS];
        memcpy(values, data, bytes);
        Apply(slot, values, count, count == 1);
        return g_realSetValue(effect, parameter, values, bytes);
    }

    static HRESULT WINAPI SetFloatHook(void* effect, Handle parameter, float value)
    {
        const int slot = SlotOf(effect, parameter, &value, 1, "SetFloat");
        if (slot >= 0 && g_preset.load() != Day) Apply(slot, &value, 1, true);
        return g_realSetFloat(effect, parameter, value);
    }

    static HRESULT WINAPI SetFloatArrayHook(void* effect, Handle parameter, const float* data, UINT count)
    {
        const int slot = data && count <= MAX_FLOATS ? SlotOf(effect, parameter, data, count, "SetFloatArray") : -1;
        if (slot < 0 || g_preset.load() == Day) return g_realSetFloatArray(effect, parameter, data, count);
        float values[MAX_FLOATS];
        memcpy(values, data, count * sizeof(float));
        Apply(slot, values, count, count == 1);
        return g_realSetFloatArray(effect, parameter, values, count);
    }

    static HRESULT WINAPI SetVectorHook(void* effect, Handle parameter, const float* vector)
    {
        const int slot = vector ? SlotOf(effect, parameter, vector, 4, "SetVector") : -1;
        if (slot < 0 || g_preset.load() == Day) return g_realSetVector(effect, parameter, vector);
        float values[4];
        memcpy(values, vector, sizeof(values));
        Apply(slot, values, 4, false);
        return g_realSetVector(effect, parameter, values);
    }

    static HRESULT WINAPI SetVectorArrayHook(void* effect, Handle parameter, const float* vectors, UINT count)
    {
        const int slot = vectors && count * 4 <= MAX_FLOATS ? SlotOf(effect, parameter, vectors, count * 4, "SetVectorArray") : -1;
        if (slot < 0 || g_preset.load() == Day) return g_realSetVectorArray(effect, parameter, vectors, count);
        float values[MAX_FLOATS];
        memcpy(values, vectors, count * 4 * sizeof(float));
        Apply(slot, values, count * 4, false);
        return g_realSetVectorArray(effect, parameter, values, count);
    }

    // Raw bytes into the parameter, from byteOffset. Only whole vectors from a vector boundary are
    // tinted, so each float still lines up with its x, y, z or w.
    static HRESULT WINAPI SetRawValueHook(void* effect, Handle parameter, const void* data, UINT byteOffset, UINT bytes)
    {
        const UINT count = bytes / sizeof(float);
        const int slot = data && byteOffset % 16 == 0 && bytes % sizeof(float) == 0 && count <= MAX_FLOATS
            ? SlotOf(effect, parameter, static_cast<const float*>(data), count, "SetRawValue") : -1;
        if (slot < 0 || g_preset.load() == Day) return g_realSetRawValue(effect, parameter, data, byteOffset, bytes);
        float values[MAX_FLOATS];
        memcpy(values, data, bytes);
        Apply(slot, values, count, count == 1);
        return g_realSetRawValue(effect, parameter, values, byteOffset, bytes);
    }

    // A freed effect's address can come back as another effect, with other handles.
    static ULONG WINAPI ReleaseHook(void* effect)
    {
        const ULONG left = g_realRelease(effect);
        if (left == 0) {
            Guard guard;
            g_slots.erase(effect);
            g_effectNames.erase(effect);
        }
        return left;
    }

    static bool HookAt(void** vtable, size_t index, void* detour, void** original, const char* name)
    {
        if (MH_CreateHook(vtable[index], detour, original) != MH_OK || MH_EnableHook(vtable[index]) != MH_OK) {
            Log::Warn("Couldn't hook ID3DXEffect::%s, so day and night can't change the game's light.\n", name);
            return false;
        }
        return true;
    }

    // Every effect shares D3DX's code, so hooking the first one's methods covers them all.
    static void HookEffect(void* effect)
    {
        if (g_effectHooked || !effect) return;
        g_effectHooked = true;
        void** vtable = *static_cast<void***>(effect);
        g_getParameterDesc = reinterpret_cast<GetParameterDescFn>(vtable[EFFECT_GET_PARAMETER_DESC]);
        const bool ok =
            HookAt(vtable, EFFECT_RELEASE, reinterpret_cast<void*>(ReleaseHook), reinterpret_cast<void**>(&g_realRelease), "Release") &&
            HookAt(vtable, EFFECT_SET_VALUE, reinterpret_cast<void*>(SetValueHook), reinterpret_cast<void**>(&g_realSetValue), "SetValue") &&
            HookAt(vtable, EFFECT_SET_FLOAT, reinterpret_cast<void*>(SetFloatHook), reinterpret_cast<void**>(&g_realSetFloat), "SetFloat") &&
            HookAt(vtable, EFFECT_SET_FLOAT_ARRAY, reinterpret_cast<void*>(SetFloatArrayHook), reinterpret_cast<void**>(&g_realSetFloatArray), "SetFloatArray") &&
            HookAt(vtable, EFFECT_SET_VECTOR, reinterpret_cast<void*>(SetVectorHook), reinterpret_cast<void**>(&g_realSetVector), "SetVector") &&
            HookAt(vtable, EFFECT_SET_VECTOR_ARRAY, reinterpret_cast<void*>(SetVectorArrayHook), reinterpret_cast<void**>(&g_realSetVectorArray), "SetVectorArray") &&
            HookAt(vtable, EFFECT_SET_RAW_VALUE, reinterpret_cast<void*>(SetRawValueHook), reinterpret_cast<void**>(&g_realSetRawValue), "SetRawValue");
        if (ok) Log::Debug("[OpenProStreet] Atmosphere: hooked the game's effects in %s\n", D3DHooks::ModuleOf(vtable[EFFECT_SET_VECTOR]));
    }

    // Which effect is which, for the log. FromResourceA may make its effect through D3DXCreateEffectEx,
    // so the outer call's name is set last and wins.
    static void NameEffect(void* effect, const std::string& name)
    {
        if (!effect) return;
        Guard guard;
        g_effectNames[effect] = name;
    }

    static HRESULT WINAPI FromResourceHook(void* a, void* b, void* c, void* d, void* e, DWORD flags, void* pool, void** effect, void** errors)
    {
        const HRESULT result = g_realFromResource(a, b, c, d, e, flags, pool, effect, errors);
        if (SUCCEEDED(result) && effect && *effect) {
            HookEffect(*effect);
            char name[64] = {};
            const uintptr_t resource = reinterpret_cast<uintptr_t>(c);
            if (resource >> 16) snprintf(name, sizeof(name), "%s", static_cast<const char*>(c));
            else snprintf(name, sizeof(name), "#%u", static_cast<unsigned>(resource));
            NameEffect(*effect, name);
        }
        return result;
    }

    // nfs.exe keeps its compiled effects as RCDATA resources named like IDI_SKY_FX, and may hand their
    // bytes to D3DXCreateEffect itself. An effect made from memory is named after the resource its bytes
    // came from.
    struct EffectResource
    {
        std::string name;
        const void* data;
        DWORD       size;
    };
    static std::vector<EffectResource> g_resources;

    static BOOL CALLBACK OnResource(HMODULE module, LPCSTR type, LPSTR name, LONG_PTR)
    {
        if (IS_INTRESOURCE(name)) return TRUE;
        HRSRC found = FindResourceA(module, name, type);
        HGLOBAL loaded = found ? LoadResource(module, found) : nullptr;
        const void* data = loaded ? LockResource(loaded) : nullptr;
        if (data) g_resources.push_back({name, data, SizeofResource(module, found)});
        return TRUE;
    }

    static std::string NameFromData(const void* data, size_t size)
    {
        for (const EffectResource& resource : g_resources)
            if (resource.data == data) return resource.name;
        // The game may have copied the bytes first.
        for (const EffectResource& resource : g_resources)
            if (resource.size == size && data && memcmp(resource.data, data, size) == 0) return resource.name;
        return "an effect from memory";
    }

    static HRESULT WINAPI CreateHook(void* a, void* b, void* c, void* d, void* e, DWORD flags, void* pool, void** effect, void** errors)
    {
        const HRESULT result = g_realCreate(a, b, c, d, e, flags, pool, effect, errors);
        if (SUCCEEDED(result) && effect && *effect) {
            HookEffect(*effect);
            NameEffect(*effect, NameFromData(b, reinterpret_cast<uintptr_t>(c)));
        }
        return result;
    }

    static HRESULT WINAPI CreateExHook(void* a, void* b, void* c, void* d, void* e, void* skip, DWORD flags, void* pool, void** effect, void** errors)
    {
        const HRESULT result = g_realCreateEx(a, b, c, d, e, skip, flags, pool, effect, errors);
        if (SUCCEEDED(result) && effect && *effect) {
            HookEffect(*effect);
            NameEffect(*effect, NameFromData(b, reinterpret_cast<uintptr_t>(c)));
        }
        return result;
    }

    static bool HookExport(HMODULE module, const char* name, void* detour, void** original)
    {
        void* target = reinterpret_cast<void*>(GetProcAddress(module, name));
        if (!target) return false;
        if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
            Log::Warn("Couldn't hook %s.\n", name);
            return false;
        }
        return true;
    }

    // ---- Switching ----

    static bool GameHasFocus()
    {
        DWORD process = 0;
        HWND window = GetForegroundWindow();
        if (window) GetWindowThreadProcessId(window, &process);
        return process == GetCurrentProcessId();
    }

    // ---- The sky's texture set ----
    //
    // Each track names one of 15 sky styles, SKY_STYLE_01 to SKY_STYLE_15 (listed in
    // GLOBAL\DYNTEX_timestamp.moo). Track models use SKY_STYLE1_LAYER1 to 4 and an envmap, and 0x754700
    // remaps those to SKY_STYLE_<style>_LAYERn. 0x76A670 switches style at any time: it stores the style
    // at 0xA79F9C and the envmap's at 0xA79FA0, rebuilds the remap and streams the textures in. The game
    // calls it from 0x7765F7 with the track's attribute 0x295014E6 and a float kept at 0xAEB744.
    typedef void (__cdecl *SetSkyStyleFn)(int style, int envmap, float value);
    static const uintptr_t SET_SKY_STYLE = 0x76A670;
    static const int       SKY_STYLES = 15;

    static SetSkyStyleFn g_realSetSkyStyle = nullptr;
    static std::atomic<int> g_skyOverride(0); // 0: the track's own sky
    static int   g_skyKey = VK_F9;
    static int   g_presetSky[3] = {0, 0, 0};

    // The game asks for its track's sky as it loads: a chosen style replaces it.
    static void __cdecl SetSkyStyleHook(int style, int envmap, float value)
    {
        const int sky = g_skyOverride.load();
        if (sky > 0) {
            Log::Event(Log::Tag::Client, "Atmosphere: sky style %d instead of the track's %d\n", sky, style);
            g_realSetSkyStyle(sky, sky, value);
        } else {
            g_realSetSkyStyle(style, envmap, value);
        }
    }

    // The sky only changes when the game switches it itself, as a track loads. Calling the switch from
    // a frame crashed the game (client 10.5 test build 167): it frees the sky textures while they're in
    // use, and the game only ever calls it during a load.
    static void SetSky(int sky, bool announce)
    {
        g_skyOverride.store(sky);
        if (!announce) return;
        char title[32];
        snprintf(title, sizeof(title), sky > 0 ? "Sky style %d" : "The track's own sky", sky);
        std::vector<std::string> lines;
        lines.push_back("One of the game's 15 skies. It shows from the next race.");
        Overlay::PushNotice("result", title, lines);
    }

    static void BeforePresent(IDirect3DDevice9*, IDirect3DSwapChain9*)
    {
        static bool wasDown = false, skyWasDown = false;
        const bool focus = GameHasFocus();
        const bool down = focus && (GetAsyncKeyState(g_key) & 0x8000) != 0;
        if (down && !wasDown) Set(static_cast<Preset>((g_preset.load() + 1) % 3), true);
        wasDown = down;
        const bool skyDown = focus && g_realSetSkyStyle && (GetAsyncKeyState(g_skyKey) & 0x8000) != 0;
        if (skyDown && !skyWasDown) SetSky((g_skyOverride.load() + 1) % (SKY_STYLES + 1), true);
        skyWasDown = skyDown;
    }

    Preset Current()
    {
        return static_cast<Preset>(g_preset.load());
    }

    void Set(Preset preset, bool announce)
    {
        if (preset < Day || preset > Night) return;
        g_preset.store(preset);
        g_skyOverride.store(g_presetSky[preset]);
        Log::Event(Log::Tag::Client, "Atmosphere: %s\n", PRESET_NAMES[preset]);
        if (!announce) return;
        std::vector<std::string> lines;
        lines.push_back(preset == Day ? "The game's own light." : preset == Dusk ? "A low, warm sun." : "A dim, blue night.");
        lines.push_back("The next key press changes it.");
        Overlay::PushNotice("result", PRESET_NAMES[preset], lines);
    }

    void Init(uintptr_t)
    {
        InitializeCriticalSection(&g_lock);
        EnumResourceNamesA(GetModuleHandleA(nullptr), MAKEINTRESOURCEA(10), OnResource, 0); // RT_RCDATA
        Log::Debug("[OpenProStreet] Atmosphere: %u effect resource(s) in nfs.exe\n", static_cast<unsigned>(g_resources.size()));
        const std::string& settings = Log::SettingsPath();
        for (const Default& entry : DEFAULTS) {
            SetTint(Dusk, entry.name, entry.dusk);
            SetTint(Night, entry.name, entry.night);
        }
        if (!settings.empty()) {
            g_enabled = GetPrivateProfileIntA("Atmosphere", "Enabled", 1, settings.c_str()) != 0;
            g_log = GetPrivateProfileIntA("Atmosphere", "Log", 0, settings.c_str()) != 0;
            char text[32] = {};
            GetPrivateProfileStringA("Atmosphere", "Key", "F8", text, sizeof(text), settings.c_str());
            g_key = ParseKey(text, VK_F8);
            GetPrivateProfileStringA("Atmosphere", "SkyKey", "F9", text, sizeof(text), settings.c_str());
            g_skyKey = ParseKey(text, VK_F9);
            GetPrivateProfileStringA("Atmosphere", "Start", "Day", text, sizeof(text), settings.c_str());
            for (int preset = Day; preset <= Night; ++preset)
                if (_stricmp(text, PRESET_NAMES[preset]) == 0) g_preset.store(preset);
            ReadPresetSection(Dusk, settings);
            ReadPresetSection(Night, settings);
            // SkyStyle=1 to 15 swaps in that sky for the look. 0 keeps the track's own.
            for (int preset = Dusk; preset <= Night; ++preset) {
                const std::string section = std::string("Atmosphere.") + PRESET_NAMES[preset];
                const int sky = GetPrivateProfileIntA(section.c_str(), "SkyStyle", 0, settings.c_str());
                g_presetSky[preset] = sky >= 0 && sky <= SKY_STYLES ? sky : 0;
            }
        }
        if (!g_enabled) return;

        // The game's own sky switch, so a chosen style also replaces the track's when a track loads.
        const MH_STATUS minhook = MH_Initialize();
        if ((minhook != MH_OK && minhook != MH_ERROR_ALREADY_INITIALIZED) ||
            MH_CreateHook(reinterpret_cast<void*>(SET_SKY_STYLE), reinterpret_cast<void*>(SetSkyStyleHook),
                          reinterpret_cast<void**>(&g_realSetSkyStyle)) != MH_OK ||
            MH_EnableHook(reinterpret_cast<void*>(SET_SKY_STYLE)) != MH_OK) {
            g_realSetSkyStyle = nullptr;
            Log::Warn("Couldn't hook the game's sky switch, so the sky style can't change.\n");
        }
        g_skyOverride.store(g_presetSky[g_preset.load()]);

        HMODULE d3dx = GetModuleHandleA("d3dx9_34.dll");
        if (!d3dx) d3dx = LoadLibraryA("d3dx9_34.dll");
        const MH_STATUS started = MH_Initialize();
        if (!d3dx || (started != MH_OK && started != MH_ERROR_ALREADY_INITIALIZED)) {
            Log::Warn("Day and night are off: d3dx9_34.dll couldn't be hooked.\n");
            return;
        }
        const bool hooked =
            HookExport(d3dx, "D3DXCreateEffectFromResourceA", reinterpret_cast<void*>(FromResourceHook), reinterpret_cast<void**>(&g_realFromResource)) |
            HookExport(d3dx, "D3DXCreateEffect", reinterpret_cast<void*>(CreateHook), reinterpret_cast<void**>(&g_realCreate)) |
            HookExport(d3dx, "D3DXCreateEffectEx", reinterpret_cast<void*>(CreateExHook), reinterpret_cast<void**>(&g_realCreateEx));
        if (!hooked) {
            Log::Warn("Day and night are off: d3dx9_34.dll couldn't be hooked.\n");
            return;
        }
        if (D3DHooks::Install()) {
            D3DHooks::Listener listener;
            listener.beforePresent = BeforePresent;
            D3DHooks::AddListener(listener);
        }
        Log::Debug("[OpenProStreet] Atmosphere ready: %s, key F%d\n", PRESET_NAMES[g_preset.load()], g_key - VK_F1 + 1);
    }
}
