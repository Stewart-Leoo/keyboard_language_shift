# 键盘切换监测程序 - 模块三：输入语言自动切换

> 关联模块一日志：DEVLOG.md（键盘监测）
> 关联模块二日志：DEVLOG_Module2.md（语言配置）
> 模块三职责：检测到键盘切换时，自动执行语言修正序列，实现每块键盘锁定其配置语言

---

## 模块设计概览

### 核心流程

当检测到"新的按键来自与上一次不同的键盘"时，触发修正序列：

```
用户按下某键（来自新键盘）
    │
    ▼
1. 发送 Backspace          ← 删除刚输入的错误字符
    │
    ▼
2. 检查 CapsLock 状态
   若开启 → 发送 CapsLock   ← 关闭大写锁定
    │
    ▼
3. 发送语言切换快捷键        ← 根据新键盘的 lang 和 hotkeys 配置
   (Ctrl+Shift+F11=EN / Ctrl+Shift+F12=CN)
    │
    ▼
4. 重新发送原始按键          ← 在正确语言环境下重新输入
```

### 注入键过滤
SendInput 发出的模拟键，在 Raw Input 中 `hDevice == NULL`，
InputSwitcher 通过此特征过滤自身注入的键，避免递归触发。

### 新增文件
| 文件 | 职责 |
|------|------|
| `InputSwitcher.h` | 自动切换模块公共接口 |
| `InputSwitcher.cpp` | 切换逻辑实现 |

### 依赖关系
```
keyboard_shift_dev.cpp (main)
    |-- KeyboardMonitor  (监测 + 提供按键回调)
    |-- InputConfig      (提供语言设置 + 快捷键定义)
    |-- InputSwitcher    (订阅按键回调，执行切换序列)
            |-- 依赖 KeyboardMonitor::OnKeyPressed 回调
            |-- 依赖 InputConfig::GetLang / GetHotkey
```

---

## 对话记录

---

### [第 1 次] 2026-05-11 -- Step 1：实现键盘切换后的语言自动修正

#### 需求描述
检测到输入键盘切换后，依次执行：
1. 退格键（删除错误字符）
2. 关闭 CapsLock（如已开启）
3. 发送单项语言切换快捷键（根据新键盘 lang 配置）
4. 重新键入原始字符

#### 实现方案

**KeyboardMonitor 扩展：**
- 新增 `OnKeyPressed` 回调类型：`(HANDLE hDevice, USHORT vkey, USHORT flags)`
- 新增 `SetKeyPressCallback(fn)` 方法
- `WndProc` 在记录击键 tick 的同时调用此回调

**InputSwitcher 设计：**
| 组件 | 说明 |
|------|------|
| `Start()` | 向 KeyboardMonitor 注册 OnKeyPressed 回调 |
| `Stop()` | 清除回调，停止工作线程 |
| `OnKeyEvent()` | 回调处理：判断是否发生键盘切换，若是则投递切换任务 |
| 工作线程 | 从任务队列取出切换任务，调用 `PerformSwitch()` |
| `PerformSwitch()` | 用 `SendInput` 依次发送：Backspace、(CapsLock)、热键、原始按键 |
| `KeyNameToVK()` | 字符串键名 → 虚拟键码（F1-F24、A-Z、0-9 等）|

**注入键过滤：**
`hDevice == NULL` 的按键事件为 SendInput 注入，直接忽略，不触发切换逻辑。

#### 涉及文件
| 文件 | 操作 |
|------|------|
| `InputSwitcher.h` | 新建 |
| `InputSwitcher.cpp` | 新建 |
| `KeyboardMonitor.h` | 新增 `OnKeyPressed` 类型和 `SetKeyPressCallback()` 声明 |
| `KeyboardMonitor.cpp` | 在 WndProc 中调用按键回调 |
| `keyboard_shift_dev.cpp` | 创建 InputSwitcher 实例，启动，新增 `t` 命令开关 |

#### 反馈 / 问题记录
- 启动后首次输入未触发语言修正（只有切换键盘时才触发）

---

### [第 2 次] 2026-05-11 -- 首次输入也触发语言修正

#### 需求描述
程序启动后第一次按键也应进行语言检测并修正，补足只在"切换键盘"时响应的缺陷。

#### 修改内容
`OnKeyEvent` 中去掉 `m_lastDevice == nullptr` 时的豁免返回，
改为统一判断：**只要不是"与上次相同的键盘"就触发修正**。

```
// 修改前
if (m_lastDevice == nullptr) { m_lastDevice = hDevice; return; }  // 首次跳过
if (hDevice == m_lastDevice) return;

// 修改后
if (m_lastDevice != nullptr && hDevice == m_lastDevice) return;   // 只有同一键盘才跳过
m_lastDevice = hDevice;
// 首次 or 切换 → 继续执行修正序列
```

#### 触发条件对比
| 场景 | 修改前 | 修改后 |
|------|--------|--------|
| 启动后首次按键 | 跳过 | 触发修正 |
| 切换到另一块键盘 | 触发修正 | 触发修正 |
| 同一块键盘连续输入 | 跳过 | 跳过 |

#### 涉及文件
| 文件 | 操作 |
|------|------|
| `InputSwitcher.cpp` | 修改 `OnKeyEvent` 中的首次按键判断逻辑 |
| `DEVLOG_Module3.md` | 更新 |

#### 反馈 / 问题记录
- 切换键盘时 CapsLock 反而被打开了

---

### [第 3 次] 2026-05-11 -- 修复 CapsLock 被错误打开的问题

#### 根本原因
`GetAsyncKeyState` 与 `GetKeyState` 对 CapsLock 低位含义不同：

| 函数 | 低位（`& 0x0001`）含义 |
|---|---|
| `GetKeyState(VK_CAPITAL)` | **切换状态**：1=锁定开启，0=锁定关闭 ✓ |
| `GetAsyncKeyState(VK_CAPITAL)` | **上次调用后是否被按过**：与开关状态无关 ✗ |

使用 `GetAsyncKeyState` 时，切换键盘动作本身会让该低位为 1，导致不管 CapsLock 当前状态如何，都会错误地按下一次，把它从关变成开。

CapsLock 的切换状态是全局系统属性（对应键盘 LED 状态），`GetKeyState` 的低位直接反映此全局状态，在工作线程中调用同样准确。

#### 修复内容
`InputSwitcher.cpp` 的 `PerformSwitch()` 中，将：
```cpp
if (GetAsyncKeyState(VK_CAPITAL) & 0x0001)
```
改为：
```cpp
if (GetKeyState(VK_CAPITAL) & 0x0001)
```

#### 历史变更说明
| 版本 | 使用函数 | 问题 |
|------|----------|------|
| 初版 | `GetKeyState`（在消息线程捕获） | 投入队列后状态可能陈旧 |
| 修改1 | `GetAsyncKeyState`（在工作线程实时读取）| 低位语义错误，反而打开 CapsLock |
| 修改2（最终）| `GetKeyState`（在工作线程实时读取）| 低位=切换状态，全局属性，工作线程可用 |

#### 涉及文件
| 文件 | 操作 |
|------|------|
| `InputSwitcher.cpp` | `PerformSwitch()` 中将 `GetAsyncKeyState` 改回 `GetKeyState` |
| `DEVLOG_Module3.md` | 更新 |

#### 反馈 / 问题记录
> _(待调试后填写)_

---

<!-- 后续对话追加在此处 -->
