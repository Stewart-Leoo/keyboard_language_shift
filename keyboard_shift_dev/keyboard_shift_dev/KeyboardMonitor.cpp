// KeyboardMonitor.cpp
// 键盘监测模块 -- 实现

#include "KeyboardMonitor.h"
#include <hidsdi.h>     // HidD_GetProductString (链接: hid.lib)
#include <setupapi.h>   // 设备安装API            (链接: setupapi.lib)
#include <iostream>     // wcout（PrintStatus 使用）
#include <conio.h>      // _kbhit / _getch（RunStatusView 非阻塞按键检测）
#include <vector>
#include <climits>

// 消息窗口类名（每个进程内唯一即可）
static const wchar_t* KB_WND_CLASS = L"KeyboardMonitor_MsgWnd";

// ================================================================
// 构造 / 析构
// ================================================================

KeyboardMonitor::KeyboardMonitor()
    : m_running(false)
    , m_hWnd(nullptr)
    , m_pollIntervalMs(1000)
    , m_lastActiveDevice(nullptr)
{
}

KeyboardMonitor::~KeyboardMonitor()
{
    Stop();
}

// ================================================================
// Start：启动消息线程 + 轮询线程
// ================================================================
bool KeyboardMonitor::Start(OnDeviceChanged callback, DWORD pollIntervalMs)
{
    if (m_running) return true;

    m_callback       = callback;
    m_pollIntervalMs = pollIntervalMs;
    m_running        = true;

    // 先启动消息线程，它会创建窗口并注册 Raw Input
    m_msgThread = std::thread(&KeyboardMonitor::MessageThreadFunc, this);

    // 等待消息线程完成窗口注册（简单等待，后续可用事件对象优化）
    Sleep(200);

    // 首次枚举，填充初始键盘列表
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_keyboards = EnumerateKeyboards();
    }

    // 启动轮询线程
    m_pollThread = std::thread(&KeyboardMonitor::PollingThreadFunc, this);

    return true;
}

// ================================================================
// Stop：通知线程退出并等待
// ================================================================
void KeyboardMonitor::Stop()
{
    if (!m_running) return;

    m_running = false;

    // 向消息线程的窗口发送 WM_QUIT，使 GetMessage 返回 false 退出循环
    if (m_hWnd)
    {
        PostMessageW(m_hWnd, WM_QUIT, 0, 0);
        m_hWnd = nullptr;
    }

    if (m_msgThread.joinable())  m_msgThread.join();
    if (m_pollThread.joinable()) m_pollThread.join();
}

// ================================================================
// GetKeyboards：返回当前键盘列表的快照（线程安全）
// ================================================================
std::vector<KeyboardMonitor::KeyboardInfo> KeyboardMonitor::GetKeyboards() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_keyboards;
}

// ================================================================
// GetLastActiveDevice：返回最近一次击键来源的设备句柄
// 整个连接键盘列表中，只有此句柄对应的键盘为"活跃"，其余均为"静默"
// ================================================================
HANDLE KeyboardMonitor::GetLastActiveDevice() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastActiveDevice;
}

// ================================================================
// SetLangProvider：注入语言标签提供者回调
// ================================================================
void KeyboardMonitor::SetLangProvider(LangProvider provider)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_langProvider = std::move(provider);
}

// ================================================================
// SetKeyPressCallback：注册按键事件回调
// ================================================================
void KeyboardMonitor::SetKeyPressCallback(OnKeyPressed callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_keyPressCallback = std::move(callback);
}

// ================================================================
// GetMsSinceLastInput：返回距上次击键的毫秒数
// ================================================================
ULONGLONG KeyboardMonitor::GetMsSinceLastInput(HANDLE hDevice) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_lastInputTick.find(hDevice);
    if (it == m_lastInputTick.end()) return ULLONG_MAX; // 从未有过击键
    return GetTickCount64() - it->second;
}

// ================================================================
// 静态工具：读取 HID 产品名称
// ================================================================
std::wstring KeyboardMonitor::GetFriendlyName(const std::wstring& path)
{
    HANDLE hFile = CreateFileW(path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);

    if (hFile != INVALID_HANDLE_VALUE)
    {
        wchar_t buf[256] = {};
        if (HidD_GetProductString(hFile, buf, sizeof(buf)))
        {
            CloseHandle(hFile);
            return buf;
        }
        CloseHandle(hFile);
    }

    // 回退：截取路径末段作为可读标识
    size_t pos = path.rfind(L'\\');
    return (pos != std::wstring::npos) ? path.substr(pos + 1) : path;
}

