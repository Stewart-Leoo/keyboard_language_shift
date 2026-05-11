#pragma once
// KeyboardMonitor.h
// 键盘监测模块 -- 公共接口
//
// 职责：
//   - 枚举当前连接的 USB / 2.4G 外接物理键盘
//   - 通过 Raw Input 实时感知哪块键盘有物理击键
//   - 检测键盘插拔事件，通过回调通知调用方
//
// 使用示例：
//   KeyboardMonitor monitor;
//   monitor.Start([](const KeyboardMonitor::KeyboardInfo& kb, bool connected){
//       // 插拔回调
//   });
//   auto list   = monitor.GetKeyboards();
//   HANDLE last = monitor.GetLastActiveDevice(); // 最后一次击键来源句柄
//   monitor.Stop();
//
// 链接依赖：hid.lib  setupapi.lib

#include <Windows.h>
#include <string>
#include <vector>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>

class KeyboardMonitor
{
public:
    // ================================================================
    // 公共数据结构
    // ================================================================

    // 描述一块已过滤的物理键盘
    struct KeyboardInfo
    {
        HANDLE       hDevice      = nullptr; // Raw Input 设备句柄（系统分配，唯一标识）
        std::wstring deviceName;             // 设备路径（\\?\HID#VID_...）
        std::wstring friendlyName;           // 产品名称（如 "USB Keyboard"）
    };

    // 插拔事件回调类型
    //   kb        -- 发生变化的键盘信息
    //   connected -- true=接入  false=断开
    using OnDeviceChanged = std::function<void(const KeyboardInfo& kb, bool connected)>;

    // 按键事件回调类型（每次物理按键按下时触发）
    //   hDevice  -- 发出按键的设备句柄（NULL 表示 SendInput 注入的模拟键，应忽略）
    //   vkey     -- 虚拟键码（如 VK_F11、'A' 等）
    //   scanCode -- 扫描码
    //   flags    -- Raw Input flags（RI_KEY_BREAK=1 为抬起，=0 为按下）
    using OnKeyPressed = std::function<void(HANDLE hDevice, USHORT vkey,
                                            USHORT scanCode, USHORT flags)>;

    // ================================================================
    // 生命周期
    // ================================================================

    KeyboardMonitor();
    ~KeyboardMonitor();

    // 禁止拷贝（持有线程/句柄资源，不可复制）
    KeyboardMonitor(const KeyboardMonitor&)            = delete;
    KeyboardMonitor& operator=(const KeyboardMonitor&) = delete;

    // ================================================================
    // 控制接口
    // ================================================================

    // 启动监测（创建消息线程 + 轮询线程）
    //   callback       -- 可选，键盘插拔时触发，在内部线程上调用
    //   pollIntervalMs -- 插拔检测轮询间隔（毫秒，默认 1000）
    // 返回 true 表示启动成功
    bool Start(OnDeviceChanged callback = nullptr, DWORD pollIntervalMs = 1000);

    // 停止监测，等待内部线程退出
    void Stop();

    // ================================================================
    // 查询接口
    // ================================================================

    // 获取当前已连接的物理键盘列表（线程安全）
    std::vector<KeyboardInfo> GetKeyboards() const;

    // 获取"最后一次有击键输入"的键盘句柄
    //   定义：最近一次按键来源的设备句柄 = 活跃键盘
    //         其余已连接设备均为静默
    //   若尚无任何输入记录，返回 nullptr
    HANDLE GetLastActiveDevice() const;

    // 获取某键盘距上次击键的毫秒数；从未有过击键则返回 ULLONG_MAX
    ULONGLONG GetMsSinceLastInput(HANDLE hDevice) const;

    // ================================================================
    // 语言标签注入
    // ================================================================

    // 语言标签提供者回调类型：传入设备路径，返回显示字符串（如 "EN" / "CN"）
    // 由外部（main）根据 InputConfig 注入，KeyboardMonitor 本身不依赖 InputConfig
    using LangProvider = std::function<std::wstring(const std::wstring& devicePath)>;

    // 注入语言标签提供者；未设置时显示 "--"
    void SetLangProvider(LangProvider provider);

    // 注册按键事件回调（每次物理按键按下/抬起时在消息线程上调用）
    // 传入 nullptr 可清除回调
    void SetKeyPressCallback(OnKeyPressed callback);

    // ================================================================
    // 显示接口
    // ================================================================

    // 向控制台打印当前键盘连接状态及活跃情况（追加输出，不清屏）
    void PrintStatus() const;

    // 进入实时状态显示模式：清屏循环刷新，按 q 退出返回调用方
    // 每秒刷新一次，每 100ms 检测一次按键
    void RunStatusView() const;

    // ================================================================
    // 静态工具
    // ================================================================

    // 清空控制台并将光标归位左上角（无闪烁）
    static void ClearConsole();

private:
    // ================================================================
    // 内部实现（在 KeyboardMonitor.cpp 中定义）
    // ================================================================

    // 消息窗口过程（静态，通过 GWLP_USERDATA 拿到 this）
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam);

    // 枚举并过滤外接物理键盘
    static std::vector<KeyboardInfo> EnumerateKeyboards();

    // 读取 HID 产品名称字符串
    static std::wstring GetFriendlyName(const std::wstring& path);

    // 线程函数：消息循环（接收 WM_INPUT，记录击键时间）
    void MessageThreadFunc();

    // 线程函数：轮询插拔（对比前后枚举结果，触发回调）
    void PollingThreadFunc();

    // ================================================================
    // 成员变量
    // ================================================================

    std::thread        m_msgThread;      // 消息窗口线程
    std::thread        m_pollThread;     // 插拔轮询线程
    std::atomic<bool>  m_running;        // 运行标志，Stop() 时置 false

    HWND               m_hWnd;           // 隐藏消息窗口句柄

    DWORD              m_pollIntervalMs; // 轮询间隔
    OnDeviceChanged    m_callback;       // 插拔回调
    LangProvider       m_langProvider;   // 语言标签提供者（由外部注入）
    OnKeyPressed       m_keyPressCallback; // 按键事件回调（由 InputSwitcher 注入）

    mutable std::mutex              m_mutex;            // 保护以下成员
    std::map<HANDLE, ULONGLONG>     m_lastInputTick;    // 句柄 -> 最后击键 tick
    std::vector<KeyboardInfo>       m_keyboards;        // 当前已连接键盘列表
    HANDLE                          m_lastActiveDevice; // 最近一次击键来源的设备句柄
};
