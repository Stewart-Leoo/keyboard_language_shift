#pragma once
// InputConfig.h
// 输入语言配置模块 -- 公共接口
//
// 配置文件格式（keyboard_config.json）：
// {
//     "version": 1,
//     "hotkeys": {
//         "switch_to_english": { "ctrl": true, "shift": true, "alt": false, "key": "F11" },
//         "switch_to_chinese": { "ctrl": true, "shift": true, "alt": false, "key": "F12" }
//     },
//     "keyboards": {
//         "<device_path>": { "friendly_name": "...", "lang": "CN" }
//     }
// }

#include <Windows.h>
#include <string>
#include <map>

// ================================================================
// 输入语言枚举
// ================================================================
enum class InputLang
{
    English = 0,  // 英文（默认）
    Chinese = 1,  // 中文
};

// ================================================================
// 快捷键定义（仅存储配置，不做注册/监听，由后续模块调用）
// ================================================================
struct HotkeyDef
{
    bool         ctrl  = false; // 是否需要 Ctrl
    bool         shift = false; // 是否需要 Shift
    bool         alt   = false; // 是否需要 Alt
    std::wstring key;           // 主键名称，如 "F11"、"F12"

    // 返回人类可读字符串，如 "Ctrl+Shift+F11"
    std::wstring ToString() const
    {
        std::wstring s;
        if (ctrl)  s += L"Ctrl+";
        if (shift) s += L"Shift+";
        if (alt)   s += L"Alt+";
        s += key.empty() ? L"(未设置)" : key;
        return s;
    }
};

// ================================================================
// InputConfig 类
// ================================================================
class InputConfig
{
public:
    // 构造：filePath 为配置文件完整路径
    explicit InputConfig(const std::wstring& filePath);

    // ---- 持久化 ------------------------------------------------

    // 从文件加载；文件不存在时返回 false（使用默认值，不报错）
    bool Load();

    // 写入文件；失败返回 false
    bool Save() const;

    // ---- 键盘语言设置 ------------------------------------------

    InputLang    GetLang(const std::wstring& devicePath) const;
    void         SetLang(const std::wstring& devicePath, InputLang lang);
    void         ToggleLang(const std::wstring& devicePath);
    void         SetFriendlyName(const std::wstring& devicePath, const std::wstring& name);

    // ---- 快捷键配置（仅存储，供后续模块读取）-------------------

    const HotkeyDef& GetHotkeyToEnglish() const;
    const HotkeyDef& GetHotkeyToChinese() const;
    void             SetHotkeyToEnglish(const HotkeyDef& hk);
    void             SetHotkeyToChinese(const HotkeyDef& hk);

    // ---- 静态工具 -----------------------------------------------

    static std::wstring LangToStr(InputLang lang);
    static InputLang    StrToLang(const std::wstring& str);
    static std::wstring GetDefaultFilePath(); // exe 同目录的 keyboard_config.json

private:
    std::wstring                         m_filePath;
    std::map<std::wstring, InputLang>    m_settings;
    std::map<std::wstring, std::wstring> m_friendlyNames;
    HotkeyDef                            m_hotkeyToEnglish;
    HotkeyDef                            m_hotkeyToChinese;
};