// ================================================================
// 静态工具：枚举并过滤外接物理键盘
//
// 过滤规则（基于设备路径字符串）：
//   保留：路径含 "HID#VID_" 且不含 "&Col"
//   排除：路径含 "ROOT" / "ACPI" / "TERMINPUT"（内置/虚拟设备）
//
// 不使用 RIDI_DEVICEINFO.hid.usUsagePage 判断--
//   RID_DEVICE_INFO 是 union，RIM_TYPEKEYBOARD 时数据在 keyboard 成员，
//   hid.usUsagePage 与 keyboard.dwNumberOfFunctionKeys 内存重叠，
//   读出的是功能键数量而非 HID Usage，会导致所有键盘被误过滤。
// ================================================================
std::vector<KeyboardMonitor::KeyboardInfo> KeyboardMonitor::EnumerateKeyboards()
{
    std::vector<KeyboardInfo> result;

    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0)
        return result;

    std::vector<RAWINPUTDEVICELIST> devList(count);
    if (GetRawInputDeviceList(devList.data(), &count, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1)
        return result;

    for (const auto& dev : devList)
    {
        if (dev.dwType != RIM_TYPEKEYBOARD)
            continue;

        // 读取设备路径
        UINT nameLen = 0;
        GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICENAME, nullptr, &nameLen);
        if (nameLen == 0) continue;

        std::wstring path(nameLen, L'\0');
        GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICENAME, path.data(), &nameLen);
        while (!path.empty() && path.back() == L'\0') path.pop_back();

        // 过滤：必须有 VID/PID（真实物理设备）
        if (path.find(L"HID#VID_") == std::wstring::npos) continue;
        // 过滤：排除子集合附属接口
        if (path.find(L"&Col")      != std::wstring::npos) continue;
        // 过滤：排除内置/虚拟键盘
        if (path.find(L"ROOT")      != std::wstring::npos) continue;
        if (path.find(L"ACPI")      != std::wstring::npos) continue;
        if (path.find(L"TERMINPUT") != std::wstring::npos) continue;

        KeyboardInfo kb;
        kb.hDevice      = dev.hDevice;
        kb.deviceName   = path;
        kb.friendlyName = GetFriendlyName(path);
        result.push_back(std::move(kb));
    }
    return result;
}

// ================================================================
// 消息窗口过程（静态）
// 通过 GWLP_USERDATA 取回 KeyboardMonitor 实例指针
// ================================================================
LRESULT CALLBACK KeyboardMonitor::WndProc(HWND hWnd, UINT msg,
                                          WPARAM wParam, LPARAM lParam)
{
    KeyboardMonitor* self = reinterpret_cast<KeyboardMonitor*>(
        GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    if (msg == WM_INPUT && self)
    {
        UINT dataSize = 0;
        GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam),
            RID_INPUT, nullptr, &dataSize, sizeof(RAWINPUTHEADER));

        if (dataSize > 0)
        {
            std::vector<BYTE> buf(dataSize);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam),
                RID_INPUT, buf.data(), &dataSize, sizeof(RAWINPUTHEADER)) == dataSize)
            {
                const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buf.data());
                if (raw->header.dwType == RIM_TYPEKEYBOARD)
                {
                    {
                        HANDLE    hDev    = raw->header.hDevice;
                        USHORT    vkey    = raw->data.keyboard.VKey;
                        USHORT    scan    = raw->data.keyboard.MakeCode;
                        USHORT    kflags  = raw->data.keyboard.Flags;

                        // 按下事件：更新活跃设备和时间戳
                        if ((kflags & RI_KEY_BREAK) == 0)
                        {
                            ULONGLONG now = GetTickCount64();
                            std::lock_guard<std::mutex> lock(self->m_mutex);
                            self->m_lastInputTick[hDev] = now;
                            self->m_lastActiveDevice    = hDev;
                        }

                        // 按键回调（按下和抬起均通知，由订阅方自行过滤）
                        // 在锁外调用，避免回调内再次加锁造成死锁
                        OnKeyPressed cb;
                        {
                            std::lock_guard<std::mutex> lock(self->m_mutex);
                            cb = self->m_keyPressCallback;
                        }
                        if (cb) cb(hDev, vkey, scan, kflags);
                    }
                }
            }
        }
        // 必须转发给 DefWindowProc 以释放 Raw Input 缓冲区
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ================================================================
// 消息线程：创建隐藏窗口，注册 Raw Input，运行消息循环
// ================================================================
void KeyboardMonitor::MessageThreadFunc()
{
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    // 注册窗口类（已注册时返回 0，不影响后续流程）
    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = KB_WND_CLASS;
    RegisterClassExW(&wc);

    // 创建消息专用隐藏窗口（HWND_MESSAGE 父窗口，不出现在任务栏/屏幕）
    HWND hWnd = CreateWindowExW(0, KB_WND_CLASS, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hWnd) { m_running = false; return; }

    // 将 this 指针存入窗口用户数据，供静态 WndProc 访问实例成员
    SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    m_hWnd = hWnd;

    // 注册 Raw Input：监听所有键盘（Generic Desktop 0x01 / Keyboard 0x06）
    // RIDEV_INPUTSINK：即使程序不在前台也能收到消息
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;
    rid.usUsage     = 0x06;
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = hWnd;

    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
    {
        OutputDebugStringW(L"KeyboardMonitor: RegisterRawInputDevices failed");
        m_running = false;
        return;
    }

    // 消息循环（阻塞直到 Stop() 发送 WM_QUIT）
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ================================================================
// RunStatusView：实时状态显示模式
// 进入后循环"清屏 -> 刷新状态"，按 q/Q 退出返回调用方
// ================================================================
void KeyboardMonitor::RunStatusView() const
{
    while (true)
    {
        ClearConsole();
        PrintStatus();
        std::wcout << L"  [ Live view -- press q to return ]\n";

        // 每 100ms 检测一次按键，累计 10 次 = 1 秒刷新间隔
        // _kbhit() 和 _getch() 直接操作控制台缓冲区，不受 stdin 模式影响
        for (int i = 0; i < 10; ++i)
        {
            if (_kbhit())
            {
                int ch = _getch();
                if (ch == 'q' || ch == 'Q')
                    return; // 退出实时模式，返回调用方
            }
            Sleep(100);
        }
    }
}

// ================================================================
// ClearConsole：清空控制台并将光标归位左上角（无闪烁）
// 原理：用空格填满整个缓冲区，再重置颜色属性，最后移动光标
// ================================================================
void KeyboardMonitor::ClearConsole()
{
    HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hCon == INVALID_HANDLE_VALUE) return;

    CONSOLE_SCREEN_BUFFER_INFO csbi = {};
    if (!GetConsoleScreenBufferInfo(hCon, &csbi)) return;

    DWORD totalCells = csbi.dwSize.X * csbi.dwSize.Y;
    COORD origin     = { 0, 0 };
    DWORD written    = 0;

    FillConsoleOutputCharacterW(hCon, L' ', totalCells, origin, &written);
    FillConsoleOutputAttribute(hCon, csbi.wAttributes, totalCells, origin, &written);
    SetConsoleCursorPosition(hCon, origin);
}

