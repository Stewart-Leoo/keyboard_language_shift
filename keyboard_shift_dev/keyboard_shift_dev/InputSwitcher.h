#pragma once
// InputSwitcher.h
// 输入语言自动切换模块 -- 公共接口
//
// 职责：
//   监听 KeyboardMonitor 的按键事件，检测到键盘切换时自动执行修正序列：
//   1. Backspace        -- 删除刚输入的错误字符
//   2. 关闭 CapsLock    -- 若当前为开启状态则先关闭
//   3. 语言切换快捷键   -- 根据新键盘的 lang 配置发送对应热键
//   4. 重新键入原始按键 -- 在正确语言环境下重新输入
//
// 注入键过滤：
//   SendInput 注入的模拟键在 Raw Input 中 hDevice==NULL，
//   本模块自动忽略这类事件，避免递归触发。

#include "KeyboardMonitor.h"
#include "InputConfig.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>

class InputSwitcher
{
public:
    // ================================================================
    // 构造 / 析构
    // ================================================================

    // monitor -- 用于注册按键回调
    // config  -- 用于读取键盘语言设置和快捷键定义
    InputSwitcher(KeyboardMonitor& monitor, InputConfig& config);
    ~InputSwitcher();

    // 禁止拷贝
    InputSwitcher(const InputSwitcher&)            = delete;
    InputSwitcher& operator=(const InputSwitcher&) = delete;

    // ================================================================
    // 控制接口
    // ================================================================

    // 启动：注册按键回调，开启工作线程
    void Start();

    // 停止：清除回调，停止工作线程
    void Stop();

    // 查询是否正在运行
    bool IsRunning() const;

private:
    // ================================================================
    // 内部实现
    // ================================================================

    // 按键事件处理（由 KeyboardMonitor 在消息线程上调用）
    void OnKeyEvent(HANDLE hDevice, USHORT vkey, USHORT scanCode, USHORT flags);

    // 工作线程：从任务队列取出切换任务并执行
    void WorkerThreadFunc();

    // 执行完整的语言修正序列（CapsLock 在函数内实时读取）
    void PerformSwitch(InputLang targetLang, USHORT vkey, bool shiftDown);

    // ================================================================
    // SendInput 辅助
    // ================================================================

    static void SendKeyDown(WORD vk);
    static void SendKeyUp(WORD vk);
    static void SendKeyPress(WORD vk);
    static void SendHotkey(const HotkeyDef& hk);

    // 字符串键名 -> 虚拟键码（"F11"->"VK_F11"，"A"->'A' 等）
    static WORD KeyNameToVK(const std::wstring& name);

    // ================================================================
    // 成员变量
    // ================================================================

    KeyboardMonitor&   m_monitor;
    InputConfig&       m_config;

    std::atomic<bool>  m_running;
    HANDLE             m_lastDevice;  // 上一次按键来源的设备句柄

    // 切换任务结构
    struct SwitchTask
    {
        InputLang targetLang; // 目标语言
        USHORT    vkey;       // 需要重新键入的虚拟键码
        bool      shiftDown;  // 按下时 Shift 是否被按住
        // CapsLock 不在此捕获，由 PerformSwitch 执行时实时读取
    };

    std::mutex              m_queueMutex;
    std::condition_variable m_cv;
    std::queue<SwitchTask>  m_taskQueue;
    std::thread             m_workerThread;
};
