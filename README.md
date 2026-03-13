# External-DX11-Hook

An external, two-process DX11 overlay framework for CS2 (and any DXGI title).
Renders a fully interactive ImGui UI in a headless Direct3D 11 process and composites
it onto the game's back-buffer every frame via a hooked `IDXGISwapChain::Present` —
no DLL injection, no in-process threads. Using [LiquidHookEx - External Hooking Library for C++](https://github.com/xsip/LiquidHookEx)

---

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
   - [Projects / modules](#projects--modules)
   - [Handshake protocol](#handshake-protocol)
   - [Synchronisation (IDXGIKeyedMutex)](#synchronisation-idxgikeyedmutex)
- [Startup sequence](#startup-sequence)
- [HeadlessRenderer](#headlessrenderer)
   - [Thread model](#thread-model)
   - [Shared texture](#shared-texture)
   - [Input feeding](#input-feeding)
   - [RenderObjectManager](#renderobjectmanager)
- [LiquidPresentEx — hkPresent](#liquidpresentex--hkpresent)
   - [Hook installation](#hook-installation)
   - [Per-frame work](#per-frame-work)
   - [Composite shaders](#composite-shaders)
   - [D3D11 state save / restore](#d3d11-state-save--restore)
- [Steam — GameOverlayRenderer64](#steam--gameoverlayrenderer64)
- [Known issues](#known-issues)
- [Planned improvements](#planned-improvements)
- [Project structure](#project-structure)

---

## Overview

External-DX11-Hook is a framework for building game overlays that run entirely outside
the target process. The overlay UI lives in a separate process (`CS2-Example.exe`) and
never touches the game's address space directly. The only coupling between the two
processes is a GPU-synchronised shared texture and a small named file-mapping handshake.

**Key design goals:**

- **No DLL injection** — runs as an external process; no `LoadLibrary` call in the target.
- **Zero game-state coupling** — the overlay renderer and the present hook are decoupled by IPC.
- **Full D3D11 state save/restore** — the hook leaves the game's render state completely intact.
- **`IDXGIKeyedMutex` sync** — GPU-side cross-process synchronisation; no CPU spin-locks.
- **[LiquidHookEx](https://github.com/xsip/LiquidHookEx) VTable engine** — shellcode-section hooks with remote trampolines and RIP-slot patching.

**Frame pipeline (high level):**

```
HeadlessRenderer  →  SharedTexture (KeyedMutex)  →  hkPresent (cs2.exe)  →  Backbuffer Composite
```

---

## Architecture

### Projects / modules

| Module | Role |
|---|---|
| `Overlay.dll` | HeadlessRenderer + ImGui render loop (producer side) |
| `CS2-Example.exe` | Bootstrapper: inits [LiquidHookEx](https://github.com/xsip/LiquidHookEx), hooks Present, owns the main loop |
| `LiquidHookEx.lib` | External-process VTable / Detour / CallSite hook engine |

### Handshake protocol

Before the hook is installed, `HeadlessRenderer` creates a shared texture and publishes
its NT kernel handle via a named Windows file mapping (`Local\LiquidOverlayHandshake`):

```cpp
struct SharedOverlayHandshake {
    HANDLE  sharedTextureHandle;   // NT kernel handle for the overlay texture
    UINT    width, height;          // texture dimensions at creation time
    DWORD   producerPid;            // overlay process PID
};
```

On the **first** `IDXGISwapChain::Present` call after the hook is installed, the shellcode
running inside `cs2.exe`:

1. Opens the file mapping with `OpenFileMappingA`.
2. Maps it with `MapViewOfFile` and reads `sharedTextureHandle`.
3. Calls `ID3D11Device::OpenSharedResource` to import the texture into the game's D3D11 device.
4. Creates a `ID3D11ShaderResourceView` and queries `IDXGIKeyedMutex` from the texture.

From this point on the shared texture is accessible from both processes and composited
every frame.

### Synchronisation (IDXGIKeyedMutex)

Cross-process GPU texture access is serialised with `IDXGIKeyedMutex`. The texture is created
with `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` and two key values are used:

| Key | Holder | Meaning |
|---|---|---|
| `0` | Producer (HeadlessRenderer) | Overlay is rendering into the texture |
| `1` | Consumer (hkPresent) | Game is compositing the texture onto the back-buffer |

**Protocol:**

1. Render thread starts by calling `ReleaseSync(0)` — hands initial ownership to itself.
2. Each render frame: `AcquireSync(0, 32ms)` → clear + render ImGui → `ReleaseSync(1)`.
3. Each present frame: `AcquireSync(1, 8ms)` → composite onto back-buffer → `ReleaseSync(0)`.

No CPU spin-locks. All waiting happens inside the GPU driver.

---

## Startup sequence

```cpp
// 1. Attach to cs2.exe, map its modules
LiquidHookEx::INIT("cs2.exe");

// 2. Start headless render thread + scroll hook thread; publish handshake
HeadlessRenderer::Start(&renderObjectManager, hwnd, 1920, 1080);

// 3. Wait for shared texture to be live (render thread signals s_ready)
while (!HeadlessRenderer::IsReady()) Sleep(1);

// 4. Recover the real IDXGISwapChain* from Steam's overlay DLL
auto pSwapChain = Steam::GameOverlayRenderer64::GetSwapChain();

// 5. Install VTable hook on IDXGISwapChain::Present (slot 8)
LiquidHookEx::Present::Hook(pSwapChain);

// 6. Main loop — add per-frame game logic here
while (!GetAsyncKeyState(VK_DELETE)) Sleep(500);

// 7. Cleanly unhook: restores vtable slot, frees trampoline + remote allocs
LiquidHookEx::Present::Unhook();

// 8. Signal render thread, join, release all D3D11 resources
HeadlessRenderer::Stop();
```

---

## HeadlessRenderer

`HeadlessRenderer` is the overlay producer. It owns a dedicated `ID3D11Device` with no
swap chain and no OS window. Every frame it renders ImGui into a shared texture and signals
the consumer via `IDXGIKeyedMutex`.

### Thread model

| Thread | Purpose |
|---|---|
| **Render thread** | Device init → `CreateSharedTexture` → `PublishHandshake` → ImGui render loop |
| **ScrollHook thread** | Installs `WH_MOUSE_LL`; pumps its own message loop; accumulates wheel delta for `FeedInput` via `InterlockedAdd` |
| **Resize watch thread** | Polls `GetClientRect`; calls `Resize()` when the game window changes dimensions. Currently disabled — see [Known issues](#known-issues). |

### Shared texture

```cpp
D3D11_TEXTURE2D_DESC desc = {};
desc.Format   = DXGI_FORMAT_B8G8R8A8_UNORM;      // broadest cross-device shared support
desc.Usage    = D3D11_USAGE_DEFAULT;
desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
```

`DXGI_FORMAT_B8G8R8A8_UNORM` is used because it is the most widely supported format for
cross-process `OpenSharedResource` calls on Windows. After creation, the NT handle is
retrieved via `IDXGIResource::GetSharedHandle` and stored in the handshake file mapping.

### Input feeding

`FeedInput()` is called once per render frame, directly inside the render loop:

- `GetCursorPos` + `ScreenToClient` → `ImGuiIO::MousePos` (clamped to texture bounds).
- `GetAsyncKeyState` for LMB / RMB / MMB → `ImGuiIO::MouseDown[0..2]`.
- `InterlockedExchange` on `s_wheelAccum` (written by the scroll hook thread) → `ImGuiIO::MouseWheel`.
- `GetTickCount64` delta → `ImGuiIO::DeltaTime`.

### RenderObjectManager

A lightweight polymorphic render object list. Users push objects via `AddRenderObject()`;
each object implements:

```cpp
virtual void Render(ImVec2 displaySize, ImVec2 cursorPos, bool bMouseReleased);
```

All objects are iterated each frame between `ImGui::NewFrame()` and `ImGui::Render()`.

---

## LiquidPresentEx — hkPresent

### Hook installation

`IDXGISwapChain::Present` is virtual. Its address is recovered at slot 8:

```cpp
uintptr_t presentAddr =
    LiquidHookEx::proc->GetVTableFunction<8>(reinterpret_cast<uintptr_t>(pSwapChain));
```

The hook is installed with `LiquidHookEx::VTable::HookUsingAddr`, which:

1. Allocates a remote trampoline inside `cs2.exe` (`PAGE_EXECUTE_READWRITE`).
2. Writes a 14-byte `FF 25` absolute indirect jump to redirect execution.
3. Copies the `PresentHookData` struct (shaders, API function pointers, IIDs, etc.) into remote memory.
4. Patches two RIP slots in the shellcode body: `g_pOriginalPresent` (trampoline address) and `g_pHookData` (remote struct pointer).
5. Overwrites vtable slot 8 with the shellcode address.

Everything the shellcode needs at runtime — API function pointers, IIDs, shader blobs,
the shared memory name — is embedded in `PresentHookData` before the hook is installed.
The hook body does **not** call any imports; it only calls addresses stored in the struct.

### Per-frame work

On each `IDXGISwapChain::Present` call inside `cs2.exe`:

| Step | Action |
|---|---|
| **1** | First call only: init D3D11 device ref, create VS/PS from pre-compiled blobs, open shared texture, create SRV + IDXGIKeyedMutex. |
| **2** | Save full `D3D11StateBlock` (shaders, IA buffers, OM targets, blend/DS/RS states, viewports, SRVs, samplers, class instances). |
| **3** | `AcquireSync(key 1, 8 ms timeout)` — take ownership of the overlay texture from the producer. |
| **4** | Get back-buffer, create RTV, set fullscreen viewport. |
| **5** | Draw 4 vertices as `D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP` — fullscreen quad generated from `SV_VertexID`, no vertex buffer needed. |
| **6** | Restore full `D3D11StateBlock`; release all captured COM references. |
| **7** | `ReleaseSync(key 0)` — return texture to the HeadlessRenderer producer. |
| **8** | Call original `Present` via the trampoline. |

### Composite shaders

Compiled at hook-install time (via `d3dcompiler_47.dll`) and stored as raw bytecode in
`PresentHookData::vsBlob` / `psBlob` (up to 4096 bytes each). The blobs are created in
`Present::Hook()` and written into remote memory as part of the hook data struct, so the
shellcode can call `CreateVertexShader` / `CreatePixelShader` on first entry without
accessing any module outside the target process.

**Vertex shader** — no vertex buffer; position and UV are derived entirely from `SV_VertexID`:

```hlsl
void VS(uint id : SV_VertexID,
        out float4 pos : SV_Position,
        out float2 uv  : TEXCOORD0)
{
    uv  = float2((id & 1) ? 1.0 : 0.0, (id & 2) ? 1.0 : 0.0);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
```

**Pixel shader** — samples the overlay texture and passes RGBA straight through:

```hlsl
Texture2D    overlayTex : register(t0);
SamplerState s          : register(s0);

float4 PS(float4 pos : SV_Position,
          float2 uv  : TEXCOORD0) : SV_Target
{
    return overlayTex.Sample(s, uv);
}
```

**Blend state** — straight (non-premultiplied) alpha compositing:

```
SrcBlend      = SRC_ALPHA          DestBlend      = INV_SRC_ALPHA
SrcBlendAlpha = ONE                DestBlendAlpha = INV_SRC_ALPHA
```

### D3D11 state save / restore

`hkPresent` performs a complete save and restore of the D3D11 immediate context pipeline state
via `D3D11StateBlock`. Every stage that the composite draw touches is captured before and
restored after, including full class instance arrays for VS/PS/GS:

```
VS / PS / GS shaders + class instances
IA  input layout, primitive topology, vertex buffers (all slots), index buffer
OM  render targets (all slots) + DSV, blend state + factor + mask, depth-stencil state + ref
RS  rasterizer state, viewports (all slots)
PS  shader resources (all slots), samplers (all slots)
```

All captured COM interfaces are `Release()`d after restoration so no reference count is leaked.

---

## Steam — GameOverlayRenderer64

CS2 runs under Steam, which injects `GameOverlayRenderer64.dll` early in process startup
and replaces the game's `IDXGISwapChain` vtable pointer with its own. Hooking a dummy swap
chain obtained by creating a temporary DX11 device in our process would target the wrong
vtable entry and `hkPresent` would never fire.

`Steam::GameOverlayRenderer64::GetSwapChain()` recovers the real `IDXGISwapChain*` that
the Steam overlay is wrapping, so [LiquidHookEx](https://github.com/xsip/LiquidHookEx) patches the correct vtable slot 8 that
`cs2.exe` actually calls every frame.

> Without this step, hooking a dummy swap chain would install the hook on an object that
> is never presented, and the overlay would never appear.

---

## Known issues

### `bHandleResize` — causes crashes (disabled)

Both `HeadlessRenderer` and `LiquidPresentEx` have `bHandleResize = false` with a comment
noting it causes crashes. The root cause is in `hkPresent`:

```cpp
// Inside AcquireSync(key 1) block:
data->g_overlayMutex->Release();   // ← BUG: keyed mutex protocol violation
data->g_overlaySRV->Release();
data->g_overlayTex->Release();
data->bOverlayReady = false;
data->bInitDone = false;
```

`Release()` is called on the keyed mutex while the hook still holds the `AcquireSync(1)`
lock — the consumer side never called `ReleaseSync` first. This is a keyed mutex protocol
violation that will deadlock or crash the producer on its next `AcquireSync(0)`.

**Fix:** call `g_overlayMutex->ReleaseSync(0)` before releasing and nulling the resources,
then `return oPresent(...)` immediately to let the next frame cycle through a clean re-init.

### Detached resize watch thread leaks on `Stop()`

```cpp
std::thread([]() {
    while (true) {          // ← no termination condition
        HeadlessRenderer::HandleResize();
        Sleep(1);
    }
}).detach();
```

The resize watch thread is detached and never joined. After `Stop()` sets `s_running = false`,
this thread continues to spin indefinitely (or until the process exits).

**Fix:** track it as a `HANDLE` (like `s_scrollThread`) and check `s_running` in the loop body.

### Handshake race on resize

After a resize, `hkPresent` sets `bInitDone = false` and re-initialises on the next frame,
calling `OpenSharedResource` from the file mapping. However, `HeadlessRenderer::RebuildSharedResources`
creates a new texture with a new handle and overwrites the mapping — there is a window where
the hook reads the mapping before the overlay has finished publishing the new handle.

**Fix:** add a generation counter or a `handshakeReady` boolean to `SharedOverlayHandshake`.
The hook should only proceed with `OpenSharedResource` once the flag is set.

### `pWsprintfA` resolved but never used

`GetProcAddress(kernel32, "wsprintfA")` stores a function pointer in `PresentHookData` that
is never called anywhere in `hkPresent`. Leftover from earlier debug logging. Safe to remove.

### Shader blob buffers are fixed-size (4 KB)

`vsBlob[4096]` / `psBlob[4096]` are embedded inside `PresentHookData`, which lives in
the target process's remote memory. If the shaders grow beyond 4 096 bytes the overflow
is silent and will corrupt adjacent fields in the struct.

**Fix:** add a `static_assert` on `vsBlob->GetBufferSize() <= sizeof(hkData.vsBlob)` after
compilation (one already exists as a runtime check; make it a compile-time guard too), or
move the blobs to a separate heap allocation.

### `FeedInput` forces focus unconditionally

```cpp
io.AppFocusLost = false;
io.AddFocusEvent(true);
```

These two lines are set every render frame regardless of whether the game window has OS
focus. ImGui widgets will respond to clicks even when the game is backgrounded.

---

## Planned improvements

- Resize support with correct keyed mutex teardown (see above).
- `DXGI_FORMAT_R8G8B8A8_UNORM` fallback for GPU drivers that reject `BGRA` cross-process shared textures.
- `DuplicateHandle()` cross-process path instead of passing raw `HANDLE` values through shared memory (raw handles are only valid if producer and consumer are on the same session/integrity level).
- Keyboard input via `WH_KEYBOARD_LL` for text-entry ImGui widgets.
- Generation counter in `SharedOverlayHandshake` to close the resize race window.
- Replace the detached resize thread with a joinable `HANDLE`-tracked thread.

---

## Project structure

```
External-DX11-Hook/
│
├── CS2-Example/
│   ├── Main.cpp                           ← bootstrapper: INIT, Start, Hook, loop, Unhook, Stop
│   ├── Include/
│   │   ├── Generic/
│   │   │   └── LiquidPresentEx.h          ← PresentHookData, D3D11StateBlock, Present class
│   │   └── Steam/
│   │       └── GameOverlayRenderer64.h    ← GetSwapChain() — recovers the real IDXGISwapChain*
│   └── Source/
│       ├── Generic/
│       │   └── LiquidPresentEx.cpp        ← hkPresent shellcode + Present::Hook / Unhook
│       └── Steam/
│           └── GameOverlayRenderer64.cpp
│
├── Overlay/
│   ├── Include/
│   │   └── HeadlessRenderer/
│   │       ├── HeadlessRenderer.h         ← Start / Stop / IsReady / Resize API
│   │       ├── RenderObject.h             ← polymorphic base: virtual Render(...)
│   │       ├── RenderObjectManager.h      ← render object list + per-frame dispatch
│   │       ├── LiquidMenu.h               ← RenderLiquidMenu() declaration
│   │       └── Definitions.h             ← OVERLAY_API dllexport/import + OverlayAPI::InitImGuiContext
│   └── Source/
│       └── HeadlessRenderer/
│           ├── HeadlessRenderer.cpp       ← device, shared texture, render loop, input, threads
│           ├── RenderObjectManager.cpp
│           ├── LiquidMenu.cpp             ← RenderLiquidMenu() — project overview ImGui menu
│           └── Definitions.cpp
│
└── LiquidHookEx/                          ← external-process hooking library (submodule)
    └── LiquidHookEx/
        └── Include/LiquidHookEx/
            ├── VTable.h                   ← vtable slot hook (x64)
            ├── CallSite.h                 ← call site instruction hook (x64)
            ├── Detour.h                   ← function prologue detour (x64)
            ├── Config.h                   ← HookConfig JSON persistence
            ├── Process.h                  ← remote process abstraction
            ├── Pattern.h                  ← byte pattern scanner
            ├── Globals.h                  ← global Process* + INIT()
            └── Macros.h                   ← LH_START / LH_END shellcode boundary macros

```