// ================================================================
// PrintStatus：向控制台打印当前键盘状态（追加输出，不自动清屏）
// ================================================================
void KeyboardMonitor::PrintStatus() const
{
    auto   kbs        = GetKeyboards();
    HANDLE lastActive = GetLastActiveDevice();

    // 获取当前时间用于时间戳
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t timeBuf[32] = {};
    swprintf_s(timeBuf, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

    std::wcout << L"\n";
    std::wcout << L"  +==========================================+\n";
    std::wcout << L"  |  Keyboard Status  @" << timeBuf << L"             |\n";
    std::wcout << L"  +==========================================+\n";

    if (kbs.empty())
    {
        std::wcout << L"  |  (no external keyboard detected)         |\n";
        std::wcout << L"  +==========================================+\n\n";
        return;
    }

    std::wcout << L"  |  Connected: " << kbs.size() << L"                                |\n";
    std::wcout << L"  +------------------------------------------+\n";

    // 在锁外获取 langProvider 快照，避免在回调期间持锁
    LangProvider langProvider;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        langProvider = m_langProvider;
    }

    for (size_t i = 0; i < kbs.size(); ++i)
    {
        bool      active     = (kbs[i].hDevice == lastActive);
        ULONGLONG msSince    = GetMsSinceLastInput(kbs[i].hDevice);
        ULONGLONG secondsAgo = (msSince != ULLONG_MAX) ? msSince / 1000 : 0;

        // 通过回调获取语言标签；未注入时显示 "--"
        std::wstring langLabel = langProvider
            ? langProvider(kbs[i].deviceName)
            : L"--";

        const wchar_t* tag = active ? L"[ACTIVE]" : L"[ idle ]";

        std::wcout << L"  |  Keyboard " << (i + 1) << L"  " << tag << L"\n";
        std::wcout << L"  |    Name : " << kbs[i].friendlyName << L"\n";
        std::wcout << L"  |    Lang : " << langLabel << L"\n";

        if (msSince != ULLONG_MAX)
            std::wcout << L"  |    Last : " << secondsAgo << L"s ago\n";
        else
            std::wcout << L"  |    Last : no input yet\n";

        if (i + 1 < kbs.size())
            std::wcout << L"  +------------------------------------------+\n";
    }

    std::wcout << L"  +==========================================+\n\n";
}

// ================================================================
// 轮询线程：每隔 m_pollIntervalMs 重新枚举，检测插拔并触发回调
// ================================================================
void KeyboardMonitor::PollingThreadFunc()
{
    while (m_running)
    {
        Sleep(m_pollIntervalMs);
        if (!m_running) break;

        auto newList = EnumerateKeyboards();

        // 在锁外准备事件列表，避免在回调内持锁（防死锁）
        std::vector<std::pair<KeyboardInfo, bool>> events; // {info, connected}

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // 检测新增
            for (const auto& n : newList)
            {
                bool found = false;
                for (const auto& o : m_keyboards)
                    if (o.deviceName == n.deviceName) { found = true; break; }
                if (!found)
                    events.push_back({ n, true });
            }

            // 检测移除
            for (const auto& o : m_keyboards)
            {
                bool found = false;
                for (const auto& n : newList)
                    if (n.deviceName == o.deviceName) { found = true; break; }
                if (!found)
                    events.push_back({ o, false });
            }

            // 更新列表
            m_keyboards = newList;
        }

        // 触发回调（在锁外执行，调用方可自由调用 GetKeyboards 等接口）
        if (m_callback)
        {
            for (const auto& ev : events)
                m_callback(ev.first, ev.second);
        }
    }
}
