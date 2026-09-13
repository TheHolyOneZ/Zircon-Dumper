// Host for the live browser: a window, a D3D11 device, the ImGui frame loop. Everything
// the user actually interacts with lives in Browser, which knows none of this. Two shells
// call in here, the standalone executable and the injected payload, and both get the same
// UI out.

#include "Host.h"
#include "Browser.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include "resource.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

ID3D11Device*           g_device        = nullptr;
ID3D11DeviceContext*    g_context       = nullptr;
IDXGISwapChain*         g_swap_chain    = nullptr;
ID3D11RenderTargetView* g_render_target = nullptr;
bool                    g_occluded      = false;
ImFont*                 g_mono_font     = nullptr;
UINT                    g_resize_width  = 0;
UINT                    g_resize_height = 0;

void CreateRenderTarget() {
    ID3D11Texture2D* back_buffer = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (!back_buffer) return;
    g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target);
    back_buffer->Release();
}

void ReleaseRenderTarget() {
    if (g_render_target) { g_render_target->Release(); g_render_target = nullptr; }
}

bool CreateDevice(HWND window) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount                        = 2;
    desc.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator   = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    desc.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow                       = window;
    desc.SampleDesc.Count                   = 1;
    desc.Windowed                           = TRUE;
    desc.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
        &desc, &g_swap_chain, &g_device, &level, &g_context);

    // WARP, don't refuse to start. A machine without a usable GPU path should still get
    // the tool.
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
            &desc, &g_swap_chain, &g_device, &level, &g_context);
    }
    if (FAILED(hr)) return false;

    CreateRenderTarget();
    return true;
}

void DestroyDevice() {
    ReleaseRenderTarget();
    if (g_swap_chain) { g_swap_chain->Release(); g_swap_chain = nullptr; }
    if (g_context)    { g_context->Release();    g_context = nullptr; }
    if (g_device)     { g_device->Release();     g_device = nullptr; }
}

// The icon for the title bar, Alt-Tab and the taskbar.
//
// Loaded from whichever module this code is linked into, found from the address of a
// function inside it. Same module in the standalone browser; different ones in the
// injected payload, where the host process is the game and has an icon of its own.
HICON LoadZirconIcon(int size) {
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&CreateRenderTarget), &self))
        return nullptr;

    return static_cast<HICON>(::LoadImageW(self, MAKEINTRESOURCEW(IDI_ZIRCON), IMAGE_ICON,
                                           size, size, LR_DEFAULTCOLOR));
}

// Windows paints the title bar from the user's accent colour, which puts a bright strip
// above a near-black window. These attributes are the supported way to ask for something
// else. Both are no-ops before Windows 10 2004 and a failure is ignored: a light caption
// is a blemish, not a reason to refuse to open.
void ApplyDarkCaption(HWND window) {
    HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;

    using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto set = reinterpret_cast<SetAttribute>(
        reinterpret_cast<void*>(::GetProcAddress(dwm, "DwmSetWindowAttribute")));
    if (set) {
        constexpr DWORD kUseImmersiveDarkMode = 20;   // DWMWA_USE_IMMERSIVE_DARK_MODE
        constexpr DWORD kCaptionColour        = 35;   // DWMWA_CAPTION_COLOR, Win11 22000+
        constexpr DWORD kBorderColour         = 34;   // DWMWA_BORDER_COLOR

        const BOOL     dark    = TRUE;
        const COLORREF caption = RGB(14, 15, 18);
        const COLORREF border  = RGB(46, 49, 58);

        set(window, kUseImmersiveDarkMode, &dark, sizeof(dark));
        set(window, kCaptionColour, &caption, sizeof(caption));
        set(window, kBorderColour, &border, sizeof(border));
    }

    ::FreeLibrary(dwm);
}

LRESULT WINAPI WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return true;

    switch (message) {
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) return 0;
            g_resize_width  = static_cast<UINT>(LOWORD(lparam));
            g_resize_height = static_cast<UINT>(HIWORD(lparam));
            return 0;
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_KEYMENU) return 0;   // swallow the alt menu
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(window, message, wparam, lparam);
}

