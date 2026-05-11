# 键盘切换监测程序 - 模块二：输入语言配置

> 关联模块一日志：DEVLOG.md（键盘监测）
> 模块二职责：为每块键盘维护"默认输入语言"标签（中/英），持久化到本地配置文件

---

## 模块设计概览

### 新增文件
| 文件 | 职责 |
|------|------|
| `InputConfig.h` | 输入语言配置模块公共接口 |
| `InputConfig.cpp` | 配置读写实现 |
| `keyboard_config.ini` | 本地持久化配置文件（运行时生成，位于 exe 同目录）|

### 配置文件格式（keyboard_config.ini）
```ini
# Keyboard Input Language Configuration
# Format: <device_path>=<EN|CN>
\\?\HID#VID_05AC&PID_024F&MI_00#b&146ae608&0&0000#...=CN
\\?\HID#VID_05AC&PID_024F&MI_00#b&39814f36&0&0000#...=EN
```

### 模块间依赖关系
```
keyboard_shift_dev.cpp (main)
    |-- KeyboardMonitor  (监测 + 显示)
    |       |-- LangProvider 回调 (由 main 注入)
    |-- InputConfig      (配置读写，独立无依赖)
            |-- keyboard_config.ini
```

---

## 对话记录

---

### [第 1 次] 2026-05-11 -- Step 1：键盘语言标签 + 本地配置持久化

#### 需求描述
- 为每块检测到的键盘添加"默认输入语言"标签（CN=中文 / EN=英文）
- 支持在命令行中修改切换
- 设置写入本地配置文件，程序重启后读取恢复
- 查看键盘状态时同步显示语言标签

#### 实现方案

**InputConfig 模块（新建）：**
| 接口 | 说明 |
|------|------|
| `GetLang(devicePath)` | 获取某键盘的语言设置（默认 EN）|
| `SetLang(devicePath, lang)` | 设置语言 |
| `ToggleLang(devicePath)` | 切换语言（EN↔CN）|
| `Save()` | 写入配置文件 |
| `Load()` | 从配置文件读取 |
| `LangToStr(lang)` | 枚举转显示字符串（"EN" / "CN"）|

**KeyboardMonitor 扩展：**
- 新增 `SetLangProvider(fn)` 方法，接受一个回调 `(devicePath)->wstring`
- `PrintStatus()` / `RunStatusView()` 调用回调获取语言标签并显示

**main 扩展：**
- 新增命令 `l`：列出键盘 + 当前语言，输入编号切换语言，自动保存配置
- 启动时 `Load()` 配置，注入 `LangProvider` 回调到 monitor

#### 涉及文件
| 文件 | 操作 |
|------|------|
| `InputConfig.h` | 新建 |
| `InputConfig.cpp` | 新建 |
| `KeyboardMonitor.h` | 新增 `LangProvider` 类型别名和 `SetLangProvider()` 声明 |
| `KeyboardMonitor.cpp` | `PrintStatus()` 增加语言标签行 |
| `keyboard_shift_dev.cpp` | 整合 InputConfig，新增 `l` 命令 |

#### 反馈 / 问题记录
- 程序正常运行，配置文件生成为 INI 格式，需改为 JSON 格式

---

### [第 2 次] 2026-05-11 -- 配置文件改为 JSON 格式

#### 需求描述
将 `keyboard_config.ini` 改为 `keyboard_config.json`，格式更易读易修改。

#### JSON 格式设计
```json
{
    "version": 1,
    "keyboards": {
        "\\\\?\\HID#VID_05AC&PID_024F&MI_00#b&39814f36...": {
            "friendly_name": "GS3104T PRO",
            "lang": "CN"
        },
        "\\\\?\\HID#VID_05AC&PID_024F&MI_00#b&146ae608...": {
            "friendly_name": "GS3104T-PRO 2.4G",
            "lang": "EN"
        }
    }
}
```

#### 实现方案
不引入外部库，手动实现：
- **Save()**：字符串拼接生成格式化 JSON，宽字符转 UTF-8 写文件（含 BOM）
- **Load()**：自制最小 `JsonParser` 递归下降解析器，`parseObject(callback)` 模板方法
  驱动解析，按需提取 `lang` 和 `friendly_name` 字段
- **JsonEscape / JsonUnescape**：处理设备路径中的反斜杠转义

新增接口：
- `SetFriendlyName(devicePath, name)` -- 保存前由 main 注入系统获取的实际名称，写入 JSON 可读字段
- `m_friendlyNames` 内部 map

文件名从 `.ini` 改为 `.json`（`GetDefaultFilePath` 返回值更新）。

#### 涉及文件
| 文件 | 操作 |
|------|------|
| `InputConfig.h` | 新增 `SetFriendlyName()` 声明、`m_friendlyNames` 成员、注释更新 |
| `InputConfig.cpp` | 全文重写：JSON 生成器 + 手写解析器 |
| `keyboard_shift_dev.cpp` | 保存前调用 `SetFriendlyName` 同步名称 |

#### 反馈 / 问题记录
- 警告：`ReadFile` 返回值被忽略 → 已修复（检查返回值并 resize 缓冲区）
- 配置文件成功生成为 JSON 格式

---

### [第 3 次] 2026-05-11 -- 快捷键配置写入 JSON

#### 需求描述
将切换语言的快捷键写入 JSON 配置文件，供后续模块调用：
- Ctrl+Shift+F11 → 切换到英文
- Ctrl+Shift+F12 → 切换到中文

#### JSON 新增 hotkeys 节
```json
{
    "version": 1,
    "hotkeys": {
        "switch_to_english": {
            "ctrl": true,
            "shift": true,
            "alt": false,
            "key": "F11"
        },
        "switch_to_chinese": {
            "ctrl": true,
            "shift": true,
            "alt": false,
            "key": "F12"
        }
    },
    "keyboards": { ... }
}
```

#### 实现方案
| 改动 | 说明 |
|------|------|
| `HotkeyDef` 结构体（新增在 `.h`）| 含 `ctrl/shift/alt` bool + `key` 字符串 + `ToString()` 方法 |
| `InputConfig` 成员 | `m_hotkeyToEnglish` / `m_hotkeyToChinese`，构造函数写默认值 |
| `InputConfig` 接口 | `Get/SetHotkeyToEnglish()` / `Get/SetHotkeyToChinese()` |
| `JsonParser::parseBool()` | 新增，识别 `true`/`false` 字面量 |
| `Save()` | 新增 `hotkeys` 节序列化，`hotkeyJson()` lambda 格式化单个快捷键对象 |
| `Load()` | 顶层回调新增 `hotkeys` 分支，嵌套解析两个快捷键对象 |
| `keyboard_shift_dev.cpp` | 启动时打印当前快捷键配置供用户确认 |

#### 涉及文件
| 文件 | 操作 |
|------|------|
| `InputConfig.h` | 新增 `HotkeyDef` 结构体，新增成员和接口声明 |
| `InputConfig.cpp` | 构造函数初始化默认值，Load/Save 新增 hotkeys 节，实现 4 个新接口 |
| `keyboard_shift_dev.cpp` | 启动界面展示快捷键提示 |

#### 反馈 / 问题记录
> _(待调试后填写)_

---

<!-- 后续对话追加在此处 -->
