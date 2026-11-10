// squeak: a small window showing Razer mice and keyboards (battery, DPI
// stages, polling rate) and setting their lighting, without Synapse.
// Drawn with Dear ImGui on Direct3D 11, frameless like kitchensink.
#include <windows.h>
#include <d3d11.h>
#include <dbt.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <cstdio>
#include <memory>
#include <vector>

#include "app.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "resource.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr UINT WM_APP_WAKE = WM_APP + 1;
const wchar_t* kClassName = L"squeak";

ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;
IDXGISwapChain* swapChain = nullptr;
ID3D11RenderTargetView* target = nullptr;
UINT resizeW = 0, resizeH = 0;

HWND hwnd = nullptr;
std::unique_ptr<sq::App> app;
int framesWanted = 2;
bool dark = true;

void createTarget() {
    ID3D11Texture2D* back = nullptr;
    swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    device->CreateRenderTargetView(back, nullptr, &target);
    back->Release();
}

void releaseTarget() {
    if (target) target->Release();
    target = nullptr;
}

bool createDevice() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &swapChain, &device, nullptr, &context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &swapChain, &device, nullptr, &context);
    if (FAILED(hr)) return false;
    createTarget();
    return true;
}

void destroyDevice() {
    releaseTarget();
    if (swapChain) swapChain->Release();
    if (context) context->Release();
    if (device) device->Release();
}

bool systemDark() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

void renderFrame() {
    static bool rendering = false;
    if (rendering || !app) return;
    rendering = true;
    if (resizeW != 0) {
        releaseTarget();
        swapChain->ResizeBuffers(0, resizeW, resizeH, DXGI_FORMAT_UNKNOWN, 0);
        resizeW = resizeH = 0;
        createTarget();
    }
    if (app->FontsDirty()) {
        app->ReloadFonts();
        ImGui_ImplDX11_InvalidateDeviceObjects();
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    app->Frame();
    ImGui::Render();
    const float clear[4] = {dark ? 0x15 / 255.0f : 0xF2 / 255.0f, dark ? 0x17 / 255.0f : 0xF3 / 255.0f,
                            dark ? 0x1C / 255.0f : 0xF5 / 255.0f, 1};
    context->OMSetRenderTargets(1, &target, nullptr);
    context->ClearRenderTargetView(target, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain->Present(1, 0);
    rendering = false;
}

void applyTheme() {
    dark = systemDark();
    BOOL v = dark;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &v, sizeof(v));
    if (app) app->SetDark(dark);
}

// hitTest: a band inside the edges resizes (there's no frame), and the
// title bar's empty space drags the window.
LRESULT hitTest(HWND h, LPARAM lParam) {
    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ScreenToClient(h, &pt);
    RECT rc;
    GetClientRect(h, &rc);
    UINT dpi = GetDpiForWindow(h);
    int band = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    int corner = band * 2;
    bool left = pt.x < band, right = pt.x >= rc.right - band;
    bool top = pt.y < band, bottom = pt.y >= rc.bottom - band;
    bool nearLeft = pt.x < corner, nearRight = pt.x >= rc.right - corner;
    bool nearTop = pt.y < corner, nearBottom = pt.y >= rc.bottom - corner;
    if ((top && nearLeft) || (left && nearTop)) return HTTOPLEFT;
    if ((top && nearRight) || (right && nearTop)) return HTTOPRIGHT;
    if ((bottom && nearLeft) || (left && nearBottom)) return HTBOTTOMLEFT;
    if ((bottom && nearRight) || (right && nearBottom)) return HTBOTTOMRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (app && app->InCaption(float(pt.x), float(pt.y))) return HTCAPTION;
    return HTCLIENT;
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wParam) {
            // Maximised, keep within the screen rather than over its edges.
            if (IsZoomed(h)) {
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                UINT dpi = GetDpiForWindow(h);
                int f = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                p->rgrc[0].left += f;
                p->rgrc[0].right -= f;
                p->rgrc[0].top += f;
                p->rgrc[0].bottom -= f;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: return hitTest(h, lParam);
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            resizeW = LOWORD(lParam);
            resizeH = HIWORD(lParam);
            if (IsWindowVisible(h)) renderFrame();
        }
        return 0;
    case WM_EXITSIZEMOVE:
        if (app && !IsZoomed(h)) {
            RECT r;
            GetWindowRect(h, &r);
            app->SavePlace(r);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(h);
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize = {LONG(560 * scale), LONG(220 * scale)};
        return 0;
    }
    case WM_DPICHANGED: {
        if (app) {
            app->SetScale(float(HIWORD(wParam)) / 96.0f);
            ImGui_ImplDX11_InvalidateDeviceObjects();
        }
        const RECT* r = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) applyTheme();
        return 0;
    case WM_DEVICECHANGE:
        if (app && (wParam == DBT_DEVNODES_CHANGED || wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE))
            app->DevicesChanged();
        return TRUE;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wParam, lParam);
}

