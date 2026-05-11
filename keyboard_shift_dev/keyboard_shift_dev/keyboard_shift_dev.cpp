// keyboard_shift_dev.cpp
// 程序入口 -- 托盘常驻运行，按需打开设置控制台
//
// 子系统：Windows（无控制台窗口）
// 若在 VS 项目属性中已设置 /SUBSYSTEM:WINDOWS，可删除下面的 pragma
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")

#include <iostream>
#include <fcntl.h>
#include <io.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <Windows.h>
#include "KeyboardMonitor.h"
#include "InputConfig.h"
#include "InputSwitcher.h"
#include "TrayApp.h"

// ================================================================
// 控制台管理：AllocConsole / FreeConsole + stdio 重定向
// ================================================================

static std::mutex        g_consoleMutex;    // 防止并发打开多个控制台窗口
static std::atomic<bool> g_consoleOpen(false);

// 打开控制台并重定向 stdio
static bool OpenConsole()
{
    if (!AllocConsole()) return false;

    // 用 CreateFileW 直接打开 CONOUT$/CONIN$
    // SUBSYSTEM:WINDOWS 启动后 GetStdHandle 返回无效句柄，不可直接使用
    HANDLE hOut = CreateFileW(L"CONOUT$",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);

    HANDLE hIn = CreateFileW(L"CONIN$",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);

    // 将新句柄设置为进程标准句柄
    SetStdHandle(STD_OUTPUT_HANDLE, hOut);
    SetStdHandle(STD_ERROR_HANDLE,  hOut);
    SetStdHandle(STD_INPUT_HANDLE,  hIn);

    // 重定向 C 运行时 FILE* 流
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$",  "r", stdin);

    // 关键：清除 C++ 流的 failbit
    // SUBSYSTEM:WINDOWS 启动时无控制台，wcout 向无效句柄写入失败并置位 failbit，
    // 即使之后重定向了 stdout，failbit 仍残留，所有后续输出被静默丢弃
    std::wcout.clear();
    std::wcin.clear();
    std::wcerr.clear();

    // 设置 UTF-16 输出/输入模式
    (void)_setmode(_fileno(stdout), _O_U16TEXT);
    (void)_setmode(_fileno(stdin),  _O_U16TEXT);

    // 设置控制台字体（控制台输出内容均为 ASCII/英文，使用 Consolas）
    CONSOLE_FONT_INFOEX cfi = {};
    cfi.cbSize       = sizeof(cfi);
    cfi.dwFontSize.Y = 16;
    cfi.FontFamily   = FF_DONTCARE;
    cfi.FontWeight   = FW_NORMAL;
    wcscpy_s(cfi.FaceName, L"Consolas");
    SetCurrentConsoleFontEx(hOut, FALSE, &cfi);

    SetConsoleTitleW(L"Keyboard Switcher -- Settings");

    g_consoleOpen = true;
    return true;
}

// 关闭控制台
static void CloseConsole()
{
    g_consoleOpen = false;
    FreeConsole();
}

// ================================================================
// 设置控制台界面
// ================================================================

// 打印命令提示符（包含当前自动切换状态）
static void PrintPrompt(bool switcherOn)
{
    std::wcout << L"> [AutoSwitch:" << (switcherOn ? L"ON" : L"OFF")
               << L"] cmd (s=status  l=lang  t=toggle  q=close  h=help): ";
}

// 打印帮助信息
static void PrintHelp()
{
    std::wcout << L"\n"
               << L"  s / Enter  -- live keyboard status view (q to exit)\n"
               << L"  l          -- list keyboards and toggle lang (EN/CN)\n"
               << L"  t          -- enable/disable auto language switch\n"
               << L"  q          -- close settings (program keeps running in tray)\n"
               << L"  h          -- show this help\n\n";
}

// 处理 l 命令：列出键盘并切换语言标签
static void HandleLangSwitch(KeyboardMonitor& monitor, InputConfig& config)
{
    auto kbs = monitor.GetKeyboards();
    if (kbs.empty())
    {
        std::wcout << L"\n  (no external keyboard detected)\n\n";
        return;
    }

    std::wcout << L"\n  Connected keyboards:\n";
    for (size_t i = 0; i < kbs.size(); ++i)
    {
        std::wstring lang = InputConfig::LangToStr(config.GetLang(kbs[i].deviceName));
        std::wcout << L"  [" << (i + 1) << L"] " << kbs[i].friendlyName
                   << L"  [ " << lang << L" ]\n";
    }

    std::wcout << L"\n  Enter keyboard number to toggle lang (0 to cancel): ";
    std::wstring input;
    if (!std::getline(std::wcin, input)) return;

    int idx = 0;
    try { idx = std::stoi(input); } catch (...) {}

    if (idx < 1 || idx > static_cast<int>(kbs.size()))
    {
        std::wcout << L"  Cancelled.\n\n";
        return;
    }

    const std::wstring& path = kbs[idx - 1].deviceName;
    config.ToggleLang(path);
    std::wcout << L"\n  [" << idx << L"] " << kbs[idx - 1].friendlyName
               << L"  lang -> [ " << InputConfig::LangToStr(config.GetLang(path)) << L" ]\n";

    // 保存前同步所有键盘的友好名称
    for (const auto& kb : kbs)
        config.SetFriendlyName(kb.deviceName, kb.friendlyName);

    std::wcout << (config.Save()
        ? L"  Config saved.\n\n"
        : L"  WARNING: failed to save config.\n\n");
}

