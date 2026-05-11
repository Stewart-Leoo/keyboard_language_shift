# 键盘切换监测程序 开发日志

> 项目路径：`keyboard_shift_dev/keyboard_shift_dev/keyboard_shift_dev.cpp`
> 开发环境：Visual Studio (C++, Windows x64)
> 目标：实现锁定特定键盘中英文默认设置的后台程序

---

## 对话记录

---

### [第 1 次] 2026-05-11 — Step 1：实时键盘连接检测

#### 需求描述
实现实时键盘检测功能，程序要一直运行并监测并显示出当前键盘连接情况。

#### 实现方案
使用 **Windows Raw Input API**（`GetRawInputDeviceList`）枚举系统中所有已注册的 HID 键盘设备。  
主循环每秒轮询一次，比较前后两次枚举结果，检测设备插入/拔出事件。

#### 涉及文件
| 文件 | 操作 | 说明 |
|------|------|------|
| `keyboard_shift_dev.cpp` | 重写 | 第一步主程序，包含全部键盘枚举与监测逻辑 |
| `DEVLOG.md` | 新建 | 本开发日志文件 |

#### 关键API & 依赖
| API / 头文件 / 库 | 用途 |
|-------------------|------|
| `GetRawInputDeviceList` | 枚举所有 Raw Input 设备 |
| `GetRawInputDeviceInfoW` | 获取设备路径字符串 |
| `HidD_GetProductString` | 读取 HID 设备产品名称（友好名称）|
| `<hidsdi.h>` + `hid.lib` | HID 设备 API |
| `<setupapi.h>` + `setupapi.lib` | 设备安装 API（头文件已引入，库需在项目中手动添加）|

#### VS 项目需要手动添加的附加依赖项
在 VS **项目属性 → 链接器 → 输入 → 附加依赖项** 中添加：
```
hid.lib
setupapi.lib
```

#### 程序输出示例
```
================================================
  键盘连接监测程序  v0.1  (Step 1: 设备枚举)
  按 Ctrl+C 退出
================================================

========================================
  当前已连接键盘数量：2
========================================
  [键盘 1]
    句柄      : 0x00000...
    友好名称  : USB Keyboard
    设备路径  : \\?\HID#VID_046D&PID_C31C&MI_00#...
  [键盘 2]
    句柄      : 0x00000...
    友好名称  : HID Keyboard Device
    设备路径  : \\?\HID#VID_0000&PID_0000&MI_00#...
========================================

[+] 检测到键盘接入：USB Keyboard
    路径：\\?\HID#...
```

#### 已知注意事项
- `_setmode(_fileno(stdout), _O_U16TEXT)` 需要 `#include <fcntl.h>` 和 `#include <io.h>`，  
  若编译报错 `_O_U16TEXT` 未定义，请在 cpp 顶部补充这两个头文件（已在代码中列出，  
  如 VS 提示缺失请手动添加）。
- 部分虚拟/系统键盘设备（如 HID-compliant keyboard 驱动的内部键盘）也会被枚举出来，  
  后续步骤会根据 VID/PID 或设备路径前缀进行过滤。

#### 反馈 / 问题记录
- 运行结果出现 4 条键盘记录，实为同一物理键盘的多个 HID 接口（MI_00/MI_01/MI_02）
- 用户需求：只关注有物理信号输入的设备

---

### [第 2 次] 2026-05-11 — Step 2：过滤附属接口 + Raw Input 活跃状态检测

#### 需求描述
`GetRawInputDeviceList` 把同一物理键盘的多个 HID 子接口都列出来（消费者控制、系统控制等）。  
用户只关注**当前有物理信号输入**的设备，需要：
1. 过滤掉附属接口，只显示标准键盘主接口
2. 区分"已连接"与"有物理击键"两种状态

