#include <Steam-Example/Include.h>
#include <Steam/GameOverlayRenderer64.h>
#include <Generic/LiquidPresentEx.h>
int main() {
	
	LiquidHookEx::INIT("cs2.exe");
	RenderObjectManager renderObjectManager{ {} };
	OverlayAPI::InitImGuiContext();
	
	
	HeadlessRenderer::Start(&renderObjectManager, LiquidHookEx::proc->GetHwnd(), 1920,1080);

	while (!HeadlessRenderer::IsReady()) Sleep(1);
	auto pSwapChain = Steam::GameOverlayRenderer64::GetSwapChain();
	if (!LiquidHookEx::Present::Hook(pSwapChain)) {
		printf("Couldn't hook DX11::Present!!\n");
		return -1;
	}

	while (!GetAsyncKeyState(VK_DELETE)) {
		
		Sleep(500);
	}

	if (!LiquidHookEx::Present::Unhook()) {
		printf("Couldn't unhook DX11::Present!!\nIt's better to restart the process now!\n");
		return -1;

	}

	HeadlessRenderer::Stop();

	return 1;
}