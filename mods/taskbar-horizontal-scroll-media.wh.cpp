// ==WindhawkMod==
// @id              taskbar-horizontal-scroll-media
// @name            Taskbar Horizontal Scroll Media Control
// @description     Change tracks with taskbar wheel tilt and show the genuine Windows media panel control independently.
// @version         1.0.0
// @author          Shreyas J S
// @github          https://github.com/shreyasjswork
// @license         GPL-3.0
// @include         explorer.exe
// @include         ShellHost.exe
// @include         ShellExperienceHost.exe
// @architecture    x86-64
// @compilerOptions -lwindowsapp -lruntimeobject -ldwmapi -lshcore -lshell32 -luiautomationcore -luser32 -lole32 -loleaut32 -luuid
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Horizontal Scroll Media Control

Tilt the mouse wheel horizontally while the pointer is over the taskbar:

- Right: next track
- Left: previous track
- Ctrl + middle-click on empty taskbar space: play/pause

Ctrl is required by default for track-changing wheel tilt. The requirement can
be disabled, or the wheel modifier can be changed to Shift, Alt, or the Windows
key. Play/pause intentionally remains fixed to Ctrl + middle-click.

A held wheel tilt triggers exactly once. Repeated horizontal-wheel packets are
discarded until the configured release gap has passed.

The mod uses Windows' genuine current Quick Settings
`ControlCenter.MediaTransportControls`; it does not draw a replacement card and does not
open a Chromium bubble.

- When Quick Settings is already open, its media card updates in place.
- When Quick Settings is closed, the same Windows media control is hosted alone
  in a non-activating popup beside the taskbar.
- Opening Quick Settings dismisses the independent control.

The implementation is split between Explorer, which owns the taskbar gesture,
and ShellHost, the Windows 11 process that owns Quick Settings on current
builds (ShellExperienceHost on older Windows 11 builds). A visual-tree watcher
captures Windows' system-created control; while
the panel is closed, that same element is temporarily reparented into a desktop
XAML island and then restored to its original parent and child index.

On the first use after the owning shell process starts, Windows must create the
Quick Settings visual tree before its genuine media control exists. The mod
briefly asks Windows to create that tree, captures the control, and hides the
owning panel before showing the standalone card.

## Difference from Taskbar Scroll Actions

Taskbar Scroll Actions provides general vertical taskbar-wheel mappings. This
mod is specifically for horizontal wheel tilt, one-action-per-held-tilt
suppression, Ctrl + middle-click play/pause, and standalone presentation of
Windows' genuine Quick Settings media control.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- requireModifier: true
  $name: Require a modifier key
  $description: Turn off to change tracks with horizontal scrolling without a modifier. Ctrl + middle-click play/pause is unchanged.
- modifier: ctrl
  $name: Modifier key
  $description: Used for track-changing wheel tilt only when "Require a modifier key" is enabled. Play/pause remains Ctrl + middle-click.
  $options:
  - ctrl: Ctrl
  - shift: Shift
  - alt: Alt
  - win: Windows key
- reverseDirection: false
  $name: Reverse left and right
  $description: Enable this if the mouse reports tilt direction in reverse.
- releaseTimeoutMs: 350
  $name: Gesture release gap (milliseconds)
  $description: 'Repeated input is ignored until no packet arrives for this long. Accepted range: 80-2000.'
- nativeFlyoutDelayMs: 600
  $name: Native control delay (milliseconds)
  $description: 'Allows Windows to publish the new track before showing its control. Accepted range: 250-2500.'
- nativeFlyoutTimeoutMs: 3000
  $name: Native control timeout (milliseconds)
  $description: 'How long the independent Windows media control remains visible. Accepted range: 1500-15000.'
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <dwmapi.h>
#include <roapi.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <uiautomation.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <xamlom.h>
#include <ocidl.h>

// winbase.h defines a legacy GetCurrentTime macro that collides with the
// Windows.UI.Xaml media-animation ABI.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/base.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <string>

