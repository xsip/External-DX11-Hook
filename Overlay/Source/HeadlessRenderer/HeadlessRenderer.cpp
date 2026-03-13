#include <HeadlessRenderer/Definitions.h>
#include <HeadlessRenderer/HeadlessRenderer.h>
#include <HeadlessRenderer/RenderObjectManager.h>
#include <HeadlessRenderer/LiquidMenu.h>
#include <imgui/imgui.h>
#include <imgui/backends/imgui_impl_dx11.h>

#include <cstdio>
#include <thread>

ID3D11Device* HeadlessRenderer::s_device = nullptr;
ID3D11DeviceContext* HeadlessRenderer::s_ctx = nullptr;
ID3D11Texture2D* HeadlessRenderer::s_sharedTex = nullptr;
ID3D11RenderTargetView* HeadlessRenderer::s_rtv = nullptr;
IDXGIKeyedMutex* HeadlessRenderer::s_mutex = nullptr;

HANDLE                  HeadlessRenderer::s_hMapFile = nullptr;
SharedOverlayHandshake* HeadlessRenderer::s_handshake = nullptr;

RenderObjectManager* HeadlessRenderer::s_renderObjectManager = nullptr;
UINT                    HeadlessRenderer::s_width = 0;
UINT                    HeadlessRenderer::s_height = 0;
volatile bool           HeadlessRenderer::s_running = false;
volatile bool           HeadlessRenderer::s_ready = false;
HANDLE                  HeadlessRenderer::s_thread = nullptr;
HWND HeadlessRenderer::s_targetHwnd = nullptr;

volatile bool HeadlessRenderer::s_resizePending = false;
UINT          HeadlessRenderer::s_pendingW = 0;
UINT          HeadlessRenderer::s_pendingH = 0;
bool HeadlessRenderer::bHandleResize = false; // currently causing crashes!

HANDLE          HeadlessRenderer::s_scrollThread = nullptr;
HHOOK           HeadlessRenderer::s_mouseHook = nullptr;
volatile LONG   HeadlessRenderer::s_wheelAccum = 0;


void SetupLiquidStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.FramePadding = ImVec2(8, 6);
    style.ItemSpacing = ImVec2(10, 8);

    ImVec4* colors = style.Colors;

    colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.09f, 1.0f);

    colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.12f, 0.15f, 1);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.12f, 0.15f, 1);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.10f, 0.12f, 0.15f, 1);

    colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.14f, 0.18f, 1);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.18f, 0.23f, 1);

    colors[ImGuiCol_Button] = ImVec4(0.15f, 0.18f, 0.22f, 1);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.23f, 0.40f, 0.70f, 1);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.90f, 0.35f, 0.15f, 1);

    colors[ImGuiCol_Tab] = ImVec4(0.10f, 0.12f, 0.16f, 1);
    colors[ImGuiCol_TabHovered] = ImVec4(0.20f, 0.45f, 0.80f, 1);
    colors[ImGuiCol_TabActive] = ImVec4(0.90f, 0.40f, 0.20f, 1);

    colors[ImGuiCol_CheckMark] = ImVec4(0.30f, 0.65f, 1.0f, 1);

    colors[ImGuiCol_SliderGrab] = ImVec4(0.25f, 0.55f, 1.0f, 1);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.45f, 0.20f, 1);
}


bool HeadlessRenderer::RebuildSharedResources(UINT w, UINT h, const char* smName) {
    if (HeadlessRenderer::s_mutex) { HeadlessRenderer::s_mutex->Release();     HeadlessRenderer::s_mutex = nullptr; }
    if (HeadlessRenderer::s_rtv) { HeadlessRenderer::s_rtv->Release();       HeadlessRenderer::s_rtv = nullptr; }
    if (HeadlessRenderer::s_sharedTex) { HeadlessRenderer::s_sharedTex->Release(); HeadlessRenderer::s_sharedTex = nullptr; }

    if (HeadlessRenderer::s_handshake) { UnmapViewOfFile(HeadlessRenderer::s_handshake); HeadlessRenderer::s_handshake = nullptr; }
    if (HeadlessRenderer::s_hMapFile) { CloseHandle(HeadlessRenderer::s_hMapFile);      HeadlessRenderer::s_hMapFile = nullptr; }

    HeadlessRenderer::s_width = w;
    HeadlessRenderer::s_height = h;

    if (!HeadlessRenderer::CreateSharedTexture(w, h)) return false;

    HeadlessRenderer::PublishHandshake(smName);
    return true;
}