// ImGui's built-in font is a 13px bitmap, and nothing else makes a tool look so unfinished
// so fast. Both faces here ship with every supported Windows. If either is missing we keep
// the default instead of refusing to open a window over a font.
void LoadFonts(float dpi_scale) {
    ImGuiIO& io = ImGui::GetIO();

    wchar_t windows_dir[MAX_PATH] = {};
    if (::GetWindowsDirectoryW(windows_dir, MAX_PATH) == 0) return;

    char fonts_dir[MAX_PATH * 2] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, windows_dir, -1, fonts_dir,
                          static_cast<int>(sizeof(fonts_dir)), nullptr, nullptr);

    char buffer[MAX_PATH * 2];
    const auto path = [&](const char* name) -> const char* {
        std::snprintf(buffer, sizeof(buffer), "%s\\Fonts\\%s", fonts_dir, name);
        return buffer;
    };

    const float ui_size   = 16.0f * dpi_scale;
    const float mono_size = 15.0f * dpi_scale;

    for (const char* candidate : {"segoeui.ttf", "tahoma.ttf"}) {
        if (io.Fonts->AddFontFromFileTTF(path(candidate), ui_size)) break;
    }

    // Offsets, addresses and decompiled script are columnar. They want fixed pitch and a
    // slashed zero, which matters when you read hex all day.
    for (const char* candidate : {"CascadiaMono.ttf", "consola.ttf", "cour.ttf"}) {
        g_mono_font = io.Fonts->AddFontFromFileTTF(path(candidate), mono_size);
        if (g_mono_font) break;
    }
}

// A palette, not a tint of the default. One cool near-black ground, one accent, borders
// that separate without drawing attention. Everything that isn't text should recede; the
// content is dense and carries its own colour coding already.
void ApplyStyle(float dpi_scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding    = 0.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 6.0f;
    style.GrabRounding      = 5.0f;
    style.TabRounding       = 5.0f;
    style.ScrollbarRounding = 9.0f;
    style.ScrollbarSize     = 12.0f;
    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 1.0f;
    // Frames need an outline. An unchecked checkbox and an idle button are both FrameBg on
    // ChildBg, and at this contrast they read as empty space instead of controls. One pixel
    // of border is what makes them findable.
    style.FrameBorderSize   = 1.0f;
    style.WindowPadding     = ImVec2(12.0f, 10.0f);
    style.FramePadding      = ImVec2(9.0f, 4.0f);
    style.ItemSpacing       = ImVec2(9.0f, 6.0f);
    style.ItemInnerSpacing  = ImVec2(7.0f, 5.0f);
    style.CellPadding       = ImVec2(8.0f, 4.0f);
    style.IndentSpacing     = 20.0f;

    const ImVec4 ground     = ImVec4(0.055f, 0.059f, 0.071f, 1.00f);
    const ImVec4 panel      = ImVec4(0.086f, 0.090f, 0.106f, 1.00f);
    const ImVec4 raised     = ImVec4(0.153f, 0.161f, 0.192f, 1.00f);
    const ImVec4 raised_hot = ImVec4(0.169f, 0.180f, 0.212f, 1.00f);
    const ImVec4 border     = ImVec4(0.239f, 0.255f, 0.298f, 1.00f);
    const ImVec4 accent     = ImVec4(0.310f, 0.600f, 0.949f, 1.00f);
    const ImVec4 accent_dim = ImVec4(0.310f, 0.600f, 0.949f, 0.32f);
    const ImVec4 accent_hot = ImVec4(0.420f, 0.690f, 1.000f, 1.00f);
    const ImVec4 text       = ImVec4(0.886f, 0.898f, 0.925f, 1.00f);
    const ImVec4 text_dim   = ImVec4(0.478f, 0.502f, 0.557f, 1.00f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]             = ground;
    c[ImGuiCol_ChildBg]              = panel;
    c[ImGuiCol_PopupBg]              = raised;
    c[ImGuiCol_Border]               = border;
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text]                 = text;
    c[ImGuiCol_TextDisabled]         = text_dim;
    c[ImGuiCol_FrameBg]              = raised;
    c[ImGuiCol_FrameBgHovered]       = raised_hot;
    c[ImGuiCol_FrameBgActive]        = raised_hot;
    c[ImGuiCol_TitleBg]              = panel;
    c[ImGuiCol_TitleBgActive]        = panel;
    c[ImGuiCol_TitleBgCollapsed]     = panel;
    c[ImGuiCol_MenuBarBg]            = panel;
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = raised_hot;
    c[ImGuiCol_ScrollbarGrabHovered] = border;
    c[ImGuiCol_ScrollbarGrabActive]  = accent;
    c[ImGuiCol_CheckMark]            = accent;
    c[ImGuiCol_SliderGrab]           = accent;
    c[ImGuiCol_SliderGrabActive]     = accent_hot;
    c[ImGuiCol_Button]               = raised;
    c[ImGuiCol_ButtonHovered]        = raised_hot;
    c[ImGuiCol_ButtonActive]         = accent_dim;
    c[ImGuiCol_Header]               = accent_dim;
    c[ImGuiCol_HeaderHovered]        = raised_hot;
    c[ImGuiCol_HeaderActive]         = accent_dim;
    c[ImGuiCol_Separator]            = border;
    c[ImGuiCol_SeparatorHovered]     = accent;
    c[ImGuiCol_SeparatorActive]      = accent_hot;
    c[ImGuiCol_ResizeGrip]           = raised;
    c[ImGuiCol_ResizeGripHovered]    = accent_dim;
    c[ImGuiCol_ResizeGripActive]     = accent;
    c[ImGuiCol_Tab]                  = panel;
    c[ImGuiCol_TabHovered]           = raised_hot;
    c[ImGuiCol_TabSelected]          = raised;
    c[ImGuiCol_TabDimmed]            = panel;
    c[ImGuiCol_TabDimmedSelected]    = raised;
    c[ImGuiCol_TableHeaderBg]        = raised;
    c[ImGuiCol_TableBorderStrong]    = border;
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.133f, 0.141f, 0.169f, 1.00f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.0f, 1.0f, 1.0f, 0.018f);
    c[ImGuiCol_TextSelectedBg]       = accent_dim;
    c[ImGuiCol_NavCursor]            = accent;

    if (dpi_scale > 1.0f) style.ScaleAllSizes(dpi_scale);
}