namespace {

constexpr wchar_t kShowEventObjectName[] =
    L"Local\\Windhawk.TaskbarHorizontalScrollMedia.ShowNativeControl.v4";
constexpr wchar_t kCaptureReadyEventObjectName[] =
    L"Local\\Windhawk.TaskbarHorizontalScrollMedia.CaptureReady.v1";
constexpr wchar_t kCapturedHostClass[] =
    L"Windhawk.TaskbarHorizontalScrollMedia.CapturedNativeHost";
constexpr wchar_t kPrivateMediaControlClass[] =
    L"ControlCenter.MediaTransportControls";
constexpr UINT kResetGestureMessage = WM_APP + 1;
constexpr UINT_PTR kDismissTimer = 1;

enum class ProcessRole { Explorer, ShellHost, Unsupported };
enum class ModifierKey { Ctrl, Shift, Alt, Win };

struct Settings {
    std::atomic<bool> requireModifier{true};
    std::atomic<ModifierKey> modifier{ModifierKey::Ctrl};
    std::atomic<bool> reverseDirection{false};
    std::atomic<DWORD> releaseTimeoutMs{350};
    std::atomic<DWORD> flyoutDelayMs{600};
    std::atomic<DWORD> flyoutTimeoutMs{3000};
} g_settings;

ProcessRole g_role = ProcessRole::Unsupported;
HANDLE g_stopEvent;
HANDLE g_showEvent;
HANDLE g_captureReadyEvent;

HHOOK g_mouseHook;
HANDLE g_hookThread;
HANDLE g_routerThread;
std::atomic<DWORD> g_hookThreadId{0};
HANDLE g_hookReadyEvent;
std::atomic<bool> g_hookInstalled{false};
HANDLE g_routeRequestEvent;
HANDLE g_toggleRequestEvent;
std::atomic<LONG> g_togglePointX{0};
std::atomic<LONG> g_togglePointY{0};

HANDLE g_nativeThread;
std::atomic<bool> g_pendingStandalone{false};
std::atomic<bool> g_bootstrapCapture{false};
std::atomic<bool> g_mediaCaptured{false};
std::atomic<bool> g_shuttingDown{false};
[[clang::no_destroy]] std::mutex g_captureMutex;
[[clang::no_destroy]] winrt::Windows::UI::Xaml::FrameworkElement
    g_capturedMedia{nullptr};
[[clang::no_destroy]] winrt::Windows::UI::Core::CoreDispatcher
    g_capturedDispatcher{nullptr};
[[clang::no_destroy]] winrt::Windows::UI::Xaml::Controls::Panel
    g_originalMediaParent{nullptr};
uint32_t g_originalMediaIndex = 0;
uint64_t g_capturedMediaHandle = 0;
bool g_reparentingCapturedMedia = false;
HWND g_capturedHostWindow = nullptr;
HWND g_capturedIslandWindow = nullptr;
HINSTANCE g_capturedHostInstance = nullptr;
bool g_capturedHostClassRegistered = false;
[[clang::no_destroy]] winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource
    g_capturedXamlSource{nullptr};
ULONGLONG g_capturedExpiresAt = 0;
double g_originalMediaWidth = NAN;
double g_originalMediaHeight = NAN;
winrt::Windows::UI::Xaml::Thickness g_originalMediaMargin{};
winrt::Windows::UI::Xaml::HorizontalAlignment g_originalHorizontalAlignment{};
winrt::Windows::UI::Xaml::VerticalAlignment g_originalVerticalAlignment{};

void LogNativeState(int stage, HRESULT result = S_OK) {
    Wh_Log(L"Native state: stage=%d, result=0x%08X, process=%u", stage,
           static_cast<unsigned>(result), GetCurrentProcessId());
}

void LogRouterState(int stage, DWORD error = ERROR_SUCCESS) {
    Wh_Log(L"Router state: stage=%d, error=%u, process=%u", stage, error,
           GetCurrentProcessId());
}


// Hook-thread-only gesture state.
bool g_gestureLatched;
int g_wheelDeltaAccumulator;
ULONGLONG g_lastHorizontalInputTick;
ULONGLONG g_suppressHorizontalUntil;

DWORD ClampSetting(int value, DWORD minimum, DWORD maximum,
                   DWORD missingDefault) {
    if (value <= 0) return missingDefault;
    if (value < static_cast<int>(minimum)) return minimum;
    if (value > static_cast<int>(maximum)) return maximum;
    return static_cast<DWORD>(value);
}

std::wstring CurrentProcessName() {
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (!length) return {};
    const wchar_t* name = wcsrchr(path, L'\\');
    return name ? name + 1 : path;
}

DWORD CurrentWindowsBuild() {
    using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto rtlGetVersion = ntdll ? reinterpret_cast<RtlGetVersion_t>(
                                     GetProcAddress(ntdll, "RtlGetVersion"))
                               : nullptr;
    if (!rtlGetVersion) return 0;
    OSVERSIONINFOW version{sizeof(version)};
    return rtlGetVersion(&version) >= 0 ? version.dwBuildNumber : 0;
}

ProcessRole DetectProcessRole() {
    std::wstring name = CurrentProcessName();
    if (_wcsicmp(name.c_str(), L"explorer.exe") == 0)
        return ProcessRole::Explorer;
    DWORD build = CurrentWindowsBuild();
    if (_wcsicmp(name.c_str(), L"ShellHost.exe") == 0 &&
        (!build || build >= 26100)) {
        return ProcessRole::ShellHost;
    }
    if (_wcsicmp(name.c_str(), L"ShellExperienceHost.exe") == 0 && build &&
        build < 26100) {
        return ProcessRole::ShellHost;
    }
    return ProcessRole::Unsupported;
}

bool IsShellExplorerProcess() {
    HWND shellWindow = GetShellWindow();
    if (!shellWindow) return false;
    DWORD shellProcessId = 0;
    GetWindowThreadProcessId(shellWindow, &shellProcessId);
    return shellProcessId == GetCurrentProcessId();
}

void LoadSettings() {
    g_settings.requireModifier = Wh_GetIntSetting(L"requireModifier") != 0;
    PCWSTR value = Wh_GetStringSetting(L"modifier");
    ModifierKey modifier = ModifierKey::Ctrl;
    if (value) {
        if (wcscmp(value, L"shift") == 0) modifier = ModifierKey::Shift;
        else if (wcscmp(value, L"alt") == 0) modifier = ModifierKey::Alt;
        else if (wcscmp(value, L"win") == 0) modifier = ModifierKey::Win;
        Wh_FreeStringSetting(value);
    }
    g_settings.modifier = modifier;
    g_settings.reverseDirection = Wh_GetIntSetting(L"reverseDirection") != 0;
    g_settings.releaseTimeoutMs = ClampSetting(
        Wh_GetIntSetting(L"releaseTimeoutMs"), 80, 2000, 350);
    g_settings.flyoutDelayMs = ClampSetting(
        Wh_GetIntSetting(L"nativeFlyoutDelayMs"), 250, 2500, 600);
    g_settings.flyoutTimeoutMs = ClampSetting(
        Wh_GetIntSetting(L"nativeFlyoutTimeoutMs"), 1500, 15000, 3000);
}

bool IsModifierPressed() {
    if (!g_settings.requireModifier.load()) return true;
    switch (g_settings.modifier.load()) {
        case ModifierKey::Ctrl:
            return (GetAsyncKeyState(VK_LCONTROL) & 0x8000) ||
                   (GetAsyncKeyState(VK_RCONTROL) & 0x8000);
        case ModifierKey::Shift:
            return (GetAsyncKeyState(VK_LSHIFT) & 0x8000) ||
                   (GetAsyncKeyState(VK_RSHIFT) & 0x8000);
        case ModifierKey::Alt:
            return (GetAsyncKeyState(VK_LMENU) & 0x8000) ||
                   (GetAsyncKeyState(VK_RMENU) & 0x8000);
        case ModifierKey::Win:
            return (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
                   (GetAsyncKeyState(VK_RWIN) & 0x8000);
    }
    return false;
}

bool IsTaskbarClass(HWND window) {
    wchar_t className[64];
    if (!GetClassNameW(window, className, ARRAYSIZE(className))) return false;
    return wcscmp(className, L"Shell_TrayWnd") == 0 ||
           wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

struct TaskbarBoundsSearch {
    HMONITOR monitor;
    RECT monitorBounds;
    int top;
    bool found;
};

BOOL CALLBACK FindBottomTaskbarBoundsProc(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<TaskbarBoundsSearch*>(parameter);
    if (!IsTaskbarClass(window) ||
        MonitorFromWindow(window, MONITOR_DEFAULTTONULL) != search->monitor) {
        return TRUE;
    }

    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return TRUE;
    int monitorMidpoint =
        search->monitorBounds.top +
        (search->monitorBounds.bottom - search->monitorBounds.top) / 2;
    if (bounds.top > monitorMidpoint &&
        bounds.bottom >= search->monitorBounds.bottom - 2) {
        search->top = bounds.top;
        search->found = true;
        return FALSE;
    }
    return TRUE;
}

int GetBottomTaskbarTop(HMONITOR monitor, const MONITORINFO& monitorInfo) {
    TaskbarBoundsSearch search{monitor, monitorInfo.rcMonitor,
                               monitorInfo.rcWork.bottom, false};
    EnumWindows(FindBottomTaskbarBoundsProc,
                reinterpret_cast<LPARAM>(&search));
    return search.found && search.top < monitorInfo.rcWork.bottom
               ? search.top
               : monitorInfo.rcWork.bottom;
}

bool IsPointOverTaskbar(POINT point) {
    HWND window = WindowFromPoint(point);
    if (!window) return false;
    for (HWND current = window; current; current = GetParent(current)) {
        if (IsTaskbarClass(current)) return true;
    }
    HWND root = GetAncestor(window, GA_ROOT);
    return root && IsTaskbarClass(root);
}

bool IsCtrlPressed() {
    return (GetAsyncKeyState(VK_LCONTROL) & 0x8000) ||
           (GetAsyncKeyState(VK_RCONTROL) & 0x8000);
}

bool IsInteractiveAutomationControl(CONTROLTYPEID type) {
    switch (type) {
        case UIA_ButtonControlTypeId:
        case UIA_CheckBoxControlTypeId:
        case UIA_ComboBoxControlTypeId:
        case UIA_EditControlTypeId:
        case UIA_HyperlinkControlTypeId:
        case UIA_ListItemControlTypeId:
        case UIA_MenuItemControlTypeId:
        case UIA_RadioButtonControlTypeId:
        case UIA_ScrollBarControlTypeId:
        case UIA_SliderControlTypeId:
        case UIA_SpinnerControlTypeId:
        case UIA_SplitButtonControlTypeId:
        case UIA_TabItemControlTypeId:
        case UIA_ThumbControlTypeId:
        case UIA_TreeItemControlTypeId:
        case UIA_DataItemControlTypeId:
            return true;
    }
    return false;
}

bool HasAutomationAction(IUIAutomationElement* element, PROPERTYID propertyId) {
    VARIANT value;
    VariantInit(&value);
    bool available =
        SUCCEEDED(element->GetCurrentPropertyValue(propertyId, &value)) &&
        value.vt == VT_BOOL && value.boolVal == VARIANT_TRUE;
    VariantClear(&value);
    return available;
}

bool IsEmptyTaskbarPoint(IUIAutomation* automation, POINT point) {
    if (!automation || !IsPointOverTaskbar(point)) return false;

    HWND taskbar = GetAncestor(WindowFromPoint(point), GA_ROOT);
    if (!taskbar || !IsTaskbarClass(taskbar)) return false;

    winrt::com_ptr<IUIAutomationElement> element;
    if (FAILED(automation->ElementFromPoint(point, element.put())) || !element)
        return false;
    winrt::com_ptr<IUIAutomationTreeWalker> walker;
    if (FAILED(automation->get_RawViewWalker(walker.put())) || !walker)
        return false;

    for (int depth = 0; element && depth < 20; ++depth) {
        CONTROLTYPEID type = 0;
        element->get_CurrentControlType(&type);
        if (IsInteractiveAutomationControl(type) ||
            HasAutomationAction(element.get(),
                                UIA_IsInvokePatternAvailablePropertyId) ||
            HasAutomationAction(element.get(),
                                UIA_IsTogglePatternAvailablePropertyId) ||
            HasAutomationAction(element.get(),
                                UIA_IsSelectionItemPatternAvailablePropertyId)) {
            return false;
        }

        UIA_HWND nativeWindow = 0;
        if (SUCCEEDED(
                element->get_CurrentNativeWindowHandle(&nativeWindow)) &&
            reinterpret_cast<HWND>(nativeWindow) == taskbar) {
            return true;
        }

        winrt::com_ptr<IUIAutomationElement> parent;
        if (FAILED(walker->GetParentElement(element.get(), parent.put())) ||
            !parent) {
            break;
        }
        element = std::move(parent);
    }

    // The point is already known to be inside the taskbar, and no interactive
    // accessible element was found in its ancestry.
    return true;
}

bool IsProcessNamed(HWND window, PCWSTR expectedName) {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 processId);
    if (!process) return false;
    wchar_t path[MAX_PATH];
    DWORD length = ARRAYSIZE(path);
    bool matches = false;
    if (QueryFullProcessImageNameW(process, 0, path, &length)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        matches = _wcsicmp(name ? name + 1 : path, expectedName) == 0;
    }
    CloseHandle(process);
    return matches;
}

struct PanelSearchContext {
    HWND nativeHostToSkip;
    bool found;
};

BOOL CALLBACK FindOpenPanelProc(HWND window, LPARAM parameter) {
    auto* context = reinterpret_cast<PanelSearchContext*>(parameter);
    if (window == context->nativeHostToSkip || !IsWindowVisible(window) ||
        IsIconic(window)) return TRUE;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked))) && cloaked)
        return TRUE;

    RECT bounds{};
    if (!GetWindowRect(window, &bounds) || bounds.right - bounds.left < 300 ||
        bounds.bottom - bounds.top < 250) return TRUE;

    wchar_t className[128];
    GetClassNameW(window, className, ARRAYSIZE(className));
    // Current Windows 11 Quick Settings uses this dedicated top-level class.
    // Broad process matching falsely classifies unrelated ShellHost and
    // PowerToys windows as an open panel and suppresses every request.
    if (wcscmp(className, L"ControlCenterWindow") == 0) {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        Wh_Log(L"Open Quick Settings panel found: process=%u, window=%p",
               processId, window);
        context->found = true;
        return FALSE;
    }
    return TRUE;
}

