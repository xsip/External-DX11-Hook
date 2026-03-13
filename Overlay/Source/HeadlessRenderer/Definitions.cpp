#include <HeadlessRenderer/Definitions.h>
#include <imgui/imgui.h>
#include <Windows.h>
#include <iostream>

#include <imgui/backends/imgui_impl_dx11.h>
#include <imgui/backends/imgui_impl_win32.h>

namespace OverlayAPI {
    // In DLL (Overlay.cpp)
    static ImGuiContext* g_ctx = nullptr;

    void InitImGuiContext()
    {
        g_ctx = ::ImGui::CreateContext();
        ::ImGui::SetCurrentContext(g_ctx);


    }

}