// One window, one device, one loop, for whatever Browser the caller has prepared.
int RunLoop(zircon::gui::Browser& browser) {
    HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW window_class{};
    window_class.cbSize        = sizeof(window_class);
    window_class.style         = CS_CLASSDC;
    window_class.lpfnWndProc   = WndProc;
    window_class.hInstance     = instance;
    window_class.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon         = LoadZirconIcon(::GetSystemMetrics(SM_CXICON));
    window_class.hIconSm       = LoadZirconIcon(::GetSystemMetrics(SM_CXSMICON));
    window_class.lpszClassName = L"ZirconBrowser";
    if (!::RegisterClassExW(&window_class) &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 1;

    HWND window = ::CreateWindowW(window_class.lpszClassName,
                                  L"Zircon - live object browser",
                                  WS_OVERLAPPEDWINDOW, 100, 100, 1400, 860,
                                  nullptr, nullptr, instance, nullptr);
    if (!window) {
        ::UnregisterClassW(window_class.lpszClassName, instance);
        return 1;
    }

    if (!CreateDevice(window)) {
        DestroyDevice();
        ::UnregisterClassW(window_class.lpszClassName, instance);
        ::MessageBoxW(nullptr, L"Could not create a D3D11 device.", L"Zircon", MB_ICONERROR);
        return 1;
    }

    ApplyDarkCaption(window);
    ::ShowWindow(window, SW_SHOWDEFAULT);
    ::UpdateWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Forty thousand offsets at 96 DPI metrics on a 4K panel is unreadable, and this tool
    // gets read far more than it gets clicked. Fonts have to be added before the backend
    // builds its atlas.
    const float dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(window);
    LoadFonts(dpi_scale);
    ApplyStyle(dpi_scale);

    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_context);

    bool running = true;

    while (running) {
        MSG message;
        while (::PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessage(&message);
            if (message.message == WM_QUIT) running = false;
        }
        if (!running) break;

        // Presenting to an occluded window burns a core for nothing.
        if (g_occluded && g_swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(10);
            continue;
        }
        g_occluded = false;

        if (g_resize_width != 0 && g_resize_height != 0) {
            ReleaseRenderTarget();
            g_swap_chain->ResizeBuffers(0, g_resize_width, g_resize_height,
                                        DXGI_FORMAT_UNKNOWN, 0);
            g_resize_width = g_resize_height = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        browser.Draw();
        if (browser.WantsExit()) running = false;

        ImGui::Render();
        const float clear[4] = {0.06f, 0.06f, 0.08f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_render_target, nullptr);
        g_context->ClearRenderTargetView(g_render_target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const HRESULT present = g_swap_chain->Present(1, 0);
        g_occluded = present == DXGI_STATUS_OCCLUDED;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_mono_font = nullptr;   // owned by the atlas the context just destroyed
    DestroyDevice();
    ::DestroyWindow(window);
    ::UnregisterClassW(window_class.lpszClassName, instance);

    // Two shells share these globals, and a payload can be injected, unloaded and injected
    // again within one run of the game. Stale handles make that second attach fail in a way
    // that looks like a graphics problem.
    g_resize_width = g_resize_height = 0;
    g_occluded = false;
    return 0;
}

} // namespace

namespace zircon::gui {

ImFont* MonoFont() { return g_mono_font; }

int RunBrowserWindow(std::uint32_t attach_pid) {
    Browser browser;
    if (attach_pid != 0) browser.AttachTo(attach_pid);
    return RunLoop(browser);
}

int RunBrowserWindow(std::unique_ptr<core::IMemorySource> memory,
                     const engine::Reflection& reflection) {
    Browser browser;
    browser.Adopt(std::move(memory), reflection);
    return RunLoop(browser);
}

} // namespace zircon::gui