bool IsQuickPanelOpen(HWND nativeHostToSkip = nullptr) {
    PanelSearchContext context{nativeHostToSkip, false};
    EnumWindows(FindOpenPanelProc, reinterpret_cast<LPARAM>(&context));
    return context.found;
}

BOOL CALLBACK FindControlCenterWindowProc(HWND window, LPARAM parameter) {
    wchar_t className[128];
    GetClassNameW(window, className, ARRAYSIZE(className));
    if (wcscmp(className, L"ControlCenterWindow") == 0) {
        *reinterpret_cast<HWND*>(parameter) = window;
        return FALSE;
    }
    return TRUE;
}

void RestoreCapturedMedia();

void UnregisterCapturedHostClass() {
    if (!g_capturedHostClassRegistered || !g_capturedHostInstance) return;
    if (!UnregisterClassW(kCapturedHostClass, g_capturedHostInstance)) {
        Wh_Log(L"UnregisterClass failed: %u", GetLastError());
        return;
    }
    g_capturedHostClassRegistered = false;
    g_capturedHostInstance = nullptr;
}

LRESULT CALLBACK CapturedHostWndProc(HWND window, UINT message, WPARAM wParam,
                                     LPARAM lParam) {
    switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_TIMER:
            if (wParam == kDismissTimer) {
                if (GetTickCount64() >= g_capturedExpiresAt ||
                    IsQuickPanelOpen(window)) {
                    RestoreCapturedMedia();
                }
            }
            return 0;
        case WM_DESTROY:
            KillTimer(window, kDismissTimer);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool EnsureCapturedHostWindow() {
    if (g_capturedHostWindow) return true;

    HINSTANCE instance = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&CapturedHostWndProc),
                       &instance);
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = CapturedHostWndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kCapturedHostClass;
    if (!RegisterClassExW(&windowClass)) {
        LogNativeState(-731, HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    g_capturedHostInstance = instance;
    g_capturedHostClassRegistered = true;

    g_capturedHostWindow = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
            WS_EX_NOREDIRECTIONBITMAP,
        kCapturedHostClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
        instance, nullptr);
    if (!g_capturedHostWindow) {
        LogNativeState(-732, HRESULT_FROM_WIN32(GetLastError()));
        UnregisterCapturedHostClass();
        return false;
    }

    // The captured Windows media control already draws its own rounded outline.
    // Prevent DWM from adding another non-client frame and drop shadow to the
    // transparent XAML-island host, which otherwise appears as a ghost border
    // below and around the genuine card.
    DWMNCRENDERINGPOLICY renderingPolicy = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(g_capturedHostWindow, DWMWA_NCRENDERING_POLICY,
                          &renderingPolicy, sizeof(renderingPolicy));
    DWM_WINDOW_CORNER_PREFERENCE cornerPreference = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(g_capturedHostWindow,
                          DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference,
                          sizeof(cornerPreference));

    try {
        g_capturedXamlSource = winrt::Windows::UI::Xaml::Hosting::
            DesktopWindowXamlSource{};
        auto interop = g_capturedXamlSource.as<IDesktopWindowXamlSourceNative>();
        winrt::check_hresult(interop->AttachToWindow(g_capturedHostWindow));
        winrt::check_hresult(interop->get_WindowHandle(&g_capturedIslandWindow));
    } catch (const winrt::hresult_error& error) {
        LogNativeState(-733, error.code());
        DestroyWindow(g_capturedHostWindow);
        g_capturedHostWindow = nullptr;
        UnregisterCapturedHostClass();
        return false;
    }
    return true;
}