void HeadlessRenderer::ResolveResolution(HWND hwnd, UINT fallbackW, UINT fallbackH) {
    if (hwnd) {
        RECT rc{};
        if (GetClientRect(hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
            s_width = static_cast<UINT>(rc.right);
            s_height = static_cast<UINT>(rc.bottom);
            printf("[HeadlessRenderer] Resolution from HWND: %ux%u\n", s_width, s_height);
            return;
        }
        printf("[HeadlessRenderer] GetClientRect failed (err=%lu) — using fallback\n", GetLastError());
    }
    s_width = fallbackW;
    s_height = fallbackH;
    printf("[HeadlessRenderer] Resolution (fallback): %ux%u\n", s_width, s_height);
}


void HeadlessRenderer::Start(RenderObjectManager* renderObjectManager, HWND hwnd, UINT fallbackWidth, UINT fallbackHeight, const char* smName) {
    if (s_running) return;
    s_targetHwnd = hwnd;
    s_renderObjectManager = renderObjectManager;
    strncpy_s(s_channelName, smName, sizeof(s_channelName) - 1);
    ResolveResolution(hwnd, fallbackWidth, fallbackHeight);

    s_running = true;

    s_scrollThread = CreateThread(
        nullptr, 0,
        [](LPVOID) -> DWORD { HeadlessRenderer::ScrollHookThread(); return 0; },
        nullptr, 0, nullptr
    );
    static char s_smName[128];
    strncpy_s(s_smName, smName, sizeof(s_smName) - 1);

    s_thread = CreateThread(
        nullptr, 0,
        [](LPVOID param) -> DWORD {
            if (!HeadlessRenderer::CreateDevice()) {
                printf("[HeadlessRenderer] Device creation failed — aborting\n");
                HeadlessRenderer::s_running = false;
                return 1;
            }
            if (!HeadlessRenderer::CreateSharedTexture(HeadlessRenderer::s_width,
                HeadlessRenderer::s_height)) {
                printf("[HeadlessRenderer] Shared texture creation failed — aborting\n");
                HeadlessRenderer::s_running = false;
                return 1;
            }

            HeadlessRenderer::PublishHandshake(reinterpret_cast<const char*>(param));

            HeadlessRenderer::RenderThread();
            return 0;
        },
        reinterpret_cast<LPVOID>(s_smName), 0, nullptr
    );

    std::thread([]() {
        while (true) {
            HeadlessRenderer::HandleResize();
            Sleep(1);
        }
        }).detach();
}

void HeadlessRenderer::Stop() {
    s_running = false;
    if (s_scrollThread) {
        WaitForSingleObject(s_scrollThread, 1000);
        CloseHandle(s_scrollThread);
        s_scrollThread = nullptr;
    }
    if (s_thread) {
        WaitForSingleObject(s_thread, 3000);
        CloseHandle(s_thread);
        s_thread = nullptr;
    }
    Cleanup();
}

bool HeadlessRenderer::IsReady() {
    return s_ready;
}


void HeadlessRenderer::Resize(UINT newW, UINT newH) {
    if (newW == s_width && newH == s_height) return;
    s_pendingW = newW;
    s_pendingH = newH;
    s_resizePending = true;
}


bool HeadlessRenderer::CreateDevice() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        featureLevels, ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &s_device, &featureLevel, &s_ctx
    );

    if (FAILED(hr)) {
        printf("[HeadlessRenderer] D3D11CreateDevice failed: 0x%08X\n", hr);
        return false;
    }

    IDXGIDevice1* dxgiDev = nullptr;
    if (SUCCEEDED(s_device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDev))) {
        dxgiDev->SetGPUThreadPriority(7);
        dxgiDev->Release();
    }

    printf("[HeadlessRenderer] D3D11 device created (FL %u.%u)\n",
        (featureLevel >> 12) & 0xF, (featureLevel >> 8) & 0xF);
    return true;
}