#### 实现方案
| 改进点 | 方案 |
|--------|------|
| 过滤附属接口 | 读取 `RIDI_DEVICEINFO.hid.usUsagePage/usUsage`，只保留 UsagePage=0x01 & Usage=0x06 的设备；若 usUsagePage=0（内核 kbdclass 驱动）也保留 |
| 检测物理击键 | 独立消息线程创建 `HWND_MESSAGE` 隐藏窗口，以 `RIDEV_INPUTSINK` 注册 Raw Input，在 `WM_INPUT` 回调中记录 `g_lastInputTick[hDevice]` |
| 活跃状态显示 | 主循环比对当前 tick 与最后击键 tick，5秒内有击键标记为 `★ 活跃` |

#### 涉及文件
| 文件 | 操作 | 说明 |
|------|------|------|
| `keyboard_shift_dev.cpp` | 重写 | v0.2，增加 Usage 过滤、消息线程、活跃状态标注 |
| `DEVLOG.md` | 更新 | 补充第1次反馈 + 第2次记录 |

#### 新增依赖
链接库无变化，仍需 `hid.lib` + `setupapi.lib`。  
新增头文件：`<mutex>`、`<thread>`（C++11 标准库，VS 默认支持）。

#### 预期运行效果
```
  [键盘 1]  [★ 活跃 - 有物理输入]
    友好名称  : GS3104T PRO
    句柄      : 0x000100AB
    最后输入  : 2 秒前
    设备路径  : \\?\HID#VID_05AC&PID_024F&MI_00#...

  [键盘 2]  [  已连接 - 暂无输入]
    友好名称  : GS3104T-PRO 2.4G
    ...
```

#### 反馈 / 问题记录
- 运行结果：`已连接键盘（主接口）数量：0`，程序枚举为空
- 用户需求补充：只显示 USB / 2.4G 外接物理键盘，不要虚拟/内置键盘

---

### [第 3 次] 2026-05-11 — Step 2 BugFix：修复过滤逻辑导致枚举为0

#### 根本原因
`RID_DEVICE_INFO` 是 union 结构体：
- `dwType == RIM_TYPEKEYBOARD` 时，数据填入 `info.keyboard` 成员
- 代码错误地读了 `info.hid.usUsagePage`，该字段与 `keyboard.dwNumberOfFunctionKeys` 内存重叠
- 键盘有12个功能键 → `usUsagePage` 读出12，不等于0x01，导致所有设备被过滤

#### 修复方案
废弃 `RIDI_DEVICEINFO` 的 HID Usage 判断，改用**设备路径字符串过滤**：

| 过滤规则 | 说明 |
|----------|------|
| 路径必须含 `HID#VID_` | 真实物理USB/2.4G设备才有 VID/PID，内置键盘路径无此段 |
| 路径不含 `&Col` | 排除子集合附属接口（MI_01&Col03、MI_02&Col03等）|
| 路径不含 `ROOT`/`ACPI`/`TERMINPUT` | 排除内置键盘和远程桌面虚拟键盘 |

#### 涉及文件
| 文件 | 操作 | 说明 |
|------|------|------|
| `keyboard_shift_dev.cpp` | 修改 `EnumerateKeyboards()` | 替换过滤逻辑，移除无效常量 |
| `DEVLOG.md` | 更新 | 补充第2次反馈 + 第3次记录 |

#### 反馈 / 问题记录
编译出现以下警告/错误，需修复：
- `KB_USAGE_PAGE` / `KB_USAGE` 未定义（常量已删但 MessageThreadFunc 遗漏更新）
- `KeyboardInfo::hDevice` 未初始化成员警告
- `GetTickCount` 建议改为 `GetTickCount64`（约49天溢出）
- `_setmode` 返回值被忽略警告

---

### [第 4 次] 2026-05-11 — 修复所有编译警告/错误

#### 修复清单