void RestoreCapturedMedia() {
    if (g_capturedHostWindow) {
        KillTimer(g_capturedHostWindow, kDismissTimer);
        ShowWindow(g_capturedHostWindow, SW_HIDE);
    }
    if (g_originalMediaParent && g_capturedMedia) {
        g_reparentingCapturedMedia = true;
        try {
            if (g_capturedXamlSource) g_capturedXamlSource.Content(nullptr);
            auto children = g_originalMediaParent.Children();
            uint32_t index = g_originalMediaIndex;
            if (index > children.Size()) index = children.Size();
            children.InsertAt(index, g_capturedMedia);
            g_capturedMedia.Width(g_originalMediaWidth);
            g_capturedMedia.Height(g_originalMediaHeight);
            g_capturedMedia.Margin(g_originalMediaMargin);
            g_capturedMedia.HorizontalAlignment(g_originalHorizontalAlignment);
            g_capturedMedia.VerticalAlignment(g_originalVerticalAlignment);
        } catch (const winrt::hresult_error& error) {
            LogNativeState(-748, error.code());
        } catch (...) {
            LogNativeState(-748, E_FAIL);
        }
        g_reparentingCapturedMedia = false;
    }
    g_originalMediaParent = nullptr;
    LogNativeState(749);
}

struct NativeCardBounds {
    double x{};
    double y{};
    double width{};
    double height{};
    double score{-1.0};
};

void FindNativeCardBounds(
    const winrt::Windows::UI::Xaml::DependencyObject& parent,
    const winrt::Windows::UI::Xaml::FrameworkElement& root, double rootWidth,
    double rootHeight, int depth, NativeCardBounds& best) {
    if (!parent || depth > 16) return;

    int count = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::
        GetChildrenCount(parent);
    for (int index = 0; index < count; ++index) {
        auto child = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::
            GetChild(parent, index);
        if (auto element =
                child.try_as<winrt::Windows::UI::Xaml::FrameworkElement>()) {
            double width = element.ActualWidth();
            double height = element.ActualHeight();
            if (width >= rootWidth * 0.85 && width <= rootWidth + 1.0 &&
                height >= 100.0 && height <= rootHeight - 2.0) {
                try {
                    auto origin = element.TransformToVisual(root).TransformPoint(
                        winrt::Windows::Foundation::Point{0.0f, 0.0f});
                    if (origin.X >= -1.0 && origin.Y >= -1.0 &&
                        origin.X + width <= rootWidth + 1.0 &&
                        origin.Y + height <= rootHeight + 1.0) {
                        double score = width * height;
                        if (winrt::get_class_name(element) ==
                            L"Windows.UI.Xaml.Controls.Border") {
                            // Prefer the full-width outline which actually
                            // paints the card over equally sized layout grids.
                            score += rootWidth * rootHeight * 10.0;
                        }
                        if (score > best.score) {
                            best = {origin.X, origin.Y, width, height, score};
                        }
                    }
                } catch (...) {
                }
            }
        }
        FindNativeCardBounds(child, root, rootWidth, rootHeight, depth + 1,
                             best);
    }
}

NativeCardBounds MeasureNativeCardBounds(
    const winrt::Windows::UI::Xaml::FrameworkElement& root, double rootWidth,
    double rootHeight) {
    NativeCardBounds bounds{0.0, 0.0, rootWidth, rootHeight, -1.0};
    FindNativeCardBounds(root, root, rootWidth, rootHeight, 0, bounds);
    return bounds;
}

