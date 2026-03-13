#pragma once
#include <LiquidHookEx/Config.h>
#include <LiquidHookEx/Include.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

namespace LiquidHookEx {

    typedef HANDLE(__stdcall* OpenFileMappingAFn)  (DWORD, BOOL, LPCSTR);
    typedef LPVOID(__stdcall* MapViewOfFileFn)      (HANDLE, DWORD, DWORD, DWORD, SIZE_T);
    typedef BOOL(__stdcall* UnmapViewOfFileFn)    (LPCVOID);
    typedef BOOL(__stdcall* CloseHandleFn)        (HANDLE);
    typedef VOID(__stdcall* OutputDebugStringA_t) (LPCSTR);
    typedef int(__cdecl* wsprintfA_t)          (LPSTR, LPCSTR, ...);
    typedef BOOL(__stdcall* WriteConsoleA_t)      (HANDLE, const VOID*, DWORD, LPDWORD, LPVOID);
    typedef HANDLE(__stdcall* GetStdHandleFn)      (DWORD);
    typedef BOOL(__stdcall* AllocConsole_t)        ();

    struct RemoteHandshake {
        HANDLE sharedTextureHandle;
        UINT   width;
        UINT   height;
        DWORD  producerPid;
    };

    struct D3D11StateBlock {
        ID3D11VertexShader* VS = nullptr;
        ID3D11PixelShader* PS = nullptr;
        ID3D11GeometryShader* GS = nullptr;
        ID3D11InputLayout* IL = nullptr;
        D3D11_PRIMITIVE_TOPOLOGY Topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

        ID3D11Buffer* VB[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};
        UINT          VBStrides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};
        UINT          VBOffsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};
        ID3D11Buffer* IB = nullptr;
        DXGI_FORMAT   IBFmt = DXGI_FORMAT_UNKNOWN;
        UINT          IBOff = 0;

        ID3D11RenderTargetView* RTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* DSV = nullptr;
        ID3D11BlendState* Blend = nullptr;
        FLOAT                    BlendFactor[4]{};
        UINT                     BlendMask = 0;
        ID3D11DepthStencilState* DSState = nullptr;
        UINT                     StencilRef = 0;

        ID3D11RasterizerState* RS = nullptr;
        D3D11_VIEWPORT VPs[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT           VPCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

        ID3D11ShaderResourceView* SRVs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
        ID3D11SamplerState* Samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT]{};

        ID3D11ClassInstance* VSInst[256]{}; UINT VSCount = 256;
        ID3D11ClassInstance* PSInst[256]{}; UINT PSCount = 256;
        ID3D11ClassInstance* GSInst[256]{}; UINT GSCount = 256;
    };

    struct PresentHookData : public LiquidHookEx::VTable::BaseHookData
    {
        ID3D11Texture2D* g_overlayTex = nullptr;
        ID3D11ShaderResourceView* g_overlaySRV = nullptr;
        IDXGIKeyedMutex* g_overlayMutex = nullptr;

        ID3D11VertexShader* g_compositeVS = nullptr;
        ID3D11PixelShader* g_compositePS = nullptr;
        ID3D11BlendState* g_compositeBlend = nullptr;
        ID3D11SamplerState* g_compositeSampler = nullptr;
        ID3D11RasterizerState* g_compositeRS = nullptr;

        BYTE   vsBlob[4096]{};
        SIZE_T vsBlobSize = 0;
        BYTE   psBlob[4096]{};
        SIZE_T psBlobSize = 0;

        ID3D11Device* pDevice = nullptr;
        ID3D11DeviceContext* pContext = nullptr;

        D3D11StateBlock stateBlock{};

        OpenFileMappingAFn  pOpenFileMappingA = nullptr;
        MapViewOfFileFn     pMapViewOfFile = nullptr;
        UnmapViewOfFileFn   pUnmapViewOfFile = nullptr;
        CloseHandleFn       pCloseHandle = nullptr;

        IID iid_ID3D11Device = {};
        IID iid_ID3D11Texture2D = {};
        IID iid_IDXGIKeyedMutex = {};

        AllocConsole_t       pAllocConsole = nullptr;
        GetStdHandleFn       pGetStdHandle = nullptr;
        WriteConsoleA_t      pWriteConsoleA = nullptr;
        OutputDebugStringA_t pOutputDebugStringA = nullptr;
        wsprintfA_t          pWsprintfA = nullptr;
        char                 logBuf[256]{};

        char sharedMemName[64]{};

        float float_max = 0.0f;
        float float_one = 0.0f;

        UINT lastBackbufferW;
        UINT lastBackbufferH;

        bool bInitDone = false;
        bool bResourcesReady = false;
        bool bOverlayReady = false;
        bool bFirstFrameLogged = false;
        bool bHandleResize;
    };

    class Present {
    private:
        inline static LiquidHookEx::VTable m_Hook = LiquidHookEx::VTable("Dx11PresentHook");

        static HRESULT __stdcall hkPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);
        static void hkPresentEnd();

    public:
        static bool Hook(IDXGISwapChain* pSwapChain);
        static bool Unhook();
    };

}