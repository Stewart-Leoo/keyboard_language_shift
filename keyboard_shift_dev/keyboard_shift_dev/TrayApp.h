#pragma once
// TrayApp.h
// 系统托盘模块 -- 公共接口
//
// 职责：
//   - 在系统托盘区注册图标
//   - 提供右键菜单：自动切换开关 / 设置 / 退出
//   - 在主线程运行 Windows 消息循环
//
// 链接依赖：shell32.lib（VS 项目默认已包含）

#include <Windows.h>
#include <functional>
#include <string>
#include <atomic>
#include "resource.h"  // 图标资源 ID（IDI_APP_ICON）

// 托盘右键菜单项 ID
#define TRAY_WM_MSG       (WM_APP + 1)  // 托盘图标通知消息
#define ID_TRAY_TOGGLE    2001          // 自动切换开关
#define ID_TRAY_SETTINGS  2002          // 打开设置控制台
#define ID_TRAY_EXIT      2003          // 退出程序

class TrayApp
{
public:
    // ================================================================
    // 构造 / 析构
    // ================================================================
    TrayApp();
    ~TrayApp();

    TrayApp(const TrayApp&)            = delete;
    TrayApp& operator=(const TrayApp&) = delete;

    // ================================================================
    // 回调注册（在 Run() 之前设置）
    // ================================================================

    // 用户点击"自动切换"菜单项时触发
    void SetOnToggleSwitcher(std::function<void()> fn);

    // 用户点击"设置"菜单项时触发（在独立线程中打开控制台）
    void SetOnShowSettings(std::function<void()> fn);

    // 用户点击"退出"菜单项时触发
    void SetOnExit(std::function<void()> fn);

    // ================================================================
    // 状态更新（可在任意线程调用）
    // ================================================================

    // 更新菜单中"自动切换"的勾选状态和 Tooltip 提示文字
    void SetSwitcherState(bool on);

    // ================================================================
    // 运行 / 退出
    // ================================================================

    // 初始化托盘图标并进入 Windows 消息循环（阻塞，在主线程调用）
    // 返回值为消息循环退出码
    int Run();

    // 向消息循环投递 WM_QUIT（可从任意线程调用）
    void Quit();

private:
    // 消息窗口过程（静态，通过 GWLP_USERDATA 取 this）
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam);

    // 在光标位置弹出右键菜单
    void ShowContextMenu();

    // 更新托盘图标 Tooltip
    void UpdateTooltip();

    // ================================================================
    // 成员变量
    // ================================================================
    HWND              m_hWnd;          // 隐藏消息窗口
    NOTIFYICONDATAW   m_nid;           // 托盘图标数据

    std::atomic<bool> m_switcherOn;    // 自动切换当前状态（用于菜单勾选）

    std::function<void()> m_onToggle;    // 切换回调
    std::function<void()> m_onSettings; // 设置回调
    std::function<void()> m_onExit;     // 退出回调
};