void ShowCapturedMediaOnUiThread() {
    if (g_shuttingDown.load() || !g_pendingStandalone.exchange(false) ||
        !g_capturedMedia) {
        return;
    }
    bool bootstrap = g_bootstrapCapture.exchange(false);
    if (g_originalMediaParent) {
        g_capturedExpiresAt =
            GetTickCount64() + g_settings.flyoutTimeoutMs.load();
        ShowWindow(g_capturedHostWindow, SW_SHOWNOACTIVATE);
        return;
    }

    try {
        auto parent = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::
                          GetParent(g_capturedMedia)
                              .try_as<winrt::Windows::UI::Xaml::Controls::Panel>();
        if (!parent) {
            LogNativeState(-734, E_NOINTERFACE);
            return;
        }

        auto children = parent.Children();
        uint32_t index = 0;
        bool found = false;
        for (; index < children.Size(); ++index) {
            if (children.GetAt(index) == g_capturedMedia) {
                found = true;
                break;
            }
        }
        if (!found) {
            LogNativeState(-735, E_BOUNDS);
            return;
        }
        if (!EnsureCapturedHostWindow()) return;

        g_originalMediaParent = parent;
        g_originalMediaIndex = index;
        g_originalMediaWidth = g_capturedMedia.Width();
        g_originalMediaHeight = g_capturedMedia.Height();
        g_originalMediaMargin = g_capturedMedia.Margin();
        g_originalHorizontalAlignment =
            g_capturedMedia.HorizontalAlignment();
        g_originalVerticalAlignment = g_capturedMedia.VerticalAlignment();

        double rootWidthDip = g_capturedMedia.ActualWidth();
        double rootHeightDip = g_capturedMedia.ActualHeight();
        if (!(rootWidthDip >= 320.0 && rootWidthDip <= 700.0))
            rootWidthDip = 448.0;
        if (!(rootHeightDip >= 100.0 && rootHeightDip <= 500.0))
            rootHeightDip = 224.0;
        NativeCardBounds cardBounds = MeasureNativeCardBounds(
            g_capturedMedia, rootWidthDip, rootHeightDip);

        g_reparentingCapturedMedia = true;
        children.RemoveAt(index);
        g_capturedMedia.Width(rootWidthDip);
        g_capturedMedia.Height(rootHeightDip);
        g_capturedMedia.Margin({0, 0, 0, 0});
        g_capturedMedia.HorizontalAlignment(
            winrt::Windows::UI::Xaml::HorizontalAlignment::Stretch);
        g_capturedMedia.VerticalAlignment(
            winrt::Windows::UI::Xaml::VerticalAlignment::Stretch);
        g_capturedXamlSource.Content(g_capturedMedia);
        g_reparentingCapturedMedia = false;
        // The first standalone presentation can occur while Control Center is
        // still completing its template pass. Resolve that layout now so the
        // crop is based on the rendered card rather than a transient height.
        g_capturedMedia.UpdateLayout();
        NativeCardBounds laidOutBounds = MeasureNativeCardBounds(
            g_capturedMedia, rootWidthDip, rootHeightDip);
        if (laidOutBounds.score >= 0.0) cardBounds = laidOutBounds;

        if (bootstrap) {
            HWND panelWindow = nullptr;
            EnumWindows(FindControlCenterWindowProc,
                        reinterpret_cast<LPARAM>(&panelWindow));
            if (panelWindow) ShowWindow(panelWindow, SW_HIDE);
        }

        POINT cursor{};
        GetCursorPos(&cursor);
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        GetMonitorInfoW(monitor, &monitorInfo);

        // Convert against the destination monitor, not the host's previous
        // monitor. This prevents right/bottom clipping on mixed-DPI setups.
        UINT dpiX = 96;
        UINT dpiY = 96;
        if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
            dpiX = dpiY = GetDpiForWindow(g_capturedHostWindow);
            if (!dpiX) dpiX = dpiY = 96;
        }
        auto dipToPixelsX = [dpiX](double value) {
            return static_cast<int>(std::lround(value * dpiX / 96.0));
        };
        auto dipToPixelsY = [dpiY](double value) {
            return static_cast<int>(std::lround(value * dpiY / 96.0));
        };
        int rootWidth = dipToPixelsX(rootWidthDip);
        int rootHeight = dipToPixelsY(rootHeightDip);
        int cropLeft = dipToPixelsX(cardBounds.x);
        int cropTop = dipToPixelsY(cardBounds.y);
        int width = dipToPixelsX(cardBounds.width);
        // Preserve a small amount of Windows' transparent root below the
        // measured border. It absorbs late first-frame rasterization changes
        // without moving the visible card or restoring the old ghost frame.
        double availableBelow =
            rootHeightDip - (cardBounds.y + cardBounds.height);
        double bottomCropSlackDip =
            availableBelow > 4.0 ? 4.0
                                 : (availableBelow > 0.0 ? availableBelow : 0.0);
        int visibleHeight = dipToPixelsY(cardBounds.height);
        int height = dipToPixelsY(cardBounds.height + bottomCropSlackDip);
        int edgeMarginX = dipToPixelsX(12.0);
        int verticalGap = dipToPixelsY(24.0);
        int taskbarTop = GetBottomTaskbarTop(monitor, monitorInfo);
        int x = monitorInfo.rcWork.right - width - edgeMarginX;
        int y = taskbarTop - visibleHeight - verticalGap;
        Wh_Log(L"Native card: rootHeight=%d, top=%d, height=%d, dpi=%u, "
               L"taskbarTop=%d, bottomSlack=%d",
               static_cast<int>(std::lround(rootHeightDip)),
               static_cast<int>(std::lround(cardBounds.y)),
               static_cast<int>(std::lround(cardBounds.height)), dpiY,
               taskbarTop,
               static_cast<int>(std::lround(bottomCropSlackDip)));
        // Keep the genuine root at its Windows-provided size, but offset the
        // island so the host clips its transparent layout padding. Positioning
        // is therefore based on the visible native card border.
        // Move the parent into the target monitor's DPI context before sizing
        // its XAML child, then reveal both with their final bounds.
        SetWindowPos(g_capturedHostWindow, HWND_TOPMOST, x, y, width, height,
                     SWP_NOACTIVATE);
        SetWindowPos(g_capturedIslandWindow, nullptr, -cropLeft, -cropTop,
                     rootWidth, rootHeight,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
        SetWindowPos(g_capturedHostWindow, HWND_TOPMOST, x, y, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g_capturedExpiresAt =
            GetTickCount64() + g_settings.flyoutTimeoutMs.load();
        SetTimer(g_capturedHostWindow, kDismissTimer, 100, nullptr);
        LogNativeState(740);
    } catch (const winrt::hresult_error& error) {
        g_reparentingCapturedMedia = false;
        LogNativeState(-740, error.code());
        RestoreCapturedMedia();
    } catch (...) {
        g_reparentingCapturedMedia = false;
        LogNativeState(-740, E_FAIL);
        RestoreCapturedMedia();
    }
}

void RequestCapturedStandalone() {
    if (g_shuttingDown.load()) return;
    g_pendingStandalone = true;
    winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
    bool captured = false;
    {
        std::lock_guard lock(g_captureMutex);
        captured = g_mediaCaptured.load() && g_capturedMedia &&
                   g_capturedDispatcher;
        if (captured) dispatcher = g_capturedDispatcher;
    }
    if (!captured) g_bootstrapCapture = true;
    if (!captured || !dispatcher) {
        LogNativeState(710);
        return;
    }
    try {
        dispatcher.RunAsync(
            winrt::Windows::UI::Core::CoreDispatcherPriority::Low,
            [] { ShowCapturedMediaOnUiThread(); });
    } catch (const winrt::hresult_error& error) {
        LogNativeState(-711, error.code());
    }
}

HRESULT InjectMediaCaptureTAP();

void RequestNativeControl() {
    if (g_routeRequestEvent) SetEvent(g_routeRequestEvent);
}

void SendMediaVirtualKey(WORD key) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = key;
    inputs[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    if (SendInput(ARRAYSIZE(inputs), inputs, sizeof(INPUT)) ==
        ARRAYSIZE(inputs)) {
        RequestNativeControl();
    } else {
        Wh_Log(L"SendInput failed: %u", GetLastError());
    }
}

bool ToggleCurrentMediaSession() {
    if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) return false;
    try {
        auto manager = winrt::Windows::Media::Control::
            GlobalSystemMediaTransportControlsSessionManager::RequestAsync()
                .get();
        if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) return false;
        auto session = manager.GetCurrentSession();
        if (!session) {
            Wh_Log(L"No current media session was found");
            return false;
        }

        if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) return false;
        bool succeeded = session.TryTogglePlayPauseAsync().get();
        Wh_Log(L"Play/pause request completed: succeeded=%d", succeeded);
        if (succeeded) RequestNativeControl();
        return succeeded;
    } catch (const winrt::hresult_error& error) {
        Wh_Log(L"Play/pause request failed: 0x%08X",
               static_cast<unsigned>(error.code()));
        return false;
    }
}

void SendTrackMediaKey(bool nextTrack) {
    SendMediaVirtualKey(nextTrack ? VK_MEDIA_NEXT_TRACK
                                  : VK_MEDIA_PREV_TRACK);
}

void ResetGesture() {
    g_gestureLatched = false;
    g_wheelDeltaAccumulator = 0;
    g_lastHorizontalInputTick = 0;
}

LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0)
        return CallNextHookEx(g_mouseHook, code, wParam, lParam);

    auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    if (wParam == WM_MBUTTONDOWN) {
        if (IsCtrlPressed() && IsPointOverTaskbar(mouse->pt)) {
            // Some tilting wheels emit a horizontal-wheel packet as the wheel
            // is pressed. Reserve this gesture for play/pause and discard that
            // companion packet instead of treating it as next/previous.
            g_suppressHorizontalUntil = GetTickCount64() + 500;
            ResetGesture();
            if (g_toggleRequestEvent) {
                g_togglePointX = mouse->pt.x;
                g_togglePointY = mouse->pt.y;
                SetEvent(g_toggleRequestEvent);
            }
        }
        // Empty-space validation happens asynchronously through UI Automation.
        // Preserve the taskbar's normal middle-click behavior on its buttons.
        return CallNextHookEx(g_mouseHook, code, wParam, lParam);
    }
    if (wParam == WM_MBUTTONUP) {
        if (g_suppressHorizontalUntil) {
            g_suppressHorizontalUntil = GetTickCount64() + 250;
        }
        return CallNextHookEx(g_mouseHook, code, wParam, lParam);
    }
    if (wParam != WM_MOUSEHWHEEL)
        return CallNextHookEx(g_mouseHook, code, wParam, lParam);

    if (!IsPointOverTaskbar(mouse->pt))
        return CallNextHookEx(g_mouseHook, code, wParam, lParam);

    ULONGLONG now = GetTickCount64();
    if ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) ||
        (g_suppressHorizontalUntil && now <= g_suppressHorizontalUntil)) {
        ResetGesture();
        return 1;
    }
    g_suppressHorizontalUntil = 0;
    if (!g_lastHorizontalInputTick ||
        now - g_lastHorizontalInputTick > g_settings.releaseTimeoutMs.load()) {
        g_gestureLatched = false;
        g_wheelDeltaAccumulator = 0;
    }
    g_lastHorizontalInputTick = now;
    if (g_gestureLatched) return 1;
    if (!IsModifierPressed()) {
        g_wheelDeltaAccumulator = 0;
        return CallNextHookEx(g_mouseHook, code, wParam, lParam);
    }

    g_wheelDeltaAccumulator += static_cast<SHORT>(HIWORD(mouse->mouseData));
    if (g_wheelDeltaAccumulator >= WHEEL_DELTA ||
        g_wheelDeltaAccumulator <= -WHEEL_DELTA) {
        bool nextTrack = g_wheelDeltaAccumulator > 0;
        if (g_settings.reverseDirection.load()) nextTrack = !nextTrack;
        g_gestureLatched = true;
        g_wheelDeltaAccumulator = 0;
        SendTrackMediaKey(nextTrack);
    }
    return 1;
}

DWORD WINAPI HookThreadProc(void*) {
    MSG message;
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    g_hookThreadId = GetCurrentThreadId();
    ResetGesture();
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LowLevelMouseProc), &module);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, module, 0);
    g_hookInstalled = g_mouseHook != nullptr;
    if (!g_mouseHook) Wh_Log(L"SetWindowsHookEx failed: %u", GetLastError());
    SetEvent(g_hookReadyEvent);
    while (g_mouseHook && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == kResetGestureMessage) ResetGesture();
    }
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    g_mouseHook = nullptr;
    return 0;
}

DWORD WINAPI RouterThreadProc(void*) {
    HRESULT apartmentResult = RoInitialize(RO_INIT_MULTITHREADED);
    bool uninitializeApartment = SUCCEEDED(apartmentResult);
    winrt::com_ptr<IUIAutomation> automation;
    HRESULT automationResult = CoCreateInstance(
        CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(automation.put()));
    if (FAILED(automationResult)) {
        automationResult = CoCreateInstance(
            CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(automation.put()));
    }
    Wh_Log(L"Taskbar UI Automation initialization: 0x%08X",
           static_cast<unsigned>(automationResult));

    HANDLE events[] = {g_stopEvent, g_routeRequestEvent, g_toggleRequestEvent};
    while (true) {
        DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE,
                                            INFINITE);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_OBJECT_0 + 2) {
            if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) break;
            POINT point{g_togglePointX.load(), g_togglePointY.load()};
            if (IsEmptyTaskbarPoint(automation.get(), point)) {
                LogRouterState(510);
                ToggleCurrentMediaSession();
            } else {
                LogRouterState(505);
            }
            continue;
        }
        if (wait != WAIT_OBJECT_0 + 1) break;
        if (WaitForSingleObject(g_stopEvent, g_settings.flyoutDelayMs.load()) ==
            WAIT_OBJECT_0) break;
        // Never create another surface while the user's panel is visible. The
        // system media session updates its existing card by itself.
        if (!IsQuickPanelOpen()) {
            if (g_showEvent) {
                if (SetEvent(g_showEvent)) {
                    LogRouterState(450);
                    if (!g_captureReadyEvent ||
                        WaitForSingleObject(g_captureReadyEvent, 0) !=
                            WAIT_OBJECT_0) {
                        // Ask Windows to create the owning ControlCenterView.
                        // The ShellHost watcher captures its genuine media
                        // control before the standalone request is applied.
                        if (WaitForSingleObject(g_stopEvent, 0) ==
                            WAIT_OBJECT_0) {
                            break;
                        }
                        HINSTANCE result = ShellExecuteW(
                            nullptr, L"open", L"ms-controlcenter:", nullptr,
                            nullptr, SW_SHOWNORMAL);
                        if (reinterpret_cast<INT_PTR>(result) <= 32) {
                            Wh_Log(L"Opening Quick Settings failed: %Id",
                                   reinterpret_cast<INT_PTR>(result));
                        }
                    }
                } else {
                    DWORD error = GetLastError();
                    LogRouterState(-450, error);
                    CloseHandle(g_showEvent);
                    g_showEvent = nullptr;
                }
            }
        } else {
            LogRouterState(405);
        }
    }
    if (uninitializeApartment) RoUninitialize();
    return 0;
}

DWORD WINAPI NativeHostThreadProc(void*) {
    LogNativeState(200);
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        LogNativeState(210);
    } catch (const winrt::hresult_error& error) {
        LogNativeState(-210, error.code());
        Wh_Log(L"Native host apartment initialization failed: 0x%08X",
               error.code().value);
        return 0;
    }

    HANDLE events[] = {g_stopEvent, g_showEvent};
    bool running = true;
    while (running) {
        DWORD wait = MsgWaitForMultipleObjects(ARRAYSIZE(events), events, FALSE,
                                               INFINITE, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_OBJECT_0 + 1) {
            LogNativeState(300);
            RequestCapturedStandalone();
            if (!g_mediaCaptured.load()) {
                HRESULT tapResult = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
                for (int attempt = 0; attempt < 30 &&
                                      !g_mediaCaptured.load();
                     ++attempt) {
                    if (WaitForSingleObject(g_stopEvent, 100) == WAIT_OBJECT_0)
                        break;
                    tapResult = InjectMediaCaptureTAP();
                    Wh_Log(L"XAML diagnostics injection: 0x%08X",
                           static_cast<unsigned>(tapResult));
                    if (SUCCEEDED(tapResult)) break;
                }
                if (FAILED(tapResult) && !g_mediaCaptured.load())
                    LogNativeState(-712, tapResult);
            }
        } else if (wait == WAIT_OBJECT_0 + ARRAYSIZE(events)) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    running = false;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        } else {
            break;
        }
    }
    winrt::uninit_apartment();
    return 0;
}