// selfTest goes through the same worker as the window: sets each lit
// device's first zone to static red, waits for the save, reads it back,
// and puts the zone back as it was. Results go to the given file.
int selfTest(const wchar_t* outPath) {
    FILE* out = _wfopen(outPath, L"w");
    if (!out) return 1;
    std::vector<sq::Device> ds;
    sq::Devices devices([] {});
    auto waitRead = [&] {
        for (int i = 0; i < 100; i++) {
            Sleep(100);
            if (devices.Changed(ds)) return true;
        }
        return false;
    };
    if (!waitRead()) {
        fprintf(out, "FAIL no read\n");
        fclose(out);
        return 1;
    }
    int failures = 0;
    for (const auto& d : ds) {
        fprintf(out, "%s pid %04x battery %d firmware %s zones %zu\n", d.name.c_str(), d.pid, d.battery, d.firmware.c_str(), d.zones.size());
        if (d.zones.empty()) continue;
        sq::Zone before = d.zones[0];
        sq::Light red{sq::EffectStatic, 255, 0, 0};
        devices.SetLight(d, before.id, red);
        Sleep(2500);  // past the save delay
        devices.Refresh();
        waitRead();
        for (const auto& e : ds)
            if (e.pid == d.pid && !e.zones.empty()) {
                const auto& l = e.zones[0].light;
                bool ok = l.effect == sq::EffectStatic && l.r == 255 && l.g == 0 && l.b == 0;
                fprintf(out, "  %s set static red: %s (effect %d rgb %d,%d,%d)\n", e.zones[0].name.c_str(), ok ? "ok" : "FAIL", l.effect, l.r, l.g, l.b);
                failures += !ok;
            }
        devices.SetLight(d, before.id, before.light);
        Sleep(2500);
        devices.Refresh();
        waitRead();
        for (const auto& e : ds)
            if (e.pid == d.pid && !e.zones.empty()) {
                bool ok = e.zones[0].light.effect == before.light.effect;
                fprintf(out, "  put back: %s (effect %d)\n", ok ? "ok" : "FAIL", e.zones[0].light.effect);
                failures += !ok;
            }
    }
    fprintf(out, failures ? "FAILED\n" : "PASSED\n");
    fclose(out);
    return failures ? 1 : 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
    if (const wchar_t* t = wcsstr(cmdLine, L"-selftest ")) return selfTest(t + 10);
    // One at a time: a second start brings the first forward.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\squeak");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kClassName, nullptr)) {
            if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }
    ImGui_ImplWin32_EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    // At first, in the middle of the main screen; after that, where it was left.
    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    int w = int(760 * scale), h = int(620 * scale);
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
    hwnd = CreateWindowExW(0, kClassName, L"Squeak", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, instance, nullptr);
    MARGINS margins{0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    if (!createDevice()) {
        MessageBoxW(nullptr, L"Couldn't start Direct3D 11.", L"Squeak", MB_ICONERROR);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device, context);

    app = std::make_unique<sq::App>(hwnd, [] { PostMessageW(hwnd, WM_APP_WAKE, 0, 0); });
    app->SetScale(ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd));
    applyTheme();
    RECT placed;
    if (app->Place(placed))
        SetWindowPos(hwnd, nullptr, placed.left, placed.top, placed.right - placed.left, placed.bottom - placed.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Draw on input and when the worker has news; otherwise sleep (a
    // second at a time, for the battery refresh and plug-in settling).
    bool running = true;
    while (running) {
        bool visible = IsWindowVisible(hwnd) && !IsIconic(hwnd);
        DWORD timeout = framesWanted > 0 ? 0 : 1000;
        MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            framesWanted = 2;
        }
        if (!running) break;
        app->Pump();
        if (!visible) continue;
        renderFrame();
        if (framesWanted > 0) framesWanted--;
    }

    app.reset();  // saves any lighting still waiting
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroyDevice();
    CoUninitialize();
    ReleaseMutex(mutex);
    return 0;
}
