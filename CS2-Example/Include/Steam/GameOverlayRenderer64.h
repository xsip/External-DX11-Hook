#pragma once
#include <LiquidHookEx/Config.h>
#include <LiquidHookEx/Include.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

namespace Steam {

    class GameOverlaySwapChainData {
    public:
        IDXGISwapChain* GetSwapChain();
    };


    class GameOverlayRenderer64 {
    private:
        static GameOverlaySwapChainData* GetSwapChainData();
    public:
        static IDXGISwapChain* GetSwapChain() { return GetSwapChainData()->GetSwapChain(); };
    };

}