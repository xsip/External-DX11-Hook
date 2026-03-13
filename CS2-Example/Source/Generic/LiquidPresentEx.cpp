#include <Generic/LiquidPresentEx.h>
#include <LiquidHookEx/Include.h>
namespace LiquidHookEx {

    static void* g_pOriginalPresent = nullptr;
    static PresentHookData* g_pHookData = nullptr;

    LH_START("lhPresent")
#pragma warning(push)
#pragma warning(disable: 4733)
        __declspec(safebuffers)
        HRESULT __stdcall Present::hkPresent(IDXGISwapChain* swapChain,
            UINT syncInterval, UINT flags)
    {
        typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
        PresentFn        oPresent = (PresentFn)g_pOriginalPresent;
        PresentHookData* data = g_pHookData;

        auto _openMap = data->pOpenFileMappingA;
        auto _mapView = data->pMapViewOfFile;
        auto _unmapView = data->pUnmapViewOfFile;
        auto _closeH = data->pCloseHandle;

        if (!data->bInitDone) {

            data->bInitDone = true;

            HRESULT hr = S_OK;

            swapChain->GetDevice(data->iid_ID3D11Device, (void**)&data->pDevice);

            if (!data->pDevice) goto skip_resources;
            data->pDevice->GetImmediateContext(&data->pContext);

            hr = data->pDevice->CreateVertexShader(
                data->vsBlob, data->vsBlobSize, nullptr, &data->g_compositeVS);
            if (FAILED(hr)) goto skip_resources;

            hr = data->pDevice->CreatePixelShader(
                data->psBlob, data->psBlobSize, nullptr, &data->g_compositePS);
            if (FAILED(hr)) { data->g_compositeVS->Release(); data->g_compositeVS = nullptr; goto skip_resources; }

            {
                D3D11_BLEND_DESC bd{};
                bd.RenderTarget[0].BlendEnable = TRUE;
                bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
                bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
                bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
                bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
                bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
                bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
                bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
                hr = data->pDevice->CreateBlendState(&bd, &data->g_compositeBlend);
                if (FAILED(hr)) goto skip_resources;
            }

            {
                D3D11_SAMPLER_DESC sd{};
                sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
                sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
                sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
                sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
                sd.MaxLOD = data->float_max;
                hr = data->pDevice->CreateSamplerState(&sd, &data->g_compositeSampler);
                if (FAILED(hr)) goto skip_resources;
            }

            {
                D3D11_RASTERIZER_DESC rd{};
                rd.FillMode = D3D11_FILL_SOLID;
                rd.CullMode = D3D11_CULL_NONE;
                rd.DepthClipEnable = TRUE;
                hr = data->pDevice->CreateRasterizerState(&rd, &data->g_compositeRS);
                if (FAILED(hr)) goto skip_resources;
            }

            data->bResourcesReady = true;

            {
                HANDLE hMap = _openMap(FILE_MAP_READ, FALSE, data->sharedMemName);
                if (!hMap) { goto skip_overlay; }

                RemoteHandshake* hs = (RemoteHandshake*)_mapView(
                    hMap, FILE_MAP_READ, 0, 0, sizeof(RemoteHandshake));
                if (!hs) { _closeH(hMap); goto skip_overlay; }

                HANDLE sharedHandle = hs->sharedTextureHandle;
                _unmapView(hs);
                _closeH(hMap);

                if (!sharedHandle) { goto skip_overlay; }

                hr = data->pDevice->OpenSharedResource(
                    sharedHandle, data->iid_ID3D11Texture2D, (void**)&data->g_overlayTex);
                if (FAILED(hr)) { goto skip_overlay; }

                hr = data->pDevice->CreateShaderResourceView(
                    data->g_overlayTex, nullptr, &data->g_overlaySRV);
                if (FAILED(hr)) {
                    data->g_overlayTex->Release(); data->g_overlayTex = nullptr;
                    goto skip_overlay;
                }

                hr = data->g_overlayTex->QueryInterface(
                    data->iid_IDXGIKeyedMutex, (void**)&data->g_overlayMutex);
                if (FAILED(hr)) {
                    data->g_overlaySRV->Release(); data->g_overlaySRV = nullptr;
                    data->g_overlayTex->Release(); data->g_overlayTex = nullptr;
                    goto skip_overlay;
                }

                data->bOverlayReady = true;
            }

        skip_overlay:;
        skip_resources:;
        }

        if (data->bResourcesReady && data->bOverlayReady) {

            if (data->g_overlayMutex->AcquireSync(1, 8) == S_OK) {

                if (!data->bFirstFrameLogged) {
                    data->bFirstFrameLogged = true;
                }

                ID3D11DeviceContext* ctx = data->pContext;
                D3D11StateBlock* sb = &data->stateBlock;

                sb->VSCount = 256; sb->PSCount = 256; sb->GSCount = 256;
                ctx->VSGetShader(&sb->VS, sb->VSInst, &sb->VSCount);
                ctx->PSGetShader(&sb->PS, sb->PSInst, &sb->PSCount);
                ctx->GSGetShader(&sb->GS, sb->GSInst, &sb->GSCount);
                ctx->IAGetInputLayout(&sb->IL);
                ctx->IAGetPrimitiveTopology(&sb->Topo);
                ctx->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT,
                    sb->VB, sb->VBStrides, sb->VBOffsets);
                ctx->IAGetIndexBuffer(&sb->IB, &sb->IBFmt, &sb->IBOff);
                ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, sb->RTVs, &sb->DSV);
                ctx->OMGetBlendState(&sb->Blend, sb->BlendFactor, &sb->BlendMask);
                ctx->OMGetDepthStencilState(&sb->DSState, &sb->StencilRef);
                ctx->RSGetState(&sb->RS);
                sb->VPCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
                ctx->RSGetViewports(&sb->VPCount, sb->VPs);
                ctx->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, sb->SRVs);
                ctx->PSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, sb->Samplers);

                ID3D11Texture2D* backbuffer = nullptr;
                ID3D11RenderTargetView* backRTV = nullptr;
                swapChain->GetBuffer(0, data->iid_ID3D11Texture2D, (void**)&backbuffer);
                data->pDevice->CreateRenderTargetView(backbuffer, nullptr, &backRTV);
                backbuffer->Release();

                DXGI_SWAP_CHAIN_DESC scDesc{};
                swapChain->GetDesc(&scDesc);

                UINT bbW = scDesc.BufferDesc.Width;
                UINT bbH = scDesc.BufferDesc.Height;

                if (data->bHandleResize && data->bOverlayReady &&
                    (bbW != data->lastBackbufferW || bbH != data->lastBackbufferH) &&
                    data->lastBackbufferW != 0)
                {
                    data->g_overlayMutex->Release(); data->g_overlayMutex = nullptr;
                    data->g_overlaySRV->Release();   data->g_overlaySRV = nullptr;
                    data->g_overlayTex->Release();   data->g_overlayTex = nullptr;
                    data->bOverlayReady = false;
                    data->bInitDone = false;
                    data->lastBackbufferW = bbW;
                    data->lastBackbufferH = bbH;
                    return oPresent(swapChain, syncInterval, flags);
                }
                data->lastBackbufferW = bbW;
                data->lastBackbufferH = bbH;

                D3D11_VIEWPORT vp{};
                vp.Width = (float)scDesc.BufferDesc.Width;
                vp.Height = (float)scDesc.BufferDesc.Height;
                vp.MaxDepth = data->float_one;

                ctx->OMSetRenderTargets(1, &backRTV, nullptr);
                ctx->OMSetBlendState(data->g_compositeBlend, nullptr, 0xFFFFFFFF);
                ctx->OMSetDepthStencilState(nullptr, 0);
                ctx->RSSetState(data->g_compositeRS);
                ctx->RSSetViewports(1, &vp);
                ctx->IASetInputLayout(nullptr);
                ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
                ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
                ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                ctx->VSSetShader(data->g_compositeVS, nullptr, 0);
                ctx->PSSetShader(data->g_compositePS, nullptr, 0);
                ctx->GSSetShader(nullptr, nullptr, 0);
                ctx->PSSetShaderResources(0, 1, &data->g_overlaySRV);
                ctx->PSSetSamplers(0, 1, &data->g_compositeSampler);
                ctx->Draw(4, 0);

                backRTV->Release();

                ctx->VSSetShader(sb->VS, sb->VSInst, sb->VSCount);
                ctx->PSSetShader(sb->PS, sb->PSInst, sb->PSCount);
                ctx->GSSetShader(sb->GS, sb->GSInst, sb->GSCount);
                ctx->IASetInputLayout(sb->IL);
                ctx->IASetPrimitiveTopology(sb->Topo);
                ctx->IASetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT,
                    sb->VB, sb->VBStrides, sb->VBOffsets);
                ctx->IASetIndexBuffer(sb->IB, sb->IBFmt, sb->IBOff);
                ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, sb->RTVs, sb->DSV);
                ctx->OMSetBlendState(sb->Blend, sb->BlendFactor, sb->BlendMask);
                ctx->OMSetDepthStencilState(sb->DSState, sb->StencilRef);
                ctx->RSSetState(sb->RS);
                ctx->RSSetViewports(sb->VPCount, sb->VPs);
                ctx->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, sb->SRVs);
                ctx->PSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, sb->Samplers);

                auto SafeRelease = [](IUnknown* p) { if (p) p->Release(); };
                SafeRelease(sb->VS);  SafeRelease(sb->PS);  SafeRelease(sb->GS);
                SafeRelease(sb->IL);  SafeRelease(sb->IB);
                SafeRelease(sb->Blend); SafeRelease(sb->DSState); SafeRelease(sb->RS);
                SafeRelease(sb->DSV);
                for (auto& v : sb->VSInst)   SafeRelease(v);
                for (auto& v : sb->PSInst)   SafeRelease(v);
                for (auto& v : sb->GSInst)   SafeRelease(v);
                for (auto& v : sb->VB)       SafeRelease(v);
                for (auto& v : sb->RTVs)     SafeRelease(v);
                for (auto& v : sb->SRVs)     SafeRelease(v);
                for (auto& v : sb->Samplers) SafeRelease(v);

                data->g_overlayMutex->ReleaseSync(0);
            }
        }

        return oPresent(swapChain, syncInterval, flags);
    }

    void Present::hkPresentEnd() {}
