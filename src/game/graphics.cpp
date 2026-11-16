#include "game/graphics.h"

#include "game/d3d_hooks.h"
#include "log.h"
#include "minhook/MinHook.h"

#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace Graphics
{
    // Vtable indices, the same in every Direct3D 9.
    static const size_t DEVICE_SET_SAMPLER_STATE = 69;
    static const size_t DEVICE_CREATE_VERTEX_SHADER = 91;
    static const size_t DEVICE_CREATE_PIXEL_SHADER = 106;

    // No shader in the game comes near this: a longer walk means the bytecode isn't what it seems.
    static const size_t MAX_SHADER_BYTES = 1 << 20;

    typedef HRESULT (WINAPI *SetSamplerStateFn)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
    typedef HRESULT (WINAPI *CreateVertexShaderFn)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
    typedef HRESULT (WINAPI *CreatePixelShaderFn)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);

    static SetSamplerStateFn g_realSetSamplerState = nullptr;
    static CreateVertexShaderFn g_realCreateVertexShader = nullptr;
    static CreatePixelShaderFn g_realCreatePixelShader = nullptr;

    static DWORD g_anisotropy = 0; // off unless [Graphics] Anisotropy asks for it
    static bool g_shaders = true;
    static bool g_log = false;
    static bool g_hooked = false;

    // Replacement bytecode by the id of the game's shader it replaces.
    static std::map<uint64_t, std::vector<DWORD>> g_replacements;
    static std::mutex g_seenLock;
    static std::set<uint64_t> g_seen;

    // The length of a shader in bytes, up to and including its end token. 0 when it can't be read.
    static size_t ShaderBytes(const DWORD* code)
    {
        if (!code) return 0;
        const DWORD major = (code[0] >> 8) & 0xFF;
        size_t at = 1;
        while (at * 4 < MAX_SHADER_BYTES) {
            const DWORD token = code[at];
            if (token == 0x0000FFFF) return (at + 1) * 4;
            if ((token & 0xFFFF) == 0xFFFE) {
                at += 1 + ((token >> 16) & 0x7FFF);     // a comment, which holds its length
            } else if (major >= 2) {
                at += 1 + ((token >> 24) & 0xF);        // an instruction, which holds its length
            } else {
                // Shader model 1 has no length in the token: the operands all have the top bit set.
                ++at;
                while (at * 4 < MAX_SHADER_BYTES && (code[at] >> 31) == 1) ++at;
            }
        }
        return 0;
    }

    // FNV-1a over the bytecode. split-shaders.js computes the same id.
    static uint64_t ShaderId(const DWORD* code, size_t bytes)
    {
        const unsigned char* data = reinterpret_cast<const unsigned char*>(code);
        uint64_t hash = 14695981039346656037ULL;
        for (size_t i = 0; i < bytes; ++i) {
            hash ^= data[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    // The game's shader, or its replacement when there is one.
    static const DWORD* Choose(const DWORD* code, const char* kind)
    {
        const size_t bytes = ShaderBytes(code);
        if (!bytes) return code;
        const uint64_t id = ShaderId(code, bytes);
        const std::map<uint64_t, std::vector<DWORD>>::const_iterator found = g_replacements.find(id);
        if (g_log) {
            std::lock_guard<std::mutex> lock(g_seenLock);
            if (g_seen.insert(id).second)
                Log::Debug("[OpenProStreet] Graphics: %s shader %016llx, model %u.%u, %u bytes%s\n", kind,
                           static_cast<unsigned long long>(id), static_cast<unsigned>((code[0] >> 8) & 0xFF),
                           static_cast<unsigned>(code[0] & 0xFF), static_cast<unsigned>(bytes),
                           found != g_replacements.end() ? ", replaced" : "");
        }
        return found != g_replacements.end() ? found->second.data() : code;
    }

    static HRESULT WINAPI CreateVertexShaderHook(IDirect3DDevice9* device, const DWORD* code, IDirect3DVertexShader9** shader)
    {
        const DWORD* chosen = Choose(code, "vertex");
        if (chosen != code) {
            // A replacement Direct3D refuses must not take the game down with it.
            const HRESULT result = g_realCreateVertexShader(device, chosen, shader);
            if (SUCCEEDED(result)) return result;
            Log::Warn("A replacement vertex shader was refused (0x%08lX). The game's own is used.\n", static_cast<unsigned long>(result));
        }
        return g_realCreateVertexShader(device, code, shader);
    }

    static HRESULT WINAPI CreatePixelShaderHook(IDirect3DDevice9* device, const DWORD* code, IDirect3DPixelShader9** shader)
    {
        const DWORD* chosen = Choose(code, "pixel");
        if (chosen != code) {
            const HRESULT result = g_realCreatePixelShader(device, chosen, shader);
            if (SUCCEEDED(result)) return result;
            Log::Warn("A replacement pixel shader was refused (0x%08lX). The game's own is used.\n", static_cast<unsigned long>(result));
        }
        return g_realCreatePixelShader(device, code, shader);
    }

    static HRESULT WINAPI SetSamplerStateHook(IDirect3DDevice9* device, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
    {
        if (g_anisotropy > 1) {
            if (type == D3DSAMP_MAXANISOTROPY) {
                value = g_anisotropy;
            } else if (type == D3DSAMP_MINFILTER && value == D3DTEXF_LINEAR) {
                // Point-filtered samplers are left alone: the game picks those on purpose.
                g_realSetSamplerState(device, sampler, D3DSAMP_MAXANISOTROPY, g_anisotropy);
                value = D3DTEXF_ANISOTROPIC;
            }
        }
        return g_realSetSamplerState(device, sampler, type, value);
    }

    static bool Hook(void* target, void* detour, void** original, const char* name)
    {
        if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
            Log::Warn("Couldn't hook %s, so part of the graphics improvements is off.\n", name);
            return false;
        }
        return true;
    }

    static void DeviceCreated(IDirect3DDevice9* device, HWND, const D3DPRESENT_PARAMETERS*)
    {
        if (g_hooked) return;
        g_hooked = true;

        D3DCAPS9 caps;
        std::memset(&caps, 0, sizeof(caps));
        if (g_anisotropy > 1 && SUCCEEDED(device->GetDeviceCaps(&caps))) {
            const bool able = (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) != 0 && caps.MaxAnisotropy > 1;
            if (!able) g_anisotropy = 0;
            else if (g_anisotropy > caps.MaxAnisotropy) g_anisotropy = caps.MaxAnisotropy;
        }

        void** vtable = *reinterpret_cast<void***>(device);
        if (g_anisotropy > 1)
            Hook(vtable[DEVICE_SET_SAMPLER_STATE], reinterpret_cast<void*>(SetSamplerStateHook),
                 reinterpret_cast<void**>(&g_realSetSamplerState), "IDirect3DDevice9::SetSamplerState");
        if (g_log || !g_replacements.empty()) {
            Hook(vtable[DEVICE_CREATE_VERTEX_SHADER], reinterpret_cast<void*>(CreateVertexShaderHook),
                 reinterpret_cast<void**>(&g_realCreateVertexShader), "IDirect3DDevice9::CreateVertexShader");
            Hook(vtable[DEVICE_CREATE_PIXEL_SHADER], reinterpret_cast<void*>(CreatePixelShaderHook),
                 reinterpret_cast<void**>(&g_realCreatePixelShader), "IDirect3DDevice9::CreatePixelShader");
        }
        Log::Debug("[OpenProStreet] Graphics: anisotropic filtering %ux, %u replacement shader(s)\n",
                   static_cast<unsigned>(g_anisotropy), static_cast<unsigned>(g_replacements.size()));
    }

    // The id in "<anything>-<16 hex characters>.cso". False when the name doesn't end that way.
    static bool IdFromName(const char* name, uint64_t* id)
    {
        const size_t length = std::strlen(name);
        if (length < 21 || _stricmp(name + length - 4, ".cso") != 0 || name[length - 21] != '-') return false;
        uint64_t value = 0;
        for (size_t i = length - 20; i < length - 4; ++i) {
            const char c = name[i];
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return false;
            value = (value << 4) | static_cast<uint64_t>(digit);
        }
        *id = value;
        return true;
    }

    static void LoadReplacements(const std::string& folder)
    {
        WIN32_FIND_DATAA entry;
        const HANDLE search = FindFirstFileA((folder + "\\*.cso").c_str(), &entry);
        if (search == INVALID_HANDLE_VALUE) return;
        do {
            uint64_t id = 0;
            if (!IdFromName(entry.cFileName, &id)) {
                Log::Warn("Shader file %s is skipped: its name must end in -<id>.cso.\n", entry.cFileName);
                continue;
            }
            FILE* file = std::fopen((folder + "\\" + entry.cFileName).c_str(), "rb");
            if (!file) continue;
            std::vector<DWORD> code;
            DWORD token;
            while (code.size() * 4 < MAX_SHADER_BYTES && std::fread(&token, sizeof(token), 1, file) == 1) code.push_back(token);
            std::fclose(file);
            // Only whole shaders go in: one cut short would make Direct3D read past its end.
            if (code.size() < 2 || ShaderBytes(code.data()) != code.size() * 4) {
                Log::Warn("Shader file %s is skipped: it isn't a compiled Direct3D 9 shader.\n", entry.cFileName);
                continue;
            }
            g_replacements[id].swap(code);
        } while (FindNextFileA(search, &entry));
        FindClose(search);
    }

    void Init(uintptr_t)
    {
        const std::string& settings = Log::SettingsPath();
        if (!settings.empty()) {
            const int level = GetPrivateProfileIntA("Graphics", "Anisotropy", 0, settings.c_str());
            g_anisotropy = level < 2 ? 0 : (level > 16 ? 16 : static_cast<DWORD>(level));
            g_shaders = GetPrivateProfileIntA("Graphics", "Shaders", 1, settings.c_str()) != 0;
            g_log = GetPrivateProfileIntA("Graphics", "Log", 0, settings.c_str()) != 0;
            const size_t slash = settings.find_last_of("\\/");
            if (g_shaders && slash != std::string::npos) LoadReplacements(settings.substr(0, slash) + "\\OpenProStreetShaders");
        }
        if (g_anisotropy < 2 && !g_log && g_replacements.empty()) return;
        if (!D3DHooks::Install()) {
            Log::Warn("The graphics improvements are off: Direct3D couldn't be hooked.\n");
            return;
        }
        D3DHooks::Listener listener;
        listener.deviceCreated = DeviceCreated;
        D3DHooks::AddListener(listener);
    }
}
