#include "game/display_test.h"

#include <windows.h>
#include <d3d9.h>

#include <string>

#include "log.h"
#include "game/d3d_hooks.h"

namespace DisplayTest
{
    static const DWORD SUMMARY_INTERVAL_MS = 10000;
    static const int MAX_DEVICES = 8;

    // What each device did since the last summary.
    struct DeviceStats {
        IDirect3DDevice9* device = nullptr;
        int presents = 0;
        int chainPresents = 0;
        int endScenes = 0;
        int targetNotBackBuffer = 0; // presents where the game had something else bound
        int clearFailed = 0;
        int drawFailed = 0;
        int resets = 0;
        UINT width = 0, height = 0;
        D3DFORMAT format = D3DFMT_UNKNOWN;
        BOOL windowed = FALSE;
    };
    static DeviceStats g_devices[MAX_DEVICES];
    static int g_deviceCount = 0;
    static int g_devicesCreated = 0;
    static DWORD g_lastSummary = 0;
    static CRITICAL_SECTION g_lock;

    static DeviceStats& StatsFor(IDirect3DDevice9* device)
    {
        for (int i = 0; i < g_deviceCount; ++i)
            if (g_devices[i].device == device) return g_devices[i];
        if (g_deviceCount < MAX_DEVICES) {
            g_devices[g_deviceCount].device = device;
            return g_devices[g_deviceCount++];
        }
        return g_devices[MAX_DEVICES - 1];
    }

    static void Summary()
    {
        const DWORD now = GetTickCount();
        if (now - g_lastSummary < SUMMARY_INTERVAL_MS) return;
        g_lastSummary = now;
        Log::Event(Log::Tag::Client, "Display test: %d device(s) made, %d presenting\n", g_devicesCreated, g_deviceCount);
        for (int i = 0; i < g_deviceCount; ++i) {
            DeviceStats& s = g_devices[i];
            Log::Event(Log::Tag::Client,
                       "Display test: device %d: %d presents, %d swap chain presents, %d EndScenes, back buffer %ux%u format %u %s, "
                       "other target bound %d, clear failed %d, draw failed %d, resets %d\n",
                       i + 1, s.presents, s.chainPresents, s.endScenes, s.width, s.height, static_cast<unsigned>(s.format),
                       s.windowed ? "windowed" : "fullscreen", s.targetNotBackBuffer, s.clearFailed, s.drawFailed, s.resets);
            const IDirect3DDevice9* keep = s.device;
            const UINT w = s.width, h = s.height;
            const D3DFORMAT f = s.format;
            const BOOL win = s.windowed;
            s = DeviceStats();
            s.device = const_cast<IDirect3DDevice9*>(keep);
            s.width = w; s.height = h; s.format = f; s.windowed = win;
        }
    }

    struct Vertex {
        float x, y, z, rhw;
        D3DCOLOR color;
    };

    // The red and blue squares, on the surface that's about to be shown.
    static void DrawOnBackBuffer(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer, DeviceStats& stats)
    {
        D3DSURFACE_DESC desc;
        if (SUCCEEDED(backBuffer->GetDesc(&desc))) {
            stats.width = desc.Width;
            stats.height = desc.Height;
            stats.format = desc.Format;
        }

        IDirect3DSurface9* oldTarget = nullptr;
        IDirect3DSurface9* oldDepth = nullptr;
        D3DVIEWPORT9 oldViewport;
        device->GetRenderTarget(0, &oldTarget);
        device->GetDepthStencilSurface(&oldDepth);
        device->GetViewport(&oldViewport);
        if (oldTarget != backBuffer) ++stats.targetNotBackBuffer;

        // Every state the triangles touch goes back afterwards.
        IDirect3DStateBlock9* saved = nullptr;
        device->CreateStateBlock(D3DSBT_ALL, &saved);

        device->SetRenderTarget(0, backBuffer); // also sets the viewport to the whole surface
        device->SetDepthStencilSurface(nullptr);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);