| 问题 | 修复方式 |
|------|----------|
| `KB_USAGE_PAGE`/`KB_USAGE` 未定义 | `MessageThreadFunc` 中改为直接写字面量 `0x01` / `0x06` |
| `hDevice` 未初始化 | 结构体成员改为 `HANDLE hDevice = nullptr;` |
| `GetTickCount` 溢出风险 | 全部改为 `GetTickCount64()`，类型 `DWORD` → `ULONGLONG` |
| `_setmode` 返回值忽略 | 改为 `(void)_setmode(...)` 显式丢弃 |

#### 涉及文件
| 文件 | 操作 | 说明 |
|------|------|------|
| `keyboard_shift_dev.cpp` | 修改 | 修复4处编译警告/错误，版本仍为 v0.2 |
| `DEVLOG.md` | 更新 | 补充第3次反馈 + 第4次记录 |

#### 反馈 / 问题记录
- 编译警告/错误全部修复，程序正常运行

---

### [第 5 次] 2026-05-11 — 优化控制台显示，增加实时清屏

#### 需求描述
终端输出不断滚动，需要及时清屏，实现类似监控面板的原地刷新效果。

#### 实现方案
新增 `ClearConsole()` 函数，使用 Windows 控制台 API 原地清屏（无闪烁）：
- `FillConsoleOutputCharacterW` 用空格填满缓冲区
- `FillConsoleOutputAttribute` 重置颜色属性
- `SetConsoleCursorPosition` 光标归位 (0,0)

`PrintKeyboards()` 每次调用前先执行清屏，输出带边框的键盘状态面板，底部显示本地时间戳。  
插拔事件发生时先清屏打印提示，停留 1.5 秒后再恢复正常刷新。

#### 涉及文件
| 文件 | 操作 | 说明 |
|------|------|------|
| `keyboard_shift_dev.cpp` | 修改 | 新增 `ClearConsole()`，重写 `PrintKeyboards()` 显示格式，调整主循环插拔提示逻辑 |
| `DEVLOG.md` | 更新 | 补充第4次反馈 + 第5次记录 |

#### 反馈 / 问题记录
- 效果已达成，需要将功能封装为独立模块

---

### [第 6 次] 2026-05-11 — 模块化重构：键盘检测封装为 KeyboardMonitor

#### 需求描述
将键盘检测功能从 main 中摘出，封装成独立模块，暴露干净接口供后续功能解耦使用。

#### 文件结构
```
keyboard_shift_dev/
├── KeyboardMonitor.h        ← 新建：公共接口（头文件）
├── KeyboardMonitor.cpp      ← 新建：全部实现
└── keyboard_shift_dev.cpp   ← 重写：仅保留 main + 显示逻辑（Render/ClearConsole）
```

#### KeyboardMonitor 公共接口
| 方法 | 说明 |
|------|------|
| `Start(callback, pollIntervalMs)` | 启动消息线程+轮询线程，注册插拔回调 |
| `Stop()` | 停止监测，等待线程退出 |
| `GetKeyboards()` | 返回当前已连接键盘列表快照（线程安全）|
| `IsActive(hDevice, thresholdMs)` | 判断键盘是否在阈值内有过击键 |
| `GetMsSinceLastInput(hDevice)` | 返回距上次击键毫秒数（无记录返回 ULLONG_MAX）|

#### 设计要点
- `WndProc` 为静态函数，通过 `GWLP_USERDATA` 存取 `this` 指针，解决静态回调访问实例数据问题
- 插拔回调在锁外触发，防止调用方在回调中再调用 `GetKeyboards()` 时死锁
- 析构函数自动调用 `Stop()`，资源安全释放
- `keyboard_shift_dev.cpp` 中的 `main` 只需 `#include "KeyboardMonitor.h"`，不含任何检测逻辑

#### VS 项目操作
需在解决方案资源管理器中手动"添加现有项"将 `KeyboardMonitor.h` 和 `KeyboardMonitor.cpp` 加入项目。

#### 反馈 / 问题记录
- 模块化重构完成，编译通过