bool HeadlessRenderer::CreateSharedTexture(UINT w, UINT h) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc = { 1, 0 };
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    HRESULT hr = s_device->CreateTexture2D(&desc, nullptr, &s_sharedTex);
    if (FAILED(hr)) {
        printf("[HeadlessRenderer] CreateTexture2D failed: 0x%08X\n", hr);
        return false;
    }

    hr = s_device->CreateRenderTargetView(s_sharedTex, nullptr, &s_rtv);
    if (FAILED(hr)) {
        printf("[HeadlessRenderer] CreateRenderTargetView failed: 0x%08X\n", hr);
        return false;
    }

    hr = s_sharedTex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&s_mutex);
    if (FAILED(hr)) {
        printf("[HeadlessRenderer] QueryInterface IDXGIKeyedMutex failed: 0x%08X\n", hr);
        return false;
    }

    printf("[HeadlessRenderer] Shared texture created (%ux%u)\n", w, h);
    return true;
}

void HeadlessRenderer::PublishHandshake(const char* smName) {
    IDXGIResource* dxgiRes = nullptr;
    if (FAILED(s_sharedTex->QueryInterface(__uuidof(IDXGIResource), (void**)&dxgiRes))) {
        printf("[HeadlessRenderer] QueryInterface IDXGIResource failed\n");
        return;
    }

    HANDLE rawHandle = nullptr;
    dxgiRes->GetSharedHandle(&rawHandle);
    dxgiRes->Release();

    s_hMapFile = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, sizeof(SharedOverlayHandshake),
        smName
    );

    if (!s_hMapFile) {
        printf("[HeadlessRenderer] CreateFileMapping failed: %lu\n", GetLastError());
        return;
    }

    s_handshake = reinterpret_cast<SharedOverlayHandshake*>(
        MapViewOfFile(s_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedOverlayHandshake))
        );

    if (!s_handshake) {
        printf("[HeadlessRenderer] MapViewOfFile failed: %lu\n", GetLastError());
        return;
    }

    s_handshake->sharedTextureHandle = rawHandle;
    s_handshake->width = s_width;
    s_handshake->height = s_height;
    s_handshake->producerPid = GetCurrentProcessId();

    printf("[HeadlessRenderer] Handshake published to \"%s\" (handle=0x%p, pid=%lu)\n",
        smName, rawHandle, s_handshake->producerPid);
}

LRESULT CALLBACK HeadlessRenderer::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL)
    {
        const MSLLHOOKSTRUCT* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        const SHORT delta = static_cast<SHORT>(HIWORD(ms->mouseData));
        InterlockedAdd(reinterpret_cast<volatile LONG*>(&s_wheelAccum), static_cast<LONG>(delta));
    }
    return CallNextHookEx(s_mouseHook, nCode, wParam, lParam);
}

