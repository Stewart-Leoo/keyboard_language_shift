// InputConfig.cpp
// 输入语言配置模块 -- 实现

#include "InputConfig.h"
#include <algorithm>
#include <cwctype>

// ================================================================
// 构造：快捷键不在代码中硬编码，完全由 JSON 配置文件决定
// 若文件不存在或无 hotkeys 节，则 HotkeyDef 保持默认初始化状态
// （ctrl/shift/alt=false，key=""），由用户在 JSON 中填写
// ================================================================
InputConfig::InputConfig(const std::wstring& filePath)
    : m_filePath(filePath)
{
}

// ================================================================
// GetDefaultFilePath：与 exe 同目录的 keyboard_config.json
// ================================================================
std::wstring InputConfig::GetDefaultFilePath()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath);
    size_t pos = path.rfind(L'\\');
    path = (pos != std::wstring::npos) ? path.substr(0, pos + 1) : L".\\";
    return path + L"keyboard_config.json";
}

// ================================================================
// JSON 工具
// ================================================================

// 转义：\ -> \\，" -> \"，控制字符处理
static std::wstring JsonEscape(const std::wstring& s)
{
    std::wstring r;
    for (wchar_t c : s)
    {
        if      (c == L'\\') r += L"\\\\";
        else if (c == L'"' ) r += L"\\\"";
        else if (c == L'\n') r += L"\\n";
        else if (c == L'\r') r += L"\\r";
        else if (c == L'\t') r += L"\\t";
        else                 r += c;
    }
    return r;
}

// 反转义
static std::wstring JsonUnescape(const std::wstring& s)
{
    std::wstring r;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == L'\\' && i + 1 < s.size())
        {
            ++i;
            switch (s[i])
            {
            case L'\\': r += L'\\'; break;
            case L'"' : r += L'"';  break;
            case L'n' : r += L'\n'; break;
            case L'r' : r += L'\r'; break;
            case L't' : r += L'\t'; break;
            default   : r += L'\\'; r += s[i]; break;
            }
        }
        else
        {
            r += s[i];
        }
    }
    return r;
}

// ================================================================
// 最小 JSON 解析器（仅支持本模块所需结构）
// ================================================================
struct JsonParser
{
    const std::wstring& text;
    size_t pos = 0;

    void skip()
    {
        while (pos < text.size() && iswspace(text[pos])) ++pos;
    }

    wchar_t cur() const
    {
        return (pos < text.size()) ? text[pos] : L'\0';
    }

    bool expect(wchar_t c)
    {
        skip();
        if (cur() == c) { ++pos; return true; }
        return false;
    }

    // 解析 JSON 字符串（pos 指向起始 '"'）
    bool parseString(std::wstring& out)
    {
        skip();
        if (cur() != L'"') return false;
        ++pos;
        std::wstring raw;
        while (pos < text.size() && text[pos] != L'"')
        {
            if (text[pos] == L'\\' && pos + 1 < text.size())
            {
                raw += text[pos];
                raw += text[pos + 1];
                pos += 2;
            }
            else
            {
                raw += text[pos++];
            }
        }
        if (pos < text.size()) ++pos; // 跳过结尾 "
        out = JsonUnescape(raw);
        return true;
    }

    // 解析布尔值 true / false
    bool parseBool(bool& out)
    {
        skip();
        if (pos + 4 <= text.size() && text.substr(pos, 4) == L"true")
        {
            out = true;  pos += 4; return true;
        }
        if (pos + 5 <= text.size() && text.substr(pos, 5) == L"false")
        {
            out = false; pos += 5; return true;
        }
        return false;
    }

    // 跳过整个字符串（pos 在 '"' 上）
    void skipString()
    {
        if (cur() != L'"') return;
        ++pos;
        while (pos < text.size() && text[pos] != L'"')
        {
            if (text[pos] == L'\\') ++pos;
            ++pos;
        }
        if (pos < text.size()) ++pos;
    }

    // 跳过任意值
    void skipValue()
    {
        skip();
        if (cur() == L'"')  { skipString(); return; }
        if (cur() == L'{')  { skipObject(); return; }
        if (cur() == L'[')
        {
            ++pos;
            while (pos < text.size() && cur() != L']')
            {
                skipValue(); skip();
                if (cur() == L',') ++pos;
            }
            ++pos;
            return;
        }
        // number / true / false / null
        while (pos < text.size() &&
               cur() != L',' && cur() != L'}' && cur() != L']' && !iswspace(cur()))
            ++pos;
    }

