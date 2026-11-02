#include <windows.h>
#include <avrt.h>
#include <commctrl.h>

#include "app.h"
#include "app_gui.h"
#include "app_window.h"
#include "directshow_capture.h"

#include <cstdio>
#include <exception>
#include <iterator>
#include <string>

namespace {

// Groovy has no client timeout: if the process dies without CMD_CLOSE, the MiSTer
// keeps showing the last frame. Closing the console, Ctrl+C, logoff and shutdown
// therefore become a normal window close, and the handler waits for the cleanup
// that sends CMD_CLOSE (Windows allows about 5 s).
HWND g_main_hwnd = nullptr;
HANDLE g_cleanup_done = nullptr;

BOOL WINAPI console_control(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (!g_main_hwnd || !g_cleanup_done) return FALSE;
        PostMessageW(g_main_hwnd, WM_CLOSE, 0, 0);
        WaitForSingleObject(g_cleanup_done, 4000);
        return TRUE;
    default:
        return FALSE;
    }
}

// The exe has no console of its own. Uses the launching terminal's console, or
// opens one, unless output is already redirected to a file or pipe.
void open_console() {
    const bool redirected = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_UNKNOWN;
    if (redirected) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole()) return;
    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
}

CommandLine parse_command_line(int argc, wchar_t** argv) {
    CommandLine options;
    for (int index = 1; index < argc; ++index) {
        if (_wcsicmp(argv[index], L"--console") == 0 || _wcsicmp(argv[index], L"--stats") == 0) {
            open_console();
            break;
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    for (int index = 1; index < argc; ++index) {
        const wchar_t* arg = argv[index];
        if (_wcsicmp(arg, L"--480i") == 0) options.progressive = false;
        else if (_wcsicmp(arg, L"--240p") == 0) options.progressive = true;
        else if (_wcsicmp(arg, L"--send") == 0) options.send = true;
        else if (_wcsicmp(arg, L"--letterbox") == 0) options.letterbox = true;
        else if (_wcsicmp(arg, L"--stats") == 0) options.print_stats = true;
        else if (_wcsicmp(arg, L"--no-lock") == 0) options.rate_lock = false;
        else if (_wcsicmp(arg, L"--console") == 0) continue;
        else if (arg[0] != L'-') options.device = arg;
        else std::wprintf(L"Ignoring unknown option %ls\n", arg);
    }
    return options;
}

// False if another copy is running and the user chose not to start a second one. Two
// copies would fight over the capture card, the MiSTer and the settings file. Windows
// releases the mutex when the last copy exits, even after a crash.
bool first_copy_or_user_insists() {
    CreateMutexW(nullptr, FALSE, L"Local\\GroovyGrabRunning");
    if (GetLastError() != ERROR_ALREADY_EXISTS) return true;

    constexpr int kRunAnyway = 100;
    const TASKDIALOG_BUTTON buttons[] = {{IDCANCEL, L"Close"}, {kRunAnyway, L"I know what I'm doing"}};
    TASKDIALOGCONFIG dialog{sizeof(dialog)};
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    dialog.pszWindowTitle = L"GroovyGrab";
    dialog.pszMainIcon = TD_WARNING_ICON;
    dialog.pszMainInstruction = L"GroovyGrab is already running";
    dialog.pszContent = L"A second copy would compete for the capture card and the MiSTer.";
    dialog.cButtons = static_cast<UINT>(std::size(buttons));
    dialog.pButtons = buttons;
    dialog.nDefaultButton = IDCANCEL;
    int chosen = IDCANCEL;
    return SUCCEEDED(TaskDialogIndirect(&dialog, &chosen, nullptr, nullptr)) && chosen == kRunAnyway;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const CommandLine options = parse_command_line(argc, argv);
    if (!first_copy_or_user_insists()) return 0;

    int exit_code = 0;
    try {
        directshow_capture::check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        DWORD mmcss_index = 0;
        const HANDLE mmcss = AvSetMmThreadCharacteristics(L"Playback", &mmcss_index);

        static app_gui::Frame gui;
        const AppSettings saved = load_settings(settings_path());
        HWND hwnd = app_window::create(GetModuleHandle(nullptr), gui, saved.window_width,
                                       saved.window_height);
        g_main_hwnd = hwnd;
        g_cleanup_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler(console_control, TRUE);
        {
            App app(hwnd, gui, options, saved);
            app.run();
        }
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Error: %s\n", error.what());
        const std::string message = std::string("GroovyGrab hit a problem and has to close.\n\n") + error.what();
        MessageBoxA(nullptr, message.c_str(), "GroovyGrab", MB_OK | MB_ICONERROR);
        exit_code = 1;
    }
    // App's destructor (also when unwinding) has stopped the stream and sent CMD_CLOSE.
    g_main_hwnd = nullptr;
    if (g_cleanup_done) SetEvent(g_cleanup_done);
    CoUninitialize();
    return exit_code;
}
