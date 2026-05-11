# 键盘切换监测程序 - 模块四：系统托盘运行

> 关联模块一：DEVLOG.md（键盘监测）
> 关联模块二：DEVLOG_Module2.md（语言配置）
> 关联模块三：DEVLOG_Module3.md（自动切换）
> 模块四职责：将程序改为托盘常驻运行，默认无终端窗口，通过右键菜单进入设置

---

## 模块设计概览

### 运行模式

```
程序启动
    │
    ▼
后台静默运行（无控制台窗口）
    │
    ├── KeyboardMonitor  持续监测
    ├── InputSwitcher    持续自动切换
    └── TrayApp          托盘图标 + 消息循环
            │
            右键菜单
            ├── [✓] 自动切换：已开启  ← 点击切换 ON/OFF
            ├── ─────────────
            ├── 设置...              ← 打开控制台命令界面
            ├── ─────────────
            └── 退出
```

### 控制台生命周期

"设置" 被点击时：
1. `AllocConsole()` 动态创建控制台窗口
2. 重定向 stdin/stdout/stderr 到新控制台
3. 在独立线程运行完整命令界面（s/l/t/q/h）
4. 用户输入 `q` 或关闭控制台 → `FreeConsole()` 销毁控制台，返回托盘模式

### 新增文件
| 文件 | 职责 |
|------|------|
| `TrayApp.h` | 托盘模块公共接口 |
| `TrayApp.cpp` | 托盘图标、消息循环、右键菜单实现 |

### 链接依赖
新增：`shell32.lib`（Shell_NotifyIcon）—— VS 项目已默认链接，无需手动添加

---

## 对话记录

---

### [第 1 次] 2026-05-11 -- Step 1：托盘化 + 按需打开控制台设置界面

#### 需求描述
- 程序启动后不显示终端，以托盘图标常驻运行
- 右键托盘图标弹出菜单：自动切换开关、设置、退出
- 点击"设置"后动态打开控制台，执行原有命令界面，退出后关闭控制台

#### 实现方案

**TrayApp 类：**
| 接口 | 说明 |
|------|------|
| `Run()` | 在主线程运行 Windows 消息循环（阻塞直到退出）|
| `Quit()` | 投递 WM_QUIT，退出消息循环 |
| `SetSwitcherState(bool)` | 更新菜单中"自动切换"的勾选状态 |
| `SetOnShowSettings(fn)` | 点击"设置"时的回调 |
| `SetOnToggleSwitcher(fn)` | 点击"自动切换"时的回调 |
| `SetOnExit(fn)` | 点击"退出"时的回调 |

**控制台管理（keyboard_shift_dev.cpp）：**
- `OpenConsole()` -- AllocConsole + 重定向 stdio + UTF-16 模式
- `CloseConsole()` -- FreeConsole
- 在独立线程运行命令循环，退出时关闭控制台

**子系统切换：**
在 `keyboard_shift_dev.cpp` 顶部添加：
```cpp
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
```
或在 VS 项目属性 → 链接器 → 系统 → 子系统 改为 `Windows (/SUBSYSTEM:WINDOWS)`

#### 涉及文件
| 文件 | 操作 |
|------|------|
| `TrayApp.h` | 新建 |
| `TrayApp.cpp` | 新建 |
| `keyboard_shift_dev.cpp` | 重写：托盘启动 + 控制台按需打开 |

#### 反馈 / 问题记录
- 托盘右键"设置"打开终端显示中文乱码
- CapsLock 判定失效

**乱码修复：**
- `SUBSYSTEM:WINDOWS` 启动时无控制台，`std::wcout` 写入失败设置 `failbit`，重定向后 `failbit` 残留导致输出被丢弃
- 修复：`AllocConsole` 后加 `std::wcout.clear()`
- 使用 `CreateFileW("CONOUT$")` 获取句柄替代 `GetStdHandle`（后者在此场景下可能无效）

**CapsLock 修复：**
- 原先在消息线程捕获 `capsLockOn` 放入任务队列，工作线程执行时状态已陈旧
- 修复：在 `PerformSwitch` 执行时用 `GetAsyncKeyState(VK_CAPITAL)` 实时读取

**乱码依然存在（最终方案）：**
- 将全部控制台输出字符串改为英文，彻底规避字体/编码问题
- 涉及：`keyboard_shift_dev.cpp`、`KeyboardMonitor.cpp`（PrintStatus/RunStatusView）、`TrayApp.cpp`（菜单项）

---

### [第 2 次] 2026-05-11 -- 添加自定义图标（logo.ico）

#### 需求描述
将 `logo.png` 作为 exe 图标，同时显示在系统托盘中。

#### 实现方案
Windows 程序图标须为 `.ico` 格式，需先将 `logo.png` 转换为 `logo.ico`。

**资源文件机制：**
- `resource.h`：定义图标资源 ID（`IDI_APP_ICON = 101`）
- `resource.rc`：将 `logo.ico` 编译进 exe（`IDI_APP_ICON ICON "logo.ico"`）  
  — `.rc` 文件中的第一个 ICON 资源自动成为 exe 的主图标（文件管理器/任务栏显示）

**TrayApp 修改：**
- `TrayApp.h`：`#include "resource.h"`
- `TrayApp.cpp`：将 `LoadImageW(nullptr, IDI_APPLICATION, ...)` 改为从模块内嵌资源加载：
  ```cpp
  LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON), ...)
  ```
  图标加载失败时自动回退到系统默认图标。

#### 新增文件
| 文件 | 说明 |
|------|------|
| `resource.h` | 资源 ID 定义 |
| `resource.rc` | 资源脚本，链接 logo.ico |

#### VS 项目必须操作
1. **转换图标**：将 `logo.png` 转为 `logo.ico`（建议含 16×16 和 32×32 尺寸），放入 `keyboard_shift_dev/keyboard_shift_dev/` 目录
2. **添加资源文件**：解决方案资源管理器 → 右键项目 → **添加 → 现有项**，选中 `resource.h` 和 `resource.rc`
3. `resource.rc` 需被 VS 识别为"资源文件"类型（添加后右键 → 属性 → 项目类型应为"RC 编译器"）

#### 反馈 / 问题记录
> _(待调试后填写)_

---

<!-- 后续对话追加在此处 -->