    // 跳过整个对象 {}（处理嵌套和字符串中的花括号）
    void skipObject()
    {
        expect(L'{');
        int depth = 1;
        while (pos < text.size() && depth > 0)
        {
            if      (text[pos] == L'{') { ++depth; ++pos; }
            else if (text[pos] == L'}') { --depth; ++pos; }
            else if (text[pos] == L'"') { skipString(); }
            else                        { ++pos; }
        }
    }

    // 遍历对象：对每个 key-value 调用 callback(key)
    // callback 返回 true  = 已自行消耗 value
    // callback 返回 false = 由 parseObject 跳过 value
    template<typename Fn>
    void parseObject(Fn callback)
    {
        expect(L'{');
        while (pos < text.size())
        {
            skip();
            if (cur() == L'}') { ++pos; break; }
            if (cur() == L',') { ++pos; continue; }

            std::wstring key;
            if (!parseString(key)) break;
            if (!expect(L':'))    break;
            skip();

            bool consumed = callback(key);
            if (!consumed) skipValue();
        }
    }
};

// ================================================================
// Load：从 JSON 文件读取配置
// ================================================================
bool InputConfig::Load()
{
    HANDLE hFile = CreateFileW(m_filePath.c_str(),
        GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0)
    {
        CloseHandle(hFile);
        return false;
    }

    std::string raw(fileSize, '\0');
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, &raw[0], fileSize, &bytesRead, nullptr))
    {
        CloseHandle(hFile);
        return false;
    }
    CloseHandle(hFile);
    raw.resize(bytesRead);

    // 跳过 UTF-8 BOM（EF BB BF）
    size_t start = 0;
    if (raw.size() >= 3 &&
        (unsigned char)raw[0] == 0xEF &&
        (unsigned char)raw[1] == 0xBB &&
        (unsigned char)raw[2] == 0xBF)
        start = 3;

    // UTF-8 -> 宽字符
    int wlen = MultiByteToWideChar(CP_UTF8, 0,
        raw.c_str() + start, (int)(raw.size() - start), nullptr, 0);
    std::wstring content(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0,
        raw.c_str() + start, (int)(raw.size() - start), &content[0], wlen);

    m_settings.clear();
    m_friendlyNames.clear();

    JsonParser p{ content };

    p.parseObject([&](const std::wstring& topKey) -> bool
    {
        // ---- keyboards 节 ----
        if (topKey == L"keyboards")
        {
            p.parseObject([&](const std::wstring& devicePath) -> bool
            {
                std::wstring langStr, nameStr;
                p.parseObject([&](const std::wstring& field) -> bool
                {
                    if (field == L"lang")
                    {
                        std::wstring v; p.parseString(v); langStr = v; return true;
                    }
                    if (field == L"friendly_name")
                    {
                        std::wstring v; p.parseString(v); nameStr = v; return true;
                    }
                    return false;
                });
                if (!langStr.empty()) m_settings[devicePath]     = StrToLang(langStr);
                if (!nameStr.empty()) m_friendlyNames[devicePath] = nameStr;
                return true;
            });
            return true;
        }

        // ---- hotkeys 节 ----
        if (topKey == L"hotkeys")
        {
            // 解析单个 HotkeyDef 对象
            auto parseHotkey = [&](HotkeyDef& out)
            {
                p.parseObject([&](const std::wstring& field) -> bool
                {
                    if (field == L"ctrl")  { p.parseBool(out.ctrl);  return true; }
                    if (field == L"shift") { p.parseBool(out.shift); return true; }
                    if (field == L"alt")   { p.parseBool(out.alt);   return true; }
                    if (field == L"key")   { p.parseString(out.key); return true; }
                    return false;
                });
            };

            p.parseObject([&](const std::wstring& action) -> bool
            {
                if (action == L"switch_to_english") { parseHotkey(m_hotkeyToEnglish); return true; }
                if (action == L"switch_to_chinese") { parseHotkey(m_hotkeyToChinese); return true; }
                return false;
            });
            return true;
        }

        return false; // version 等字段跳过
    });

    return true;
}