---

### [第 7 次] 2026-05-11 — 修改活跃/静默定义

#### 需求描述
将"活跃"的判定从时间窗口（5秒内有击键）改为：**最后一次有输入来源的键盘 = 活跃，其余连接中的键盘 = 静默**。同一时刻最多只有一块键盘处于活跃状态。

#### 实现方案
| 文件 | 改动 |
|------|------|
| `KeyboardMonitor.h` | 增加 `m_lastActiveDevice` 成员、`GetLastActiveDevice()` 接口声明；移除 `IsActive()` 声明 |
| `KeyboardMonitor.cpp` | 构造函数初始化 `m_lastActiveDevice = nullptr`；`WndProc` 按键记录处同步写 `m_lastActiveDevice = hDev`；实现 `GetLastActiveDevice()` |
| `keyboard_shift_dev.cpp` | `Render()` 中改用 `GetLastActiveDevice()` 与各键盘句柄比对，决定活跃标记；移除 `ACTIVE_MS` 常量 |

#### 反馈 / 问题记录
- 编译报错：`"IsActive": 不是 "KeyboardMonitor" 的成员`
- 编译警告：三个文件均提示代码页 936 无法表示某些字符

---

### [第 8 次] 2026-05-11 -- 修复 IsActive 残留实现 + 代码页 936 字符问题

#### 根本原因

| 问题 | 原因 |
|------|------|
| `IsActive` 编译报错 | 上次从头文件删除了声明，但 `.cpp` 里第88-97行实现仍在，编译器将其视为非成员函数 |
| 代码页 936 警告 | 源文件中含 GBK 不支持的 Unicode 字符：`─ │ ┌ ┐ └ ┘ ╔ ║ ╚ ★ ·`（Unicode 制表符和特殊符号）|

#### 修复方式
- 删除 `KeyboardMonitor.cpp` 中 `IsActive` 的实现
- 三个文件全部将非 ASCII 字符替换为 ASCII 等价：
  - 注释装饰 `────` → `====` / `----`
  - 显示字符串 `╔║╚` → `+|`，`★` → `[ 活跃 ]`，`·` → `[ 静默 ]`，`┌─┐└┘│` → `+--+|`

#### 涉及文件
| 文件 | 操作 |
|------|------|
| `KeyboardMonitor.h` | 全文重写，清除所有非 ASCII 字符 |
| `KeyboardMonitor.cpp` | 全文重写，删除 `IsActive` 实现，清除非 ASCII 字符 |
| `keyboard_shift_dev.cpp` | 全文重写，显示字符串换用纯 ASCII 边框 |

#### 反馈 / 问题记录
- 编译问题全部解决，程序正常运行

---

### [第 9 次] 2026-05-11 -- 显示逻辑入模块，主函数改为命令驱动

#### 需求描述
主函数启动后默认后台监测，只有输入指令后才在控制台显示键盘详细信息；
显示功能打包进 KeyboardMonitor 模块，主函数只保留启动和命令交互。

#### 实现方案

**KeyboardMonitor 新增接口：**
| 新增 | 说明 |
|------|------|
| `PrintStatus() const` | 打印当前键盘连接状态（追加输出，不清屏）|
| `static ClearConsole()` | 清空控制台光标归位（从 main 迁移进来）|

**keyboard_shift_dev.cpp 主函数逻辑：**
| 阶段 | 说明 |
|------|------|
| 启动 | `monitor.Start(...)` 注册插拔回调后立即返回，后台静默运行 |
| 命令循环 | `std::getline(std::wcin, line)` 读取命令行输入 |
| `s` / Enter | 调用 `monitor.PrintStatus()` 显示当前状态 |
| `q` | `monitor.Stop()` 后退出 |
| `h` | 打印帮助信息 |