void HeadlessRenderer::ScrollHookThread()
{
    s_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, nullptr, 0);
    if (!s_mouseHook)
    {
        printf("[HeadlessRenderer] WH_MOUSE_LL install failed: %lu\n", GetLastError());
        return;
    }
    printf("[HeadlessRenderer] WH_MOUSE_LL installed\n");

    MSG msg{};
    while (s_running)
    {
        const DWORD waitResult = MsgWaitForMultipleObjectsEx(
            0, nullptr, 100, QS_ALLINPUT, MWMO_ALERTABLE);

        if (waitResult == WAIT_OBJECT_0)
        {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    UnhookWindowsHookEx(s_mouseHook);
    s_mouseHook = nullptr;
    printf("[HeadlessRenderer] WH_MOUSE_LL removed\n");
}

void HeadlessRenderer::FeedInput() {
    ImGuiIO& io = ImGui::GetIO();

    io.AppFocusLost = false;
    io.AddFocusEvent(true);

    POINT p;
    GetCursorPos(&p);
    if (s_targetHwnd && ScreenToClient(s_targetHwnd, &p)) {
        p.x = max(0L, min(p.x, static_cast<LONG>(s_width - 1)));
        p.y = max(0L, min(p.y, static_cast<LONG>(s_height - 1)));
    }
    io.MousePos = ImVec2(static_cast<float>(p.x), static_cast<float>(p.y));

    io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    io.MouseDown[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    io.MouseDown[2] = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;

    const LONG rawDelta = InterlockedExchange(
        reinterpret_cast<volatile LONG*>(&s_wheelAccum), 0L);
    if (rawDelta != 0)
        io.MouseWheel += static_cast<float>(rawDelta) / static_cast<float>(WHEEL_DELTA);

    static ULONGLONG lastTick = GetTickCount64();
    ULONGLONG now = GetTickCount64();
    io.DeltaTime = static_cast<float>(now - lastTick) / 1000.0f;
    if (io.DeltaTime <= 0.0f)
        io.DeltaTime = 1.0f / 1000.0f;
    lastTick = now;
}

void HeadlessRenderer::HandleResize() {
    if (HeadlessRenderer::IsReady() && bHandleResize) {
        RECT rc{};
        if (s_targetHwnd && GetClientRect(s_targetHwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
            HeadlessRenderer::Resize(
                static_cast<UINT>(rc.right),
                static_cast<UINT>(rc.bottom)
            );
        }
    }
}

void HeadlessRenderer::RenderThread() {
    printf("[HeadlessRenderer] Render thread starting\n");

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)s_width, (float)s_height);
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    SetupLiquidStyle();
    ImGui::GetStyle().Alpha = 1.0f;
    ImGui_ImplDX11_Init(s_device, s_ctx);

    constexpr DWORD PRODUCER_KEY = 0;
    constexpr DWORD CONSUMER_KEY = 1;

    s_mutex->ReleaseSync(PRODUCER_KEY);

    s_ready = true;
    printf("[HeadlessRenderer] Ready — entering render loop\n");

    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    while (s_running) {

        if (s_resizePending && bHandleResize) {
            s_resizePending = false;
            printf("[HeadlessRenderer] Resize: %ux%u -> %ux%u\n",
                s_width, s_height, s_pendingW, s_pendingH);

            ImGui::GetIO().DisplaySize = ImVec2((float)s_pendingW, (float)s_pendingH);

            RebuildSharedResources(s_pendingW, s_pendingH, s_channelName);
        }

        HRESULT hr = s_mutex->AcquireSync(PRODUCER_KEY, 32);
        if (hr != S_OK) {
            continue;
        }

        s_ctx->ClearRenderTargetView(s_rtv, clearColor);
        s_ctx->OMSetRenderTargets(1, &s_rtv, nullptr);

        D3D11_VIEWPORT vp = {};
        vp.Width = (float)s_width;
        vp.Height = (float)s_height;
        vp.MaxDepth = 1.0f;
        s_ctx->RSSetViewports(1, &vp);

        FeedInput();

        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();

        if (s_renderObjectManager) {
            
            bool mouseReleased = !(GetAsyncKeyState(VK_LBUTTON) & 0x8000);
            RenderLiquidMenu();
            s_renderObjectManager->Render(io.DisplaySize, io.MousePos, mouseReleased);
        }

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        s_ctx->Flush();

        s_mutex->ReleaseSync(CONSUMER_KEY);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    printf("[HeadlessRenderer] Render thread exiting\n");
}

void HeadlessRenderer::Cleanup() {
    s_ready = false;

    if (s_handshake) { UnmapViewOfFile(s_handshake);    s_handshake = nullptr; }
    if (s_hMapFile) { CloseHandle(s_hMapFile);          s_hMapFile = nullptr; }
    if (s_mutex) { s_mutex->Release();               s_mutex = nullptr; }
    if (s_rtv) { s_rtv->Release();                 s_rtv = nullptr; }
    if (s_sharedTex) { s_sharedTex->Release();           s_sharedTex = nullptr; }
    if (s_ctx) { s_ctx->Release();                 s_ctx = nullptr; }
    if (s_device) { s_device->Release();              s_device = nullptr; }
}