#pragma warning(pop)
    LH_END()

        bool Present::Hook(IDXGISwapChain* pSwapChain) {
        PresentHookData hkData{};

        hkData.bInitDone = false;
        hkData.bResourcesReady = false;
        hkData.bOverlayReady = false;
        hkData.bFirstFrameLogged = false;
        hkData.bHandleResize = false; // currently causing crashes!

        hkData.float_max = D3D11_FLOAT32_MAX;
        hkData.float_one = 1.0f;

        hkData.iid_ID3D11Device = __uuidof(ID3D11Device);
        hkData.iid_ID3D11Texture2D = __uuidof(ID3D11Texture2D);
        hkData.iid_IDXGIKeyedMutex = __uuidof(IDXGIKeyedMutex);

        HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
        hkData.pAllocConsole = (AllocConsole_t)GetProcAddress(hKernel32, "AllocConsole");
        hkData.pGetStdHandle = (GetStdHandleFn)GetProcAddress(hKernel32, "GetStdHandle");
        hkData.pWriteConsoleA = (WriteConsoleA_t)GetProcAddress(hKernel32, "WriteConsoleA");
        hkData.pOutputDebugStringA = (OutputDebugStringA_t)GetProcAddress(hKernel32, "OutputDebugStringA");
        hkData.pWsprintfA = (wsprintfA_t)GetProcAddress(hKernel32, "wsprintfA");
        hkData.pOpenFileMappingA = (OpenFileMappingAFn)GetProcAddress(hKernel32, "OpenFileMappingA");
        hkData.pMapViewOfFile = (MapViewOfFileFn)GetProcAddress(hKernel32, "MapViewOfFile");
        hkData.pUnmapViewOfFile = (UnmapViewOfFileFn)GetProcAddress(hKernel32, "UnmapViewOfFile");
        hkData.pCloseHandle = (CloseHandleFn)GetProcAddress(hKernel32, "CloseHandle");

        hkData.lastBackbufferW = 0;
        hkData.lastBackbufferH = 0;

        {
            typedef HRESULT(__cdecl* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR,
                const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                UINT, UINT, ID3DBlob**, ID3DBlob**);

            HMODULE hD3DCompiler = LoadLibraryA("d3dcompiler_47.dll");
            if (!hD3DCompiler) {
                printf("[Hook] failed to load d3dcompiler_47.dll\n");
                return false;
            }
            auto pCompile = (D3DCompileFn)GetProcAddress(hD3DCompiler, "D3DCompile");

            const char* vsSrc =
                "void VS(uint id : SV_VertexID,"
                "        out float4 pos : SV_Position,"
                "        out float2 uv  : TEXCOORD0) {"
                "    uv  = float2((id & 1) ? 1.0 : 0.0, (id & 2) ? 1.0 : 0.0);"
                "    pos = float4(uv * float2(2,-2) + float2(-1,1), 0, 1);"
                "}";

            const char* psSrc =
                "Texture2D    overlayTex : register(t0);"
                "SamplerState s          : register(s0);"
                "float4 PS(float4 pos : SV_Position,"
                "          float2 uv  : TEXCOORD0) : SV_Target {"
                "    return overlayTex.Sample(s, uv);"
                "}";

            ID3DBlob* vsBlob = nullptr, * psBlob = nullptr, * errBlob = nullptr;

            HRESULT hr = pCompile(vsSrc, strlen(vsSrc), nullptr, nullptr, nullptr,
                "VS", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob);
            if (FAILED(hr)) {
                printf("[Hook] VS compile failed: %s\n",
                    errBlob ? (char*)errBlob->GetBufferPointer() : "?");
                if (errBlob) errBlob->Release();
                return false;
            }
            if (errBlob) { errBlob->Release(); errBlob = nullptr; }

            hr = pCompile(psSrc, strlen(psSrc), nullptr, nullptr, nullptr,
                "PS", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errBlob);
            if (FAILED(hr)) {
                printf("[Hook] PS compile failed: %s\n",
                    errBlob ? (char*)errBlob->GetBufferPointer() : "?");
                vsBlob->Release();
                if (errBlob) errBlob->Release();
                return false;
            }
            if (errBlob) { errBlob->Release(); errBlob = nullptr; }

            if (vsBlob->GetBufferSize() > sizeof(hkData.vsBlob) ||
                psBlob->GetBufferSize() > sizeof(hkData.psBlob)) {
                printf("[Hook] shader blob too large for PresentHookData buffer\n");
                vsBlob->Release(); psBlob->Release();
                return false;
            }

            memcpy(hkData.vsBlob, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize());
            hkData.vsBlobSize = vsBlob->GetBufferSize();
            memcpy(hkData.psBlob, psBlob->GetBufferPointer(), psBlob->GetBufferSize());
            hkData.psBlobSize = psBlob->GetBufferSize();

            printf("[Hook] VS blob: %zu bytes  PS blob: %zu bytes\n",
                hkData.vsBlobSize, hkData.psBlobSize);

            vsBlob->Release();
            psBlob->Release();
        }

        strcpy_s(hkData.sharedMemName, "Local\\LiquidOverlayHandshake");

        uintptr_t presentAddr = LiquidHookEx::proc->GetVTableFunction<8>(reinterpret_cast<uintptr_t>(pSwapChain));
        return m_Hook.HookUsingAddr<PresentHookData>(
            presentAddr,
            "DXGI.DLL",
            hkData,
            reinterpret_cast<void*>(Present::hkPresent),
            reinterpret_cast<void*>(Present::hkPresentEnd),
            {
                LiquidHookEx::VTable::RipSlot::Data(&g_pHookData),
                LiquidHookEx::VTable::RipSlot::Orig(&g_pOriginalPresent),
            });
    }

    bool Present::Unhook() {
        return m_Hook.Unhook();
    }

}