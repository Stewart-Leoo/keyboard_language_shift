// TrayApp.cpp
// 系统托盘模块 -- 实现

#include "TrayApp.h"
#include <shellapi.h>   // Shell_NotifyIconW

static const wchar_t* TRAY_WND_CLASS = L"KeyboardSwitcher_TrayWnd";
static const UINT     TRAY_ICON_ID   = 1;

// ================================================================
// 构造 / 析构
// ================================================================
TrayApp::TrayApp()
    : m_hWnd(nullptr)
    , m_switcherOn(true)
{
    ZeroMemory(&m_nid, sizeof(m_nid));
}

TrayApp::~TrayApp()
{
    // 移除托盘图标
    if (m_hWnd)
    {
        m_nid.uFlags = 0;
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        DestroyWindow(m_hWnd);
        m_hWnd = nullptr;
    }
}

// ================================================================
// 回调注册
// ================================================================
void TrayApp::SetOnToggleSwitcher(std::function<void()> fn) { m_onToggle   = std::move(fn); }
void TrayApp::SetOnShowSettings  (std::function<void()> fn) { m_onSettings = std::move(fn); }
void TrayApp::SetOnExit          (std::function<void()> fn) { m_onExit     = std::move(fn); }

// ================================================================
// SetSwitcherState：更新菜单勾选状态和 Tooltip
// ================================================================
void TrayApp::SetSwitcherState(bool on)
{
    m_switcherOn = on;
    UpdateTooltip();
}

// ================================================================
// Run：初始化托盘图标，进入消息循环
// ================================================================
int TrayApp::Run()
{
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    // 注册隐藏窗口类
    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = TRAY_WND_CLASS;
    RegisterClassExW(&wc);

    // 创建不可见消息窗口（HWND_MESSAGE）
    m_hWnd = CreateWindowExW(0, TRAY_WND_CLASS, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!m_hWnd) return -1;

    // 将 this 存入窗口用户数据
    SetWindowLongPtrW(m_hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // 配置托盘图标数据
    m_nid.cbSize           = sizeof(m_nid);
    m_nid.hWnd             = m_hWnd;
    m_nid.uID              = TRAY_ICON_ID;
    m_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = TRAY_WM_MSG;

    // 从 exe 内嵌资源加载自定义图标（resource.rc 中的 IDI_APP_ICON）
    // 同时作为托盘图标和 exe 主图标（任务栏/文件管理器中显示）
    m_nid.hIcon = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr),
                   MAKEINTRESOURCEW(IDI_APP_ICON),
                   IMAGE_ICON,
                   GetSystemMetrics(SM_CXSMICON),
                   GetSystemMetrics(SM_CYSMICON),
                   LR_DEFAULTCOLOR));

    // 若自定义图标加载失败则回退使用系统默认图标
    if (!m_nid.hIcon)
    {
        m_nid.hIcon = static_cast<HICON>(
            LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON,
                       GetSystemMetrics(SM_CXSMICON),
                       GetSystemMetrics(SM_CYSMICON),
                       LR_SHARED));
    }

    UpdateTooltip(); // 写入初始 Tooltip 文字

    // 注册托盘图标
    Shell_NotifyIconW(NIM_ADD, &m_nid);

    // 消息循环
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 退出时删除托盘图标
    Shell_NotifyIconW(NIM_DELETE, &m_nid);

    return static_cast<int>(msg.wParam);
}

// ================================================================
// Quit：从任意线程投递退出消息
// ================================================================
void TrayApp::Quit()
{
    if (m_hWnd) PostMessageW(m_hWnd, WM_QUIT, 0, 0);
}

// ================================================================
// UpdateTooltip：根据当前状态刷新托盘 Tooltip 文字
// ================================================================
void TrayApp::UpdateTooltip()
{
    const wchar_t* state = m_switcherOn ? L"ON" : L"OFF";
    swprintf_s(m_nid.szTip, L"KB Switcher  [AutoSwitch:%s]", state);
    if (m_hWnd)
        Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

// ================================================================
// ShowContextMenu：在鼠标位置弹出右键菜单
// ================================================================
void TrayApp::ShowContextMenu()
{
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    // "自动切换" 条目（带勾选状态）
    UINT toggleFlags = MF_STRING;
    if (m_switcherOn) toggleFlags |= MF_CHECKED;
    InsertMenuW(hMenu, 0, MF_BYPOSITION | toggleFlags, ID_TRAY_TOGGLE,
                L"Auto Language Switch");

    InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);

    InsertMenuW(hMenu, 2, MF_BYPOSITION | MF_STRING, ID_TRAY_SETTINGS,
                L"Settings...");

    InsertMenuW(hMenu, 3, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);

    InsertMenuW(hMenu, 4, MF_BYPOSITION | MF_STRING, ID_TRAY_EXIT,
                L"Exit");

    // SetForegroundWindow 是 TrackPopupMenu 的必要前置调用（MSDN 规范）
    SetForegroundWindow(m_hWnd);

    POINT pt = {};
    GetCursorPos(&pt);

    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, m_hWnd, nullptr);

    DestroyMenu(hMenu);
}

// ================================================================
// WndProc：处理托盘通知消息和菜单命令
// ================================================================
LRESULT CALLBACK TrayApp::WndProc(HWND hWnd, UINT msg,
                                   WPARAM wParam, LPARAM lParam)
{
    TrayApp* self = reinterpret_cast<TrayApp*>(
        GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    if (msg == TRAY_WM_MSG && self)
    {
        switch (LOWORD(lParam))
        {
        case WM_RBUTTONUP:
            // 右键弹出菜单
            self->ShowContextMenu();
            break;

        case WM_LBUTTONDBLCLK:
            // 双击左键等同于"设置"
            if (self->m_onSettings)
                self->m_onSettings();
            break;
        }
        return 0;
    }

    if (msg == WM_COMMAND && self)
    {
        switch (LOWORD(wParam))
        {
        case ID_TRAY_TOGGLE:
            if (self->m_onToggle) self->m_onToggle();
            break;

        case ID_TRAY_SETTINGS:
            if (self->m_onSettings) self->m_onSettings();
            break;

        case ID_TRAY_EXIT:
            if (self->m_onExit) self->m_onExit();
            break;
        }
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
