#ifndef COMMON_UTILS_H
#define COMMON_UTILS_H

#include <cstdint>
#include <string>

// 跨模块共享的小工具，避免同一逻辑在多个源文件里各写一份。
namespace util
{
// 把 V4L2 的四字符码(fourcc)转成可读字符串，非可打印字符用 '?' 占位。
inline std::string FourccToString(uint32_t fourcc)
{
    char s[5] = {static_cast<char>(fourcc & 0xFF), static_cast<char>((fourcc >> 8) & 0xFF),
                 static_cast<char>((fourcc >> 16) & 0xFF), static_cast<char>((fourcc >> 24) & 0xFF),
                 '\0'};
    for (int i = 0; i < 4; ++i)
    {
        if (s[i] < 32 || s[i] > 126)
        {
            s[i] = '?';
        }
    }
    return std::string(s);
}

// JSON 字符串转义：处理引号/反斜杠及常见控制符，其它控制字符替换为空格以保证 JSON 合法。
inline std::string JsonEscape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 16);
    for (const char c : in)
    {
        switch (c)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    out += ' ';
                }
                else
                {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

inline std::string JsonEscape(const char* in) { return JsonEscape(std::string(in ? in : "")); }
}  // namespace util

#endif  // COMMON_UTILS_H
