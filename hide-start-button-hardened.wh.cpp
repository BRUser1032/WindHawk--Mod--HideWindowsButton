// ==WindhawkMod==
// @id              hide-start-button-hardened
// @name            Hide Start Button (hardened)
// @description     Hides the Start button from the Windows 11 taskbar. Hardened fork with a build gate, sanity checks on internal pointers, and a degraded mode instead of risky behavior.
// @version         1.1.0
// @author          ptrkhh (original), BRUser1032 / BitRize (fork), hardening assisted by Claude
// @github          https://github.com/BRUser1032
// @homepage        https://github.com/BRUser1032/WindHawk--Mod--HideWindowsButton
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject
// ==/WindhawkMod==

// NOTE: no @license tag on purpose. The license of the original mod
// ("Hide Start Button" by ptrkhh) has not been verified yet. Do not add a
// license here until it has been checked.

// ==WindhawkModReadme==
/*
# Hide Start Button (hardened)

Hides the Start button from the Windows 11 taskbar. The Start menu can still
be opened with the Win key, Ctrl+Esc or touchscreen gestures. Only Windows 11
is supported. Disabling the mod brings the button back.

## Credits

Based on "Hide Start Button" by ptrkhh, which is in turn based on the
"Start button always on the left" (taskbar-start-button-position) mod by
m417z. This fork keeps the same design and adds defensive code.
It was written with AI assistance and tested only on the builds listed below.

## Tested builds

- Windows 11 25H2, build 26200 (tested on 26200.9550)

On any other build the mod **refuses to load** by default (see the setting
below), because it depends on undocumented Windows internals.

## What the hardening does (and does not do)

Does:
- Refuses to load on untested Windows builds unless you opt in.
- Validates that the internal XAML function it hooks (vtable index 92) lives
  in a XAML module. If it does not, the mod keeps running in **degraded mode**:
  the Start button is still collapsed, but the hook that closes the leftover
  gap is not installed. No risky hook, no crash.
- Checks that internal memory is readable (VirtualQuery) before reading it.
- Logs which offset source was used (byte pattern or fallback) and the module
  and RVA of the hooked function, so problems on new builds can be diagnosed.
- Guards C++ exceptions around XAML calls and hook callbacks.
- Writes the Start button visibility only when it changes.
- Uses SendMessageTimeout (2 s) instead of SendMessage.

Does not:
- Catch access violations. `catch (...)` does not cover them; the readability
  checks reduce the risk but cannot remove it (the memory can change between
  the check and the read).
- Verify that vtable index 92 is really `UIElement::Arrange`. It only checks
  that the address belongs to a XAML module.
- Guarantee anything after a Windows update. If Explorer misbehaves, disable
  the mod and restart Explorer.

## Settings

- Allow untested Windows builds: off by default. Turn it on to try the mod on
  another build, then check the Log tab.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- allowUntestedBuilds: false
  $name: Allow untested Windows builds
  $description: >-
    By default the mod only loads on Windows builds where it was tested.
    Enabling this lets it try other builds. It depends on undocumented
    internals, so use it at your own risk and check the Log tab.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <atomic>

#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Xaml.h>

using namespace winrt::Windows::UI::Xaml;

// ---------------------------------------------------------------------------
// Configuration constants
// ---------------------------------------------------------------------------

// Windows build numbers (major build, without the UBR) on which the mod was
// actually tested. 26200 = 25H2. Add a build here only after testing it.
constexpr DWORD kTestedBuilds[] = {26200};

// Fixed vtable index of the XAML UIElement::Arrange implementation.
// This is an internal detail of Windows and cannot be fully validated.
constexpr int kArrangeVtableIndex = 92;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

std::atomic<bool> g_taskbarViewDllLoaded;
std::atomic<bool> g_unloading;
std::atomic<bool> g_arrangeHookActive;
std::atomic<bool> g_offsetSourceLogged;
thread_local bool g_inArrangeOverride;

// RAII guard: the flag is always restored, even if something throws.
struct ArrangeGuard {
    bool prev;
    ArrangeGuard() : prev(g_inArrangeOverride) { g_inArrangeOverride = true; }
    ~ArrangeGuard() { g_inArrangeOverride = prev; }
};

// ---------------------------------------------------------------------------
// Safety helpers
// ---------------------------------------------------------------------------

// Returns true if [p, p + size) is committed, readable memory.
// Not race-free (memory can change after the check), but it avoids the most
// common crash: dereferencing a stale or wrong pointer.
bool IsReadable(const void* p, size_t size) {
    if (!p || size == 0)
        return false;

    const BYTE* cur = static_cast<const BYTE*>(p);
    const BYTE* end = cur + size;
    if (end < cur)
        return false;  // overflow

    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(cur, &mbi, sizeof(mbi)))
            return false;
        if (mbi.State != MEM_COMMIT)
            return false;
        if (mbi.Protect == 0 || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        const BYTE* regionEnd =
            static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cur)
            return false;
        cur = regionEnd;
    }
    return true;
}

// Windows build number (e.g. 26200), or 0 if it could not be read.
DWORD GetWindowsBuildNumber() {
    using RtlGetVersion_t = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandle(L"ntdll.dll");
    if (!ntdll)
        return 0;
    auto rtlGetVersion = reinterpret_cast<RtlGetVersion_t>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    if (!rtlGetVersion)
        return 0;
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (rtlGetVersion(&vi) != 0)
        return 0;
    return vi.dwBuildNumber;
}

bool IsTestedBuild(DWORD build) {
    if (build == 0)
        return false;
    for (DWORD tested : kTestedBuilds) {
        if (tested == build)
            return true;
    }
    return false;
}

// Resolves the module that contains |addr|. Fills the module base name and
// the RVA of |addr| inside it.
bool GetModuleInfoFromAddress(const void* addr,
                              WCHAR* outName,
                              size_t outNameCount,
                              size_t* outRva) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &mod) ||
        !mod) {
        return false;
    }

    WCHAR path[MAX_PATH];
    DWORD len = GetModuleFileName(mod, path, ARRAYSIZE(path));
    if (len == 0 || len >= ARRAYSIZE(path))
        return false;

    const WCHAR* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    lstrcpynW(outName, base, static_cast<int>(outNameCount));

    if (outRva) {
        *outRva = static_cast<size_t>(static_cast<const BYTE*>(addr) -
                                      reinterpret_cast<const BYTE*>(mod));
    }
    return true;
}

bool NameContainsXaml(const WCHAR* name) {
    WCHAR lower[MAX_PATH];
    lstrcpynW(lower, name, ARRAYSIZE(lower));
    CharLowerBuffW(lower, lstrlenW(lower));
    return wcsstr(lower, L"xaml") != nullptr;
}

// ---------------------------------------------------------------------------
// XAML tree helpers
// ---------------------------------------------------------------------------

template <typename F>
FrameworkElement EnumChildElements(FrameworkElement element, F callback) {
    int count = Media::VisualTreeHelper::GetChildrenCount(element);

    for (int i = 0; i < count; i++) {
        auto child = Media::VisualTreeHelper::GetChild(element, i)
                         .try_as<FrameworkElement>();
        if (child && callback(child))
            return child;
    }

    return nullptr;
}

FrameworkElement FindChildByName(FrameworkElement element, PCWSTR name) {
    return EnumChildElements(element, [name](FrameworkElement child) {
        return child.Name() == name;
    });
}

bool ApplyStyle(XamlRoot xamlRoot) {
    if (!xamlRoot)
        return false;

    try {
        FrameworkElement child = xamlRoot.Content().try_as<FrameworkElement>();
        if (!child ||
            !(child = EnumChildElements(child, [](FrameworkElement c) {
                  return winrt::get_class_name(c) == L"Taskbar.TaskbarFrame";
              })) ||
            !(child = FindChildByName(child, L"RootGrid")) ||
            !(child = FindChildByName(child, L"TaskbarFrameRepeater")))
            return false;

        auto startButton = EnumChildElements(child, [](FrameworkElement c) {
            return winrt::get_class_name(c) ==
                       L"Taskbar.ExperienceToggleButton" &&
                   Automation::AutomationProperties::GetAutomationId(c) ==
                       L"StartButton";
        });

        if (startButton) {
            const Visibility wanted =
                g_unloading ? Visibility::Visible : Visibility::Collapsed;

            // Only write when different, to avoid useless layout invalidation.
            if (startButton.Visibility() != wanted)
                startButton.Visibility(wanted);
        }

        return true;
    } catch (...) {
        Wh_Log(L"ApplyStyle failed with an exception");
        return false;
    }
}

// ---------------------------------------------------------------------------
// Internal taskbar.dll pointers
// ---------------------------------------------------------------------------

void* CTaskBand_ITaskListWndSite_vftable;
void* CSecondaryTaskBand_ITaskListWndSite_vftable;

using CTaskBand_GetTaskbarHost_t = void*(WINAPI*)(void* pThis, void** result);
CTaskBand_GetTaskbarHost_t CTaskBand_GetTaskbarHost_Original;

void* TaskbarHost_FrameHeight_Original;

using CSecondaryTaskBand_GetTaskbarHost_t = void*(WINAPI*)(void* pThis,
                                                           void** result);
CSecondaryTaskBand_GetTaskbarHost_t CSecondaryTaskBand_GetTaskbarHost_Original;

using std__Ref_count_base__Decref_t = void(WINAPI*)(void* pThis);
std__Ref_count_base__Decref_t std__Ref_count_base__Decref_Original;

XamlRoot XamlRootFromTaskbarHostSharedPtr(void* taskbarHostSharedPtr[2]) {
    if (!taskbarHostSharedPtr[0] && !taskbarHostSharedPtr[1])
        return nullptr;

    XamlRoot result = nullptr;

    if (taskbarHostSharedPtr[0] && TaskbarHost_FrameHeight_Original) {
        try {
            size_t offset = 0x48;  // fallback
            bool fromPattern = false;

            const BYTE* b = (const BYTE*)TaskbarHost_FrameHeight_Original;
            if (IsReadable(b, 8) && b[0] == 0x48 && b[1] == 0x83 &&
                b[2] == 0xEC && b[4] == 0x48 && b[5] == 0x83 && b[6] == 0xC1 &&
                b[7] <= 0x7F) {
                offset = b[7];
                fromPattern = true;
            }

            // Log once which source was used, so a silent fallback on a new
            // build is visible in the Log tab.
            if (!g_offsetSourceLogged.exchange(true)) {
                Wh_Log(L"TaskbarHost offset 0x%X (source: %s)",
                       (unsigned)offset,
                       fromPattern ? L"byte pattern" : L"FALLBACK default");
            }

            BYTE* host = (BYTE*)taskbarHostSharedPtr[0];
            IUnknown** slot = (IUnknown**)(host + offset);

            if (!IsReadable(slot, sizeof(void*))) {
                Wh_Log(L"TaskbarHost slot is not readable, skipping");
            } else {
                IUnknown* unk = *slot;
                // The object must be readable and have a readable vtable
                // before QueryInterface is called on it.
                if (unk && IsReadable(unk, sizeof(void*)) &&
                    IsReadable(*(void**)unk, sizeof(void*))) {
                    FrameworkElement fe = nullptr;
                    unk->QueryInterface(winrt::guid_of<FrameworkElement>(),
                                        winrt::put_abi(fe));
                    if (fe)
                        result = fe.XamlRoot();
                } else if (unk) {
                    Wh_Log(L"TaskbarHost object/vtable not readable, skipping");
                }
            }
        } catch (...) {
            Wh_Log(L"XamlRootFromTaskbarHostSharedPtr failed");
            result = nullptr;
        }
    }

    // The reference is always released, even if something above failed.
    if (taskbarHostSharedPtr[1] && std__Ref_count_base__Decref_Original)
        std__Ref_count_base__Decref_Original(taskbarHostSharedPtr[1]);

    return result;
}

XamlRoot GetTaskbarXamlRoot(HWND hWnd, bool isSecondary) {
    HWND hTaskSwWnd =
        isSecondary ? (HWND)FindWindowEx(hWnd, nullptr, L"WorkerW", nullptr)
                    : (HWND)GetProp(hWnd, L"TaskbandHWND");
    if (!hTaskSwWnd)
        return nullptr;

    void* vftable = isSecondary ? CSecondaryTaskBand_ITaskListWndSite_vftable
                                : CTaskBand_ITaskListWndSite_vftable;
    if (!vftable)
        return nullptr;

    void* p = (void*)GetWindowLongPtr(hTaskSwWnd, 0);
    if (!p)
        return nullptr;

    for (int i = 0; *(void**)p != vftable; i++) {
        if (i == 20)
            return nullptr;

        p = (void**)p + 1;
    }

    void* sharedPtr[2]{};
    if (isSecondary) {
        if (!CSecondaryTaskBand_GetTaskbarHost_Original)
            return nullptr;
        CSecondaryTaskBand_GetTaskbarHost_Original(p, sharedPtr);
    } else {
        if (!CTaskBand_GetTaskbarHost_Original)
            return nullptr;
        CTaskBand_GetTaskbarHost_Original(p, sharedPtr);
    }

    return XamlRootFromTaskbarHostSharedPtr(sharedPtr);
}

HWND FindCurrentProcessTaskbarWnd() {
    HWND hWnd = nullptr;
    while ((hWnd = FindWindowEx(nullptr, hWnd, L"Shell_TrayWnd", nullptr))) {
        DWORD pid;
        GetWindowThreadProcessId(hWnd, &pid);
        if (pid == GetCurrentProcessId())
            return hWnd;
    }

    return nullptr;
}

void ApplySettingsFromTaskbarThread() {
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hWnd, LPARAM) -> BOOL {
            try {
                WCHAR cls[32];
                if (GetClassName(hWnd, cls, ARRAYSIZE(cls)) == 0)
                    return TRUE;

                XamlRoot xamlRoot = nullptr;
                if (_wcsicmp(cls, L"Shell_TrayWnd") == 0)
                    xamlRoot = GetTaskbarXamlRoot(hWnd, false);
                else if (_wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0)
                    xamlRoot = GetTaskbarXamlRoot(hWnd, true);

                if (xamlRoot)
                    ApplyStyle(xamlRoot);
            } catch (...) {
                Wh_Log(L"ApplySettingsFromTaskbarThread callback failed");
            }
            return TRUE;
        },
        0);
}

void ApplySettings(HWND hTaskbarWnd) {
    static const UINT msg =
        RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);

    DWORD threadId = GetWindowThreadProcessId(hTaskbarWnd, nullptr);
    if (!threadId)
        return;

    if (threadId == GetCurrentThreadId()) {
        ApplySettingsFromTaskbarThread();
        return;
    }

    HHOOK hook = SetWindowsHookEx(
        WH_CALLWNDPROC,
        [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
            try {
                if (nCode == HC_ACTION &&
                    ((const CWPSTRUCT*)lParam)->message == msg)
                    ApplySettingsFromTaskbarThread();
            } catch (...) {
                Wh_Log(L"Window hook callback failed");
            }
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        },
        nullptr, threadId);
    if (!hook) {
        Wh_Log(L"SetWindowsHookEx failed");
        return;
    }

    DWORD_PTR ignored = 0;
    if (!SendMessageTimeout(hTaskbarWnd, msg, 0, 0,
                            SMTO_ABORTIFHUNG | SMTO_NORMAL, 2000, &ignored)) {
        Wh_Log(L"SendMessageTimeout to the taskbar window failed or timed out");
    }

    UnhookWindowsHookEx(hook);
}

// ---------------------------------------------------------------------------
// IUIElement::Arrange hook (closes the gap left by the hidden button)
// ---------------------------------------------------------------------------

using IUIElement_Arrange_t =
    HRESULT(WINAPI*)(void* pThis, winrt::Windows::Foundation::Rect rect);
IUIElement_Arrange_t IUIElement_Arrange_Original;

HRESULT WINAPI IUIElement_Arrange_Hook(void* pThis,
                                       winrt::Windows::Foundation::Rect rect) {
    auto original = [=] { return IUIElement_Arrange_Original(pThis, rect); };

    if (!g_inArrangeOverride || g_unloading)
        return original();

    try {
        FrameworkElement element = nullptr;
        ((IUnknown*)pThis)
            ->QueryInterface(winrt::guid_of<FrameworkElement>(),
                             winrt::put_abi(element));
        if (!element)
            return original();

        if (winrt::get_class_name(element) !=
                L"Taskbar.ExperienceToggleButton" ||
            Automation::AutomationProperties::GetAutomationId(element) !=
                L"StartButton")
            return original();

        if (element.Visibility() != Visibility::Collapsed)
            element.Visibility(Visibility::Collapsed);
    } catch (...) {
        // If anything fails, layout proceeds as if the mod did not exist.
        return original();
    }

    return IUIElement_Arrange_Original(pThis, {0, 0, 0, rect.Height});
}

// Validates and installs the Arrange hook. On any doubt it does NOT hook and
// the mod stays in degraded mode (button collapsed, gap not closed).
bool SetupArrangeHook() {
    Shapes::Rectangle rectangle;
    IUIElement element = rectangle;
    void** vtable = *(void***)winrt::get_abi(element);

    if (!IsReadable(vtable, (kArrangeVtableIndex + 2) * sizeof(void*))) {
        Wh_Log(L"vtable is not readable up to index %d, degraded mode",
               kArrangeVtableIndex);
        return false;
    }

    void* target = vtable[kArrangeVtableIndex];
    if (!target) {
        Wh_Log(L"vtable[%d] is null, degraded mode", kArrangeVtableIndex);
        return false;
    }

    WCHAR moduleName[MAX_PATH] = L"?";
    size_t rva = 0;
    if (!GetModuleInfoFromAddress(target, moduleName, ARRAYSIZE(moduleName),
                                  &rva)) {
        Wh_Log(L"vtable[%d] does not belong to a known module, degraded mode",
               kArrangeVtableIndex);
        return false;
    }

    // Always log this: it is the data needed to support new Windows builds.
    Wh_Log(L"Arrange candidate vtable[%d] = %s+0x%X", kArrangeVtableIndex,
           moduleName, (unsigned)rva);

    if (!NameContainsXaml(moduleName)) {
        Wh_Log(L"vtable[%d] is not in a XAML module, degraded mode",
               kArrangeVtableIndex);
        return false;
    }

    // Informational only: neighbors in the vtable, to help future diagnosis.
    for (int idx : {kArrangeVtableIndex - 1, kArrangeVtableIndex + 1}) {
        WCHAR n[MAX_PATH] = L"?";
        size_t r = 0;
        if (vtable[idx] &&
            GetModuleInfoFromAddress(vtable[idx], n, ARRAYSIZE(n), &r)) {
            Wh_Log(L"  vtable[%d] = %s+0x%X", idx, n, (unsigned)r);
        }
    }

    if (!WindhawkUtils::SetFunctionHook((IUIElement_Arrange_t)target,
                                        IUIElement_Arrange_Hook,
                                        &IUIElement_Arrange_Original)) {
        Wh_Log(L"Failed to hook IUIElement::Arrange, degraded mode");
        return false;
    }

    Wh_ApplyHookOperations();
    g_arrangeHookActive = true;
    Wh_Log(L"Arrange hook installed");
    return true;
}

using TaskbarCollapsibleLayoutXamlTraits_ArrangeOverride_t =
    HRESULT(WINAPI*)(void* pThis,
                     void* context,
                     winrt::Windows::Foundation::Size size,
                     winrt::Windows::Foundation::Size* resultSize);
TaskbarCollapsibleLayoutXamlTraits_ArrangeOverride_t
    TaskbarCollapsibleLayoutXamlTraits_ArrangeOverride_Original;

HRESULT WINAPI TaskbarCollapsibleLayoutXamlTraits_ArrangeOverride_Hook(
    void* pThis,
    void* context,
    winrt::Windows::Foundation::Size size,
    winrt::Windows::Foundation::Size* resultSize) {
    [[maybe_unused]] static bool hooked = [] {
        try {
            if (!SetupArrangeHook())
                Wh_Log(L"Running in degraded mode (gap may remain)");
        } catch (...) {
            Wh_Log(L"Failed to set up the Arrange hook");
        }
        return true;
    }();

    ArrangeGuard guard;
    return TaskbarCollapsibleLayoutXamlTraits_ArrangeOverride_Original(
        pThis, context, size, resultSize);
}

// ---------------------------------------------------------------------------
// Symbol hooking
// ---------------------------------------------------------------------------

bool HookTaskbarDllSymbols() {
    HMODULE module =
        LoadLibraryEx(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        return false;

    WindhawkUtils::SYMBOL_HOOK taskbarDllHooks[] = {
        {{LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})"},
         &CTaskBand_ITaskListWndSite_vftable},
        {{LR"(const CSecondaryTaskBand::`vftable'{for `ITaskListWndSite'})"},
         &CSecondaryTaskBand_ITaskListWndSite_vftable},
        {{LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )"},
         &CTaskBand_GetTaskbarHost_Original},
        {{LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )"},
         &TaskbarHost_FrameHeight_Original},
        {{LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CSecondaryTaskBand::GetTaskbarHost(void)const )"},
         &CSecondaryTaskBand_GetTaskbarHost_Original},
        {{LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))"},
         &std__Ref_count_base__Decref_Original},
    };

    return HookSymbols(module, taskbarDllHooks, ARRAYSIZE(taskbarDllHooks));
}

bool HookTaskbarViewDllSymbols(HMODULE module) {
    // Taskbar.View.dll, ExplorerExtensions.dll
    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {{LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskbarCollapsibleLayout,struct winrt::Microsoft::UI::Xaml::Controls::IVirtualizingLayoutOverrides>::ArrangeOverride(void *,struct winrt::Windows::Foundation::Size,struct winrt::Windows::Foundation::Size *))"},
         &TaskbarCollapsibleLayoutXamlTraits_ArrangeOverride_Original,
         TaskbarCollapsibleLayoutXamlTraits_ArrangeOverride_Hook},
    };

    return HookSymbols(module, hooks, ARRAYSIZE(hooks));
}

HMODULE GetTaskbarViewModuleHandle() {
    HMODULE m = GetModuleHandle(L"Taskbar.View.dll");
    return m ? m : GetModuleHandle(L"ExplorerExtensions.dll");
}

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original;
HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName,
                                   HANDLE hFile,
                                   DWORD dwFlags) {
    HMODULE module = LoadLibraryExW_Original(lpLibFileName, hFile, dwFlags);
    if (module && !g_taskbarViewDllLoaded &&
        GetTaskbarViewModuleHandle() == module &&
        !g_taskbarViewDllLoaded.exchange(true)) {
        try {
            if (HookTaskbarViewDllSymbols(module))
                Wh_ApplyHookOperations();
        } catch (...) {
            Wh_Log(L"Hooking Taskbar.View.dll symbols failed");
        }
    }

    return module;
}

// ---------------------------------------------------------------------------
// Windhawk entry points
// ---------------------------------------------------------------------------

BOOL Wh_ModInit() {
    // Fail closed: untested Windows builds are refused unless the user opts in.
    const DWORD build = GetWindowsBuildNumber();
    const bool allowUntested = Wh_GetIntSetting(L"allowUntestedBuilds") != 0;

    Wh_Log(L"Windows build: %d", (int)build);

    if (!IsTestedBuild(build)) {
        if (!allowUntested) {
            Wh_Log(L"Build %d was not tested with this mod. Not loading. "
                   L"Enable 'Allow untested Windows builds' to try it.",
                   (int)build);
            return FALSE;
        }
        Wh_Log(L"WARNING: running on an untested build (%d) by user choice",
               (int)build);
    }

    // If internal symbols are not found (e.g. after a Windows update), the mod
    // simply does not load and Explorer runs normally.
    if (!HookTaskbarDllSymbols()) {
        Wh_Log(L"taskbar.dll symbols not found, mod not loaded");
        return FALSE;
    }

    if (HMODULE m = GetTaskbarViewModuleHandle()) {
        g_taskbarViewDllLoaded = true;
        if (!HookTaskbarViewDllSymbols(m)) {
            Wh_Log(L"Taskbar.View.dll symbols not found, mod not loaded");
            return FALSE;
        }
    } else {
        HMODULE kb = GetModuleHandle(L"kernelbase.dll");
        auto pLoadLib =
            kb ? (decltype(&LoadLibraryExW))GetProcAddress(kb, "LoadLibraryExW")
               : nullptr;
        if (!pLoadLib) {
            Wh_Log(L"LoadLibraryExW not found in kernelbase.dll");
            return FALSE;
        }

        if (!WindhawkUtils::SetFunctionHook(pLoadLib, LoadLibraryExW_Hook,
                                            &LoadLibraryExW_Original)) {
            Wh_Log(L"Failed to hook LoadLibraryExW");
            return FALSE;
        }
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    try {
        if (!g_taskbarViewDllLoaded) {
            if (HMODULE m = GetTaskbarViewModuleHandle()) {
                if (!g_taskbarViewDllLoaded.exchange(true)) {
                    if (HookTaskbarViewDllSymbols(m))
                        Wh_ApplyHookOperations();
                }
            }
        }

        HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
        if (hTaskbarWnd)
            ApplySettings(hTaskbarWnd);
    } catch (...) {
        Wh_Log(L"Wh_ModAfterInit failed");
    }
}

void Wh_ModBeforeUninit() {
    g_unloading = true;

    try {
        HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
        if (hTaskbarWnd)
            ApplySettings(hTaskbarWnd);
    } catch (...) {
        Wh_Log(L"Wh_ModBeforeUninit failed");
    }
}

void Wh_ModUninit() {}