#### 运行效果
```
+==========================================+
|      键盘切换监测程序  v0.4              |
|  后台监测已启动，输入命令查看状态        |
+==========================================+

  后台监测已就绪。输入 h 查看命令帮助。

> 输入命令 (s=键盘状态  q=退出  h=帮助): s

  +==========================================+
  |  键盘状态  @09:29:00                    |
  +==========================================+
  |  已连接：2 个                            |
  +------------------------------------------+
  |  键盘 1  [ 活跃 ]
  |    名称：GS3104T PRO
  |    输入：3 秒前
  +------------------------------------------+
  |  键盘 2  [ 静默 ]
  |    名称：GS3104T-PRO 2.4G
  |    输入：本次运行内暂无记录
  +==========================================+

> 输入命令 (s=键盘状态  q=退出  h=帮助):
```

#### 涉及文件
| 文件 | 操作 |
|------|------|
| `KeyboardMonitor.h` | 新增 `PrintStatus()` 和 `static ClearConsole()` 声明 |
| `KeyboardMonitor.cpp` | 新增 `#include <iostream>`；实现 `ClearConsole()` 和 `PrintStatus()` |
| `keyboard_shift_dev.cpp` | 重写为命令循环，移除所有显示逻辑 |

#### 反馈 / 问题记录
- 大量语法错误："缺少;"、"常量中有换行符"、乱码标识符"绉掑墠"
- 代码页 936 警告再次出现

---

### [第 10 次] 2026-05-11 -- 修复 UTF-8 BOM 缺失导致的全面乱码

#### 根本原因
Write 工具保存文件为 UTF-8 **无 BOM**。VS 在代码页 936 下看不到 BOM，
将文件当作 GBK 读取，UTF-8 多字节汉字序列被拆散成无效字节，
编译器把它们解析成奇怪的标识符/字符串常量，产生连锁语法错误。

#### 修复方式
用 PowerShell 在三个源文件头部写入 UTF-8 BOM 字节 `EF BB BF`：
```
KeyboardMonitor.h    BOM=True
KeyboardMonitor.cpp  BOM=True
keyboard_shift_dev.cpp  BOM=True
```
VS 检测到 BOM 后自动以 UTF-8 读取，中文注释和宽字符串字面量均恢复正常。

#### 后续维护说明
每次通过工具重写这三个文件后，需重新执行 BOM 添加脚本，
或在 VS 中手动"另存为 -- 编码 -- UTF-8 带签名"。

#### 反馈 / 问题记录
- 编译通过，程序正常运行，命令模式工作正常

---

### [第 11 次] 2026-05-11 -- s 命令进入实时刷新模式，q 退出返回主界面

#### 需求描述
查看键盘状态时，进入清屏实时显示模式（同之前的 Render 循环），
按 q 退出后重新显示主命令界面。

#### 实现方案
新增 `KeyboardMonitor::RunStatusView()` 方法：
- 循环执行：`ClearConsole()` -> `PrintStatus()` -> 底部提示"按 q 返回"
- 每 100ms 用 `_kbhit()` + `_getch()` 检测按键（不阻塞，不依赖 stdin 模式）
- 检测到 q/Q 时 return，回到 main 的命令循环
- `main` 的 `s` 分支调用 `RunStatusView()`，返回后清屏重绘主界面标题

#### 流程示意
```
主菜单
  |
  s --> RunStatusView() [实时刷新]
           |  按 q
           v
        返回主菜单（清屏重绘标题）
```

#### 涉及文件
| 文件 | 操作 |
|------|------|
| `KeyboardMonitor.h` | 新增 `RunStatusView()` 声明 |
| `KeyboardMonitor.cpp` | 新增 `#include <conio.h>`；实现 `RunStatusView()` |
| `keyboard_shift_dev.cpp` | `case 's'` 改调 `RunStatusView()`，返回后重绘主界面 |

#### 反馈 / 问题记录
> _(待调试后填写)_

---

<!-- 后续对话追加在此处 -->
