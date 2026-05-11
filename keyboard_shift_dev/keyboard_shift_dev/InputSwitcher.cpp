// InputSwitcher.cpp
// 输入语言自动切换模块 -- 实现

#include "InputSwitcher.h"
#include <map>

// ================================================================
// 构造 / 析构
// ================================================================
InputSwitcher::InputSwitcher(KeyboardMonitor& monitor, InputConfig& config)
    : m_monitor(monitor)
    , m_config(config)
    , m_running(false)
    , m_lastDevice(nullptr)
{
}

InputSwitcher::~InputSwitcher()
{
    Stop();
}

// ================================================================
// Start：注册按键回调，启动工作线程
// ================================================================
void InputSwitcher::Start()
{
    if (m_running) return;
    m_running    = true;
    m_lastDevice = nullptr;

    // 向 KeyboardMonitor 注册按键事件回调
    // 使用 [this] 捕获，Stop() 时会清除此回调
    m_monitor.SetKeyPressCallback(
        [this](HANDLE hDevice, USHORT vkey, USHORT scanCode, USHORT flags)
        {
            OnKeyEvent(hDevice, vkey, scanCode, flags);
        }
    );

    // 启动工作线程（负责执行 SendInput 序列）
    m_workerThread = std::thread(&InputSwitcher::WorkerThreadFunc, this);
}

// ================================================================
// Stop：清除回调，停止工作线程
// ================================================================
void InputSwitcher::Stop()
{
    if (!m_running) return;
    m_running = false;

    // 清除按键回调
    m_monitor.SetKeyPressCallback(nullptr);

    // 唤醒工作线程使其退出
    m_cv.notify_all();

    if (m_workerThread.joinable())
        m_workerThread.join();
}

bool InputSwitcher::IsRunning() const
{
    return m_running;
}

// ================================================================
// OnKeyEvent：按键事件处理（在 KeyboardMonitor 消息线程上调用）
//
// 过滤规则：
//   - hDevice == NULL  : SendInput 注入的模拟键，直接忽略（防止递归）
//   - flags & RI_KEY_BREAK : 按键抬起事件，忽略（只处理按下）
//   - 部分功能键不触发切换（避免快捷键自身被误处理）
// ================================================================
void InputSwitcher::OnKeyEvent(HANDLE hDevice, USHORT vkey,
                                USHORT /*scanCode*/, USHORT flags)
{
    // 忽略注入的模拟键（hDevice==NULL 是 SendInput 的特征）
    if (hDevice == nullptr) return;

    // 只处理按下事件
    if (flags & RI_KEY_BREAK) return;

    // 忽略修饰键自身，避免误触发
    if (vkey == VK_SHIFT   || vkey == VK_LSHIFT   || vkey == VK_RSHIFT   ||
        vkey == VK_CONTROL || vkey == VK_LCONTROL  || vkey == VK_RCONTROL ||
        vkey == VK_MENU    || vkey == VK_LMENU     || vkey == VK_RMENU    ||
        vkey == VK_CAPITAL || vkey == VK_LWIN      || vkey == VK_RWIN)
        return;

    // 检测是否需要触发修正：
    //   - 首次按键（m_lastDevice == nullptr）：启动后尚未确认当前语言，需修正
    //   - 键盘切换（hDevice != m_lastDevice）：切换到不同键盘，需修正
    //   - 同一键盘（hDevice == m_lastDevice）：无需修正
    if (m_lastDevice != nullptr && hDevice == m_lastDevice)
        return;

    m_lastDevice = hDevice;

    // 查询新键盘的目标语言
    // 通过 GetKeyboards() 找到对应设备路径
    auto keyboards = m_monitor.GetKeyboards();
    std::wstring devicePath;
    for (const auto& kb : keyboards)
    {
        if (kb.hDevice == hDevice)
        {
            devicePath = kb.deviceName;
            break;
        }
    }

    if (devicePath.empty()) return; // 未在列表中（可能是过滤掉的设备）

    InputLang targetLang = m_config.GetLang(devicePath);

    // 记录 Shift 状态（需要在按键发生时捕获，用于重新键入时保持大小写）
    bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    // CapsLock 不在此捕获：工作线程执行有延迟，届时用 GetAsyncKeyState 实时读取

    // 投递切换任务到工作线程（不在消息线程上直接 SendInput，避免阻塞）
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        // 若队列已有待处理任务则替换（取最新状态）
        while (!m_taskQueue.empty()) m_taskQueue.pop();
        m_taskQueue.push({ targetLang, vkey, shiftDown });
    }
    m_cv.notify_one();
}

// ================================================================
// WorkerThreadFunc：工作线程，取出任务后执行切换序列
// ================================================================
void InputSwitcher::WorkerThreadFunc()
{
    while (m_running)
    {
        SwitchTask task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            // 等待任务或停止信号
            m_cv.wait(lock, [this] {
                return !m_taskQueue.empty() || !m_running;
            });

            if (!m_running && m_taskQueue.empty()) break;
            if (m_taskQueue.empty()) continue;

            task = m_taskQueue.front();
            m_taskQueue.pop();
        }

        PerformSwitch(task.targetLang, task.vkey, task.shiftDown);
    }
}