void ClearCapturedMediaOnUiThread() {
    RestoreCapturedMedia();
    std::lock_guard lock(g_captureMutex);
    g_capturedMedia = nullptr;
    g_capturedDispatcher = nullptr;
    g_capturedMediaHandle = 0;
    g_mediaCaptured = false;
    if (g_captureReadyEvent) ResetEvent(g_captureReadyEvent);
}

class MediaVisualTreeWatcher
    : public winrt::implements<MediaVisualTreeWatcher,
                               IVisualTreeServiceCallback2,
                               winrt::non_agile> {
   public:
    explicit MediaVisualTreeWatcher(winrt::com_ptr<IUnknown> site)
        : diagnostics_(site.as<IXamlDiagnostics>()) {
        HANDLE thread = CreateThread(
            nullptr, 0,
            [](void* parameter) -> DWORD {
                auto* watcher =
                    reinterpret_cast<MediaVisualTreeWatcher*>(parameter);
                HRESULT result = watcher->diagnostics_
                                     .as<IVisualTreeService3>()
                                     ->AdviseVisualTreeChange(watcher);
                Wh_Log(L"XAML visual-tree watcher registration: 0x%08X",
                       static_cast<unsigned>(result));
                watcher->Release();
                return 0;
            },
            this, 0, nullptr);
        if (thread) {
            AddRef();
            CloseHandle(thread);
        }
    }

    void Unadvise() {
        diagnostics_.as<IVisualTreeService3>()->UnadviseVisualTreeChange(this);
    }

    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
        ParentChildRelation, VisualElement element,
        VisualMutationType mutationType) noexcept override {
        try {
            if (mutationType == Remove) {
                if (!g_reparentingCapturedMedia &&
                    element.Handle == g_capturedMediaHandle) {
                    ClearCapturedMediaOnUiThread();
                    LogNativeState(721);
                }
                return S_OK;
            }
            if (mutationType != Add || !element.Type ||
                wcscmp(element.Type, kPrivateMediaControlClass) != 0) {
                return S_OK;
            }
            if (element.Handle == g_capturedMediaHandle) return S_OK;
            winrt::Windows::Foundation::IInspectable inspectable{nullptr};
            winrt::check_hresult(diagnostics_->GetIInspectableFromHandle(
                element.Handle,
                reinterpret_cast<::IInspectable**>(winrt::put_abi(inspectable))));
            auto media = inspectable.try_as<
                winrt::Windows::UI::Xaml::FrameworkElement>();
            if (!media) return S_OK;

            if (g_capturedMedia) ClearCapturedMediaOnUiThread();
            winrt::Windows::UI::Core::CoreDispatcher dispatcher =
                media.Dispatcher();
            {
                std::lock_guard lock(g_captureMutex);
                g_capturedMedia = media;
                g_capturedDispatcher = dispatcher;
                g_capturedMediaHandle = element.Handle;
                g_mediaCaptured = true;
            }
            if (g_captureReadyEvent) SetEvent(g_captureReadyEvent);
            LogNativeState(720);
            if (g_pendingStandalone.load() && !g_shuttingDown.load()) {
                dispatcher.RunAsync(
                    winrt::Windows::UI::Core::CoreDispatcherPriority::Low,
                    [] { ShowCapturedMediaOnUiThread(); });
            }
        } catch (const winrt::hresult_error& error) {
            LogNativeState(-720, error.code());
        } catch (...) {
            LogNativeState(-720, E_FAIL);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(
        InstanceHandle, VisualElementState, LPCWSTR) noexcept override {
        return S_OK;
    }

   private:
    winrt::com_ptr<IXamlDiagnostics> diagnostics_;
};

[[clang::no_destroy]] winrt::com_ptr<MediaVisualTreeWatcher>
    g_visualTreeWatcher;

// {81D412E7-94A1-45D9-8997-F87EEB61B332}
constexpr CLSID CLSID_MediaCaptureTAP = {
    0x81d412e7,
    0x94a1,
    0x45d9,
    {0x89, 0x97, 0xf8, 0x7e, 0xeb, 0x61, 0xb3, 0x32}};

class MediaCaptureTAP
    : public winrt::implements<MediaCaptureTAP, IObjectWithSite,
                               winrt::non_agile> {
   public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept override {
        try {
            if (g_visualTreeWatcher) {
                g_visualTreeWatcher->Unadvise();
                g_visualTreeWatcher = nullptr;
            }
            site_.copy_from(site);
            if (site_) {
                HMODULE module = nullptr;
                GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&CapturedHostWndProc), &module);
                if (module) FreeLibrary(module);
                g_visualTreeWatcher =
                    winrt::make_self<MediaVisualTreeWatcher>(site_);
            }
            return S_OK;
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid, void** result) noexcept override {
        return site_.as(iid, result);
    }

   private:
    winrt::com_ptr<IUnknown> site_;
};

template <typename T>
struct SimpleClassFactory
    : winrt::implements<SimpleClassFactory<T>, IClassFactory,
                        winrt::non_agile> {
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid,
                                             void** result) noexcept override {
        try {
            if (outer) return CLASS_E_NOAGGREGATION;
            *result = nullptr;
            return winrt::make<T>().as(iid, result);
        } catch (...) {
            return winrt::to_hresult();
        }
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdll-attribute-on-redeclaration"

extern "C" __declspec(dllexport) HRESULT WINAPI
DllGetClassObject(REFCLSID clsid, REFIID iid, void** result) {
    if (clsid != CLSID_MediaCaptureTAP) return CLASS_E_CLASSNOTAVAILABLE;
    *result = nullptr;
    return winrt::make<SimpleClassFactory<MediaCaptureTAP>>().as(iid, result);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow() {
    return winrt::get_module_lock() ? S_FALSE : S_OK;
}

#pragma clang diagnostic pop

HRESULT InjectMediaCaptureTAP() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&InjectMediaCaptureTAP),
                            &module)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    wchar_t location[MAX_PATH];
    DWORD length = GetModuleFileNameW(module, location, ARRAYSIZE(location));
    if (!length || length >= ARRAYSIZE(location))
        return HRESULT_FROM_WIN32(GetLastError());

    HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr,
                                  LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!xaml) return HRESULT_FROM_WIN32(GetLastError());
    using InitializeXamlDiagnosticsEx_t =
        decltype(&InitializeXamlDiagnosticsEx);
    auto initialize = reinterpret_cast<InitializeXamlDiagnosticsEx_t>(
        GetProcAddress(xaml, "InitializeXamlDiagnosticsEx"));
    if (!initialize) return HRESULT_FROM_WIN32(GetLastError());

    HRESULT result = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    for (int index = 1; index <= 10000; ++index) {
        wchar_t connection[64];
        swprintf_s(connection, L"VisualDiagConnection%d", index);
        result = initialize(connection, GetCurrentProcessId(), L"", location,
                            CLSID_MediaCaptureTAP, nullptr);
        if (result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) break;
    }
    return result;
}

