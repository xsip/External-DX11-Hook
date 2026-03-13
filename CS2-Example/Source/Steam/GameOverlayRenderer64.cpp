#include <Steam/GameOverlayRenderer64.h>
#include <LiquidHookEx/Include.h>
namespace Steam {
    IDXGISwapChain* GameOverlaySwapChainData::GetSwapChain() {
        return LiquidHookEx::proc->ReadDirect<IDXGISwapChain*>(reinterpret_cast<uintptr_t>(this) + 0x148);
    }

    GameOverlaySwapChainData* GameOverlayRenderer64::GetSwapChainData() {
        auto hGameOverlayRenderer64 = LiquidHookEx::proc->GetRemoteModule("gameoverlayrenderer64.dll");
        if (!hGameOverlayRenderer64 || !hGameOverlayRenderer64->IsValid()) {
            printf("Error getting GameOverlayRenderer64.dll!!\n");
            return nullptr;
        }

        auto pInstruction = reinterpret_cast<uintptr_t>(
            hGameOverlayRenderer64->ScanMemory("48 8B 0D ?? ?? ?? ?? 48 05"));
        if (!pInstruction) {
            printf("Error getting SwapChainData instruction in GameOverlayRenderer64.dll!!\n");
            return nullptr;
        }

        return LiquidHookEx::proc->ReadDirect<GameOverlaySwapChainData*>(
            hGameOverlayRenderer64->ResolveRIP(pInstruction));
    }

}