// ================================================================
// Save：写为格式化 JSON 文件（UTF-8 BOM）
// ================================================================
bool InputConfig::Save() const
{
    auto boolLit = [](bool b) -> const wchar_t* { return b ? L"true" : L"false"; };

    auto hotkeyBlock = [&](const HotkeyDef& hk, const std::wstring& indent) -> std::wstring
    {
        std::wstring s;
        s += L"{\n";
        s += indent + L"    \"ctrl\": "  + boolLit(hk.ctrl)        + L",\n";
        s += indent + L"    \"shift\": " + boolLit(hk.shift)       + L",\n";
        s += indent + L"    \"alt\": "   + boolLit(hk.alt)         + L",\n";
        s += indent + L"    \"key\": \"" + JsonEscape(hk.key)      + L"\"\n";
        s += indent + L"}";
        return s;
    };

    std::wstring json;
    json += L"{\n";
    json += L"    \"version\": 1,\n";

    // hotkeys 节
    json += L"    \"hotkeys\": {\n";
    json += L"        \"switch_to_english\": " + hotkeyBlock(m_hotkeyToEnglish, L"        ") + L",\n";
    json += L"        \"switch_to_chinese\": " + hotkeyBlock(m_hotkeyToChinese, L"        ") + L"\n";
    json += L"    },\n";

    // keyboards 节
    json += L"    \"keyboards\": {\n";
    size_t count = 0;
    for (const auto& kv : m_settings)
    {
        json += L"        \"" + JsonEscape(kv.first) + L"\": {\n";

        auto nameIt = m_friendlyNames.find(kv.first);
        if (nameIt != m_friendlyNames.end() && !nameIt->second.empty())
            json += L"            \"friendly_name\": \"" + JsonEscape(nameIt->second) + L"\",\n";

        json += L"            \"lang\": \"" + LangToStr(kv.second) + L"\"\n";
        json += L"        }";
        ++count;
        if (count < m_settings.size()) json += L",";
        json += L"\n";
    }
    json += L"    }\n";
    json += L"}\n";

    // 宽字符 -> UTF-8
    int len = WideCharToMultiByte(CP_UTF8, 0,
        json.c_str(), (int)json.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0,
        json.c_str(), (int)json.size(), &utf8[0], len, nullptr, nullptr);

    // 写文件（带 BOM）
    HANDLE hFile = CreateFileW(m_filePath.c_str(),
        GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    const BYTE bom[] = { 0xEF, 0xBB, 0xBF };
    DWORD written = 0;
    WriteFile(hFile, bom,            sizeof(bom),         &written, nullptr);
    WriteFile(hFile, utf8.c_str(), (DWORD)utf8.size(),    &written, nullptr);
    CloseHandle(hFile);
    return true;
}

// ================================================================
// 键盘语言 Getter / Setter / Toggle
// ================================================================
InputLang InputConfig::GetLang(const std::wstring& devicePath) const
{
    auto it = m_settings.find(devicePath);
    return (it != m_settings.end()) ? it->second : InputLang::English;
}

void InputConfig::SetLang(const std::wstring& devicePath, InputLang lang)
{
    m_settings[devicePath] = lang;
}

void InputConfig::ToggleLang(const std::wstring& devicePath)
{
    m_settings[devicePath] = (GetLang(devicePath) == InputLang::English)
        ? InputLang::Chinese : InputLang::English;
}

void InputConfig::SetFriendlyName(const std::wstring& devicePath, const std::wstring& name)
{
    m_friendlyNames[devicePath] = name;
}

// ================================================================
// 快捷键 Getter / Setter
// ================================================================
const HotkeyDef& InputConfig::GetHotkeyToEnglish() const
{
    return m_hotkeyToEnglish;
}

const HotkeyDef& InputConfig::GetHotkeyToChinese() const
{
    return m_hotkeyToChinese;
}

void InputConfig::SetHotkeyToEnglish(const HotkeyDef& hk)
{
    m_hotkeyToEnglish = hk;
}

void InputConfig::SetHotkeyToChinese(const HotkeyDef& hk)
{
    m_hotkeyToChinese = hk;
}

// ================================================================
// LangToStr / StrToLang
// ================================================================
std::wstring InputConfig::LangToStr(InputLang lang)
{
    return (lang == InputLang::Chinese) ? L"CN" : L"EN";
}

InputLang InputConfig::StrToLang(const std::wstring& str)
{
    std::wstring upper = str;
    for (auto& c : upper) c = (wchar_t)towupper(c);
    if (upper == L"CN" || upper == L"ZH" || upper == L"CHINESE")
        return InputLang::Chinese;
    return InputLang::English;
}