void CloseHandleIfPresent(HANDLE& handle) {
    if (handle) CloseHandle(handle);
    handle = nullptr;
}

struct EventSignalOnExit {
    HANDLE event;
    ~EventSignalOnExit() {
        SetEvent(event);
    }
};

HANDLE CreateSharedEvent(PCWSTR name, DWORD flags) {
    // Both injected processes run as the interactive user in the same session.
    // The default DACL keeps unrelated and sandboxed processes from signalling
    // the control channel.
    return CreateEventExW(nullptr, name, flags,
                          EVENT_MODIFY_STATE | SYNCHRONIZE);
}

bool InitializeExplorerRole() {
    g_routeRequestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_toggleRequestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_hookReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_routeRequestEvent || !g_toggleRequestEvent || !g_hookReadyEvent)
        return false;
    g_routerThread = CreateThread(nullptr, 0, RouterThreadProc, nullptr, 0,
                                  nullptr);
    g_hookThread = CreateThread(nullptr, 0, HookThreadProc, nullptr, 0, nullptr);
    if (!g_routerThread || !g_hookThread) return false;
    DWORD ready = WaitForSingleObject(g_hookReadyEvent, 1000);
    return ready == WAIT_OBJECT_0 && g_hookInstalled.load();
}

bool InitializeShellRole() {
    LogNativeState(120);
    ResetEvent(g_captureReadyEvent);
    HRESULT tapResult = InjectMediaCaptureTAP();
    Wh_Log(L"Initial XAML diagnostics injection: 0x%08X",
           static_cast<unsigned>(tapResult));
    if (FAILED(tapResult)) LogNativeState(-701, tapResult);
    g_nativeThread = CreateThread(nullptr, 0, NativeHostThreadProc, nullptr, 0,
                                  nullptr);
    if (g_nativeThread) LogNativeState(130);
    return g_nativeThread != nullptr;
}

void ShutdownMediaCapture() {
    if (g_role != ProcessRole::ShellHost) return;
    if (g_visualTreeWatcher) {
        try {
            g_visualTreeWatcher->Unadvise();
        } catch (...) {
        }
        g_visualTreeWatcher = nullptr;
    }

    winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
    {
        std::lock_guard lock(g_captureMutex);
        if (g_capturedDispatcher) dispatcher = g_capturedDispatcher;
    }
    if (dispatcher) {
        HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (completed) {
            try {
                // Low priority acts as a barrier behind every standalone-card
                // callback already queued at the same priority.
                dispatcher.RunAsync(
                    winrt::Windows::UI::Core::CoreDispatcherPriority::Low,
                    [completed] {
                        EventSignalOnExit signal{completed};
                        try {
                            RestoreCapturedMedia();
                            if (g_capturedXamlSource)
                                g_capturedXamlSource.Close();
                            g_capturedXamlSource = nullptr;
                            if (g_capturedHostWindow) {
                                DestroyWindow(g_capturedHostWindow);
                                g_capturedHostWindow = nullptr;
                            }
                            g_capturedIslandWindow = nullptr;
                            UnregisterCapturedHostClass();
                            {
                                std::lock_guard lock(g_captureMutex);
                                g_originalMediaParent = nullptr;
                                g_capturedMedia = nullptr;
                                g_capturedDispatcher = nullptr;
                                g_capturedMediaHandle = 0;
                                g_mediaCaptured = false;
                            }
                        } catch (...) {
                            Wh_Log(L"Media-capture shutdown callback failed");
                        }
                    });
                WaitForSingleObject(completed, INFINITE);
            } catch (...) {
                Wh_Log(L"Failed to dispatch media-capture shutdown");
            }
            CloseHandle(completed);
        }
    }
    g_mediaCaptured = false;
}

void StopRuntime() {
    g_shuttingDown = true;
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (DWORD threadId = g_hookThreadId.load())
        PostThreadMessageW(threadId, WM_QUIT, 0, 0);
    if (g_hookThread) WaitForSingleObject(g_hookThread, INFINITE);
    if (g_routerThread) WaitForSingleObject(g_routerThread, INFINITE);
    if (g_nativeThread) WaitForSingleObject(g_nativeThread, INFINITE);
    ShutdownMediaCapture();
    CloseHandleIfPresent(g_hookThread);
    CloseHandleIfPresent(g_routerThread);
    CloseHandleIfPresent(g_nativeThread);
    CloseHandleIfPresent(g_hookReadyEvent);
    CloseHandleIfPresent(g_routeRequestEvent);
    CloseHandleIfPresent(g_toggleRequestEvent);
    CloseHandleIfPresent(g_captureReadyEvent);
    CloseHandleIfPresent(g_showEvent);
    CloseHandleIfPresent(g_stopEvent);
    g_hookThreadId = 0;
    g_hookInstalled = false;
}

}  // namespace

BOOL Wh_ModInit() {
    g_role = DetectProcessRole();
    if (g_role == ProcessRole::Unsupported) return FALSE;
    if (g_role == ProcessRole::Explorer && !IsShellExplorerProcess()) {
        Wh_Log(L"Skipping non-shell Explorer process");
        return FALSE;
    }
    g_shuttingDown = false;
    if (g_role == ProcessRole::ShellHost) LogNativeState(100);
    if (g_role == ProcessRole::Explorer) LogRouterState(100);
    LoadSettings();
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_showEvent = CreateSharedEvent(kShowEventObjectName, 0);
    g_captureReadyEvent = CreateSharedEvent(
        kCaptureReadyEventObjectName, CREATE_EVENT_MANUAL_RESET);
    if (!g_stopEvent || !g_showEvent || !g_captureReadyEvent) {
        if (g_role == ProcessRole::ShellHost)
            LogNativeState(-110, HRESULT_FROM_WIN32(GetLastError()));
        Wh_Log(L"Event creation failed: %u", GetLastError());
        StopRuntime();
        return FALSE;
    }
    if (g_role == ProcessRole::ShellHost) LogNativeState(110);

    bool initialized = false;
    if (g_role == ProcessRole::Explorer)
        initialized = InitializeExplorerRole();
    else if (g_role == ProcessRole::ShellHost)
        initialized = InitializeShellRole();

    if (!initialized) {
        Wh_Log(L"Process-role initialization failed");
        StopRuntime();
        return FALSE;
    }
    return TRUE;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    if (g_role == ProcessRole::Explorer) {
        if (DWORD threadId = g_hookThreadId.load())
            PostThreadMessageW(threadId, kResetGestureMessage, 0, 0);
    }
}

void Wh_ModUninit() {
    StopRuntime();
}