        const D3DRECT red = {40, 40, 240, 140};
        if (FAILED(device->Clear(1, &red, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 0), 1.0f, 0))) ++stats.clearFailed;

        bool drawn = false;
        if (SUCCEEDED(device->BeginScene())) {
            device->SetVertexShader(nullptr);
            device->SetPixelShader(nullptr);
            device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            device->SetTexture(0, nullptr);
            device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
            device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            device->SetRenderState(D3DRS_LIGHTING, FALSE);
            device->SetRenderState(D3DRS_FOGENABLE, FALSE);
            device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
            device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
            device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
            device->SetRenderState(D3DRS_CLIPPING, TRUE);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            const D3DCOLOR blue = D3DCOLOR_XRGB(0, 80, 255);
            const Vertex quad[4] = {
                {460.0f, 40.0f, 0.0f, 1.0f, blue}, {660.0f, 40.0f, 0.0f, 1.0f, blue},
                {460.0f, 140.0f, 0.0f, 1.0f, blue}, {660.0f, 140.0f, 0.0f, 1.0f, blue},
            };
            drawn = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex)));
            device->EndScene();
        }
        if (!drawn) ++stats.drawFailed;

        if (saved) {
            saved->Apply();
            saved->Release();
        }
        device->SetRenderTarget(0, oldTarget);
        device->SetDepthStencilSurface(oldDepth);
        device->SetViewport(&oldViewport);
        if (oldTarget) oldTarget->Release();
        if (oldDepth) oldDepth->Release();
    }

    static void BeforePresent(IDirect3DDevice9* device, IDirect3DSwapChain9* chain)
    {
        EnterCriticalSection(&g_lock);
        DeviceStats& stats = StatsFor(device);
        IDirect3DSurface9* backBuffer = nullptr;
        const HRESULT got = chain ? chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)
                                  : device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (SUCCEEDED(got) && backBuffer) {
            IDirect3DSwapChain9* own = chain;
            if (own || SUCCEEDED(device->GetSwapChain(0, &own))) {
                D3DPRESENT_PARAMETERS parameters;
                if (own && SUCCEEDED(own->GetPresentParameters(&parameters))) stats.windowed = parameters.Windowed;
                if (!chain && own) own->Release();
            }
            DrawOnBackBuffer(device, backBuffer, stats);
            backBuffer->Release();
        } else {
            ++stats.clearFailed;
        }
        Summary();
        LeaveCriticalSection(&g_lock);
    }

    static void OnPresent(IDirect3DDevice9* device, IDirect3DSwapChain9* chain)
    {
        DeviceStats& stats = StatsFor(device);
        if (chain) ++stats.chainPresents; else ++stats.presents;
        BeforePresent(device, chain);
    }

    // The green square: on whatever the game drew this scene to.
    static void OnEndScene(IDirect3DDevice9* device)
    {
        ++StatsFor(device).endScenes;
        IDirect3DSurface9* target = nullptr;
        if (FAILED(device->GetRenderTarget(0, &target)) || !target) return;
        D3DVIEWPORT9 oldViewport;
        device->GetViewport(&oldViewport);
        DWORD scissor = FALSE;
        device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
        D3DSURFACE_DESC desc;
        if (SUCCEEDED(target->GetDesc(&desc)) && desc.Width >= 460 && desc.Height >= 140) {
            D3DVIEWPORT9 whole = {0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
            device->SetViewport(&whole);
            device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            const D3DRECT green = {250, 40, 450, 140};
            device->Clear(1, &green, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 200, 0), 1.0f, 0);
            device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor);
            device->SetViewport(&oldViewport);
        }
        target->Release();
    }

    static void OnReset(IDirect3DDevice9* device)
    {
        ++StatsFor(device).resets;
    }

    static void OnDeviceCreated(IDirect3DDevice9*, HWND, const D3DPRESENT_PARAMETERS* parameters)
    {
        ++g_devicesCreated;
        if (parameters)
            Log::Event(Log::Tag::Client, "Display test: device %d made: %ux%u, format %u, %s, %u back buffer(s), swap effect %u\n",
                       g_devicesCreated, parameters->BackBufferWidth, parameters->BackBufferHeight, static_cast<unsigned>(parameters->BackBufferFormat),
                       parameters->Windowed ? "windowed" : "fullscreen", parameters->BackBufferCount, static_cast<unsigned>(parameters->SwapEffect));
    }

    void Init(uintptr_t)
    {
        const std::string& settings = Log::SettingsPath();
        if (settings.empty() || GetPrivateProfileIntA("Debug", "DisplayTest", 0, settings.c_str()) == 0) return;

        InitializeCriticalSection(&g_lock);
        if (!D3DHooks::Install()) {
            Log::Warn("Display test is off: the display couldn't be hooked.\n");
            return;
        }
        D3DHooks::Listener listener;
        listener.deviceCreated = OnDeviceCreated;
        listener.beforeReset = OnReset;
        listener.beforePresent = OnPresent;
        listener.beforeEndScene = OnEndScene;
        D3DHooks::AddListener(listener);
        HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
        Log::Event(Log::Tag::Client, "Display test is on, drawing through %s: red, green and blue squares in the top left show which ways of drawing reach the screen\n",
                   D3DHooks::ModuleOf(reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9"))));
    }
}