// ================================================================
// PerformSwitch：执行完整的语言修正序列
//   1. Backspace
//   2. 关闭 CapsLock（若需要）
//   3. 语言切换快捷键
//   4. 重新键入原始按键
// ================================================================
void InputSwitcher::PerformSwitch(InputLang targetLang, USHORT vkey,
                                   bool shiftDown)
{
    // 短暂延迟，确保原始按键已被目标窗口处理完毕
    Sleep(20);

    // ---- 步骤 1：Backspace 删除错误字符 ----
    SendKeyPress(VK_BACK);
    Sleep(20);

    // ---- 步骤 2：若大写锁定（CapsLock）处于开启状态则关闭 ----
    // 注意：必须使用 GetKeyState 而非 GetAsyncKeyState
    //   GetKeyState(VK_CAPITAL)  低位（& 0x0001）= 切换状态（1=开启，0=关闭）——正确
    //   GetAsyncKeyState(VK_CAPITAL) 低位 = 上次调用后该键是否被按过——与开关状态无关，不可用
    // CapsLock 的切换状态是全局系统属性（LED 状态），
    // GetKeyState 的低位直接反映此全局状态，在工作线程中调用同样准确
    if (GetKeyState(VK_CAPITAL) & 0x0001)
    {
        SendKeyPress(VK_CAPITAL); // 按一次 CapsLock：开启 -> 关闭
        Sleep(20);
    }

    // ---- 步骤 3：发送语言切换快捷键 ----
    const HotkeyDef& hk = (targetLang == InputLang::Chinese)
        ? m_config.GetHotkeyToChinese()
        : m_config.GetHotkeyToEnglish();

    if (!hk.key.empty())
    {
        SendHotkey(hk);
        // 等待 IME 完成语言切换（部分输入法需要较长时间响应）
        Sleep(80);
    }

    // ---- 步骤 4：重新键入原始按键 ----
    if (shiftDown) SendKeyDown(VK_SHIFT);
    SendKeyPress(static_cast<WORD>(vkey));
    if (shiftDown) SendKeyUp(VK_SHIFT);
}

// ================================================================
// SendInput 辅助函数
// ================================================================

void InputSwitcher::SendKeyDown(WORD vk)
{
    INPUT input    = {};
    input.type     = INPUT_KEYBOARD;
    input.ki.wVk   = vk;
    input.ki.dwFlags = 0;
    SendInput(1, &input, sizeof(INPUT));
}

void InputSwitcher::SendKeyUp(WORD vk)
{
    INPUT input      = {};
    input.type       = INPUT_KEYBOARD;
    input.ki.wVk     = vk;
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

void InputSwitcher::SendKeyPress(WORD vk)
{
    SendKeyDown(vk);
    SendKeyUp(vk);
}

void InputSwitcher::SendHotkey(const HotkeyDef& hk)
{
    WORD vk = KeyNameToVK(hk.key);
    if (vk == 0) return; // 无法识别的键名，跳过

    // 按下修饰键
    if (hk.ctrl)  SendKeyDown(VK_CONTROL);
    if (hk.shift) SendKeyDown(VK_SHIFT);
    if (hk.alt)   SendKeyDown(VK_MENU);

    // 按下并抬起主键
    SendKeyDown(vk);
    Sleep(10);
    SendKeyUp(vk);

    // 抬起修饰键（逆序）
    if (hk.alt)   SendKeyUp(VK_MENU);
    if (hk.shift) SendKeyUp(VK_SHIFT);
    if (hk.ctrl)  SendKeyUp(VK_CONTROL);
}

// ================================================================
// KeyNameToVK：字符串键名 -> 虚拟键码
// ================================================================
WORD InputSwitcher::KeyNameToVK(const std::wstring& name)
{
    // F1 - F24
    for (int i = 1; i <= 24; ++i)
    {
        std::wstring fn = L"F" + std::to_wstring(i);
        if (name == fn) return static_cast<WORD>(VK_F1 + i - 1);
    }

    // 单个字母 A-Z（不区分大小写）
    if (name.size() == 1)
    {
        wchar_t c = name[0];
        if (c >= L'a' && c <= L'z') return static_cast<WORD>(L'A' + (c - L'a'));
        if (c >= L'A' && c <= L'Z') return static_cast<WORD>(c);
        if (c >= L'0' && c <= L'9') return static_cast<WORD>(c);
    }

    // 常用特殊键
    static const std::map<std::wstring, WORD> table = {
        { L"BACKSPACE", VK_BACK      },
        { L"TAB",       VK_TAB       },
        { L"ENTER",     VK_RETURN    },
        { L"ESC",       VK_ESCAPE    },
        { L"ESCAPE",    VK_ESCAPE    },
        { L"SPACE",     VK_SPACE     },
        { L"PAGEUP",    VK_PRIOR     },
        { L"PAGEDOWN",  VK_NEXT      },
        { L"END",       VK_END       },
        { L"HOME",      VK_HOME      },
        { L"LEFT",      VK_LEFT      },
        { L"UP",        VK_UP        },
        { L"RIGHT",     VK_RIGHT     },
        { L"DOWN",      VK_DOWN      },
        { L"INSERT",    VK_INSERT    },
        { L"DELETE",    VK_DELETE    },
        { L"CAPSLOCK",  VK_CAPITAL   },
        { L"NUMLOCK",   VK_NUMLOCK   },
        { L"SCROLL",    VK_SCROLL    },
        { L"PAUSE",     VK_PAUSE     },
        { L"PRINTSCREEN", VK_SNAPSHOT },
    };

    // 转大写查表
    std::wstring upper = name;
    for (auto& c : upper) c = (wchar_t)towupper(c);

    auto it = table.find(upper);
    if (it != table.end()) return it->second;

    return 0; // 未识别
}