// 完整命令界面（在控制台线程中运行，阻塞直到用户输入 q）
static void RunConsoleUI(KeyboardMonitor& monitor, InputConfig& config,
                          InputSwitcher& switcher, TrayApp& tray)
{
    std::wcout << L"+==========================================+\n"
               << L"|   Keyboard Switcher v0.5  -- Settings   |\n"
               << L"|   Close window or type q to return       |\n"
               << L"+==========================================+\n\n"
               << L"  Switch to EN : " << config.GetHotkeyToEnglish().ToString() << L"\n"
               << L"  Switch to CN : " << config.GetHotkeyToChinese().ToString() << L"\n\n";

    PrintPrompt(switcher.IsRunning());
    std::wstring line;

    while (std::getline(std::wcin, line))
    {
        size_t  start = line.find_first_not_of(L" \t");
        wchar_t cmd   = (start != std::wstring::npos) ? line[start] : L's';

        switch (cmd)
        {
        case L's': case L'S':
            monitor.RunStatusView();
            KeyboardMonitor::ClearConsole();
            std::wcout << L"+==========================================+\n"
                       << L"|   Keyboard Switcher v0.5  -- Settings   |\n"
                       << L"+==========================================+\n\n";
            break;

        case L'l': case L'L':
            HandleLangSwitch(monitor, config);
            break;

        case L't': case L'T':
            if (switcher.IsRunning())
            {
                switcher.Stop();
                tray.SetSwitcherState(false);
                std::wcout << L"\n  Auto switch: OFF\n\n";
            }
            else
            {
                switcher.Start();
                tray.SetSwitcherState(true);
                std::wcout << L"\n  Auto switch: ON\n\n";
            }
            break;

        case L'q': case L'Q':
            std::wcout << L"\n  Closing settings, program continues in tray...\n";
            return; // 退出命令循环，返回调用方关闭控制台

        case L'h': case L'H':
            PrintHelp();
            break;

        default:
            std::wcout << L"  Unknown command. Type h for help.\n\n";
            break;
        }

        PrintPrompt(switcher.IsRunning());
    }
}

// ================================================================
// "设置"回调：在独立线程中打开控制台，运行命令界面，完成后关闭
// ================================================================
static void ShowSettings(KeyboardMonitor& monitor, InputConfig& config,
                          InputSwitcher& switcher, TrayApp& tray)
{
    // 同一时间只允许一个控制台窗口，防止并发打开
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    if (g_consoleOpen) return;

    if (!OpenConsole()) return;

    RunConsoleUI(monitor, config, switcher, tray);

    CloseConsole();
}

// ================================================================
// 程序入口
// ================================================================
int main()
{
    // ---- 模块二：初始化语言配置 ----------------------------------
    InputConfig config(InputConfig::GetDefaultFilePath());
    config.Load();

    // ---- 模块一：初始化键盘监测 ----------------------------------
    KeyboardMonitor monitor;

    // 注入语言标签回调（PrintStatus 显示时调用）
    monitor.SetLangProvider([&config](const std::wstring& devicePath) {
        return InputConfig::LangToStr(config.GetLang(devicePath));
    });

    // 插拔事件在托盘模式下静默处理（避免无控制台时调用 wcout 崩溃）
    monitor.Start([](const KeyboardMonitor::KeyboardInfo& kb, bool connected)
    {
        (void)kb; (void)connected;
    });

    // ---- 模块三：初始化自动切换（默认开启）----------------------
    InputSwitcher switcher(monitor, config);
    switcher.Start();

    // ---- 模块四：初始化托盘 --------------------------------------
    TrayApp tray;
    tray.SetSwitcherState(switcher.IsRunning());

    // 托盘菜单：自动切换 开/关
    tray.SetOnToggleSwitcher([&]()
    {
        if (switcher.IsRunning()) { switcher.Stop();  tray.SetSwitcherState(false); }
        else                     { switcher.Start(); tray.SetSwitcherState(true);  }
    });

    // 托盘菜单：设置 -- 在独立线程打开控制台，避免阻塞托盘消息循环
    tray.SetOnShowSettings([&]()
    {
        if (g_consoleOpen) return; // 已有控制台打开则不重复
        std::thread([&]() { ShowSettings(monitor, config, switcher, tray); }).detach();
    });

    // 托盘菜单：退出
    tray.SetOnExit([&]()
    {
        switcher.Stop();
        monitor.Stop();
        tray.Quit();
    });

    // 进入托盘消息循环（阻塞直到退出）
    return tray.Run();
}
