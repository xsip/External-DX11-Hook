#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <HeadlessRenderer/Definitions.h>

class RenderObjectManager;

struct SharedOverlayHandshake {
    HANDLE  sharedTextureHandle;
    UINT    width;
    UINT    height;
    DWORD   producerPid;
};

class OVERLAY_API HeadlessRenderer {
private:
    static void ResolveResolution(HWND hwnd, UINT fallbackW, UINT fallbackH);
public:
    static void Start(RenderObjectManager* renderer, HWND hwnd, UINT fallbackWidth, UINT fallbackHeight,
        const char* smName = "Local\\LiquidOverlayHandshake");

    static void Stop();

    static bool IsReady();
    static void HandleResize();
    static void Resize(UINT newW, UINT newH);

private:
    static void RenderThread();
    static bool CreateDevice();
    static bool CreateSharedTexture(UINT w, UINT h);
    static void PublishHandshake(const char* smName);
    static bool RebuildSharedResources(UINT w, UINT h, const char* smName = "Local\\LiquidOverlayHandshake");
    static void FeedInput();
    static void Cleanup();

    static ID3D11Device* s_device;
    static ID3D11DeviceContext* s_ctx;
    static ID3D11Texture2D* s_sharedTex;
    static ID3D11RenderTargetView* s_rtv;
    static IDXGIKeyedMutex* s_mutex;

    static HANDLE                   s_hMapFile;
    static SharedOverlayHandshake* s_handshake;
    static HWND s_targetHwnd;
    static RenderObjectManager* s_renderObjectManager;
    static UINT                     s_width;
    static UINT                     s_height;
    static volatile bool            s_running;
    static volatile bool            s_ready;
    static HANDLE                   s_thread;
    static volatile bool  s_resizePending;
    static UINT           s_pendingW;
    static UINT           s_pendingH;
    inline static char s_channelName[128] = "";
    static bool bHandleResize;

    static void         ScrollHookThread();
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);

    static HANDLE       s_scrollThread;
    static HHOOK        s_mouseHook;
    static volatile LONG s_wheelAccum;
};