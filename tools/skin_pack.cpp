// OnlyObsessionRemained 皮肤打包器（构建期工具，纯 C++，无第三方依赖）。
//
// 作用：把 skins/<名字>/ 目录下的清单与贴图编成 SkinBinaryData.h / .cpp，
// 让插件把皮肤直接带在二进制里。CMake 目标 skin_assets 会在皮肤目录任何文件变化时
// 重新调用本工具，因此"导入贴图 → 重新编译 → 皮肤更新"是一条自动链路。
//
// 用法：skin_pack <皮肤目录> <输出目录>
//
// 顺带做一次 JSON 语法校验（含行号），让清单里的手误在构建期就暴露，
// 而不是等到运行时界面回退成默认皮肤才发现。

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    // 会被打包的扩展名（其余文件忽略，方便你在皮肤目录里放素材源文件/参考图）
    const char* const kAllowedExt[] = { ".json", ".png", ".jpg", ".jpeg", ".glsl" };

    bool hasAllowedExtension (const fs::path& p)
    {
        std::string ext = p.extension().string();
        std::transform (ext.begin(), ext.end(), ext.begin(),
                        [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });

        for (auto* e : kAllowedExt)
            if (ext == e)
                return true;

        return false;
    }

    std::vector<unsigned char> readBinary (const fs::path& p)
    {
        std::ifstream in (p, std::ios::binary);
        return std::vector<unsigned char> ((std::istreambuf_iterator<char> (in)),
                                            std::istreambuf_iterator<char>());
    }

    std::string readText (const fs::path& p)
    {
        std::ifstream in (p, std::ios::binary);
        return std::string ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    }

    // ---------------------------------------------------------------
    // 极简 JSON 语法校验（只为给出"第几行、期望什么"这类可操作的报错）
    // ---------------------------------------------------------------

    struct JsonValidator
    {
        const std::string& src;
        size_t pos = 0;
        int    line = 1;
        std::string error;

        explicit JsonValidator (const std::string& s) : src (s) {}

        bool fail (const char* what)
        {
            if (error.empty())
                error = std::string ("第 ") + std::to_string (line) + " 行：" + what;

            return false;
        }

        void skipSpace()
        {
            while (pos < src.size())
            {
                const char c = src[pos];

                if (c == '\n')      { ++line; ++pos; }
                else if (std::isspace (static_cast<unsigned char> (c))) { ++pos; }
                else if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '/')
                {
                    while (pos < src.size() && src[pos] != '\n')
                        ++pos;
                }
                else
                    break;
            }
        }

        bool atEnd() { skipSpace(); return pos >= src.size(); }

        bool value()
        {
            skipSpace();

            if (atEnd())
                return fail ("内容意外结束，期望一个值");

            const char c = src[pos];

            if (c == '{')  return object();
            if (c == '[')  return array();
            if (c == '"')  return string();
            if (c == 't')  return literal ("true");
            if (c == 'f')  return literal ("false");
            if (c == 'n')  return literal ("null");

            return number();
        }

        bool literal (const char* word)
        {
            const size_t n = std::strlen (word);

            if (src.compare (pos, n, word) != 0)
                return fail ("无法识别的字面量");

            pos += n;
            return true;
        }

        bool number()
        {
            const size_t start = pos;

            while (pos < src.size() && (std::isdigit (static_cast<unsigned char> (src[pos]))
                                        || src[pos] == '-' || src[pos] == '+'
                                        || src[pos] == '.' || src[pos] == 'e' || src[pos] == 'E'))
                ++pos;

            if (pos == start)
                return fail ("期望数字、字符串、对象或数组");

            return true;
        }

        bool string()
        {
            ++pos;   // 起始引号

            while (pos < src.size())
            {
                const char c = src[pos];

                if (c == '\\') { pos += 2; continue; }
                if (c == '\n') { ++line; }
                if (c == '"')  { ++pos; return true; }

                ++pos;
            }

            return fail ("字符串没有闭合的引号");
        }

        bool object()
        {
            ++pos;   // '{'
            skipSpace();

            if (pos < src.size() && src[pos] == '}') { ++pos; return true; }

            while (true)
            {
                skipSpace();

                if (pos >= src.size() || src[pos] != '"')
                    return fail ("期望对象的键名（带引号）");

                if (! string())
                    return false;

                skipSpace();

                if (pos >= src.size() || src[pos] != ':')
                    return fail ("期望键名后的冒号");

                ++pos;

                if (! value())
                    return false;

                skipSpace();

                if (pos < src.size() && src[pos] == ',') { ++pos; continue; }
                if (pos < src.size() && src[pos] == '}') { ++pos; return true; }

                return fail ("期望逗号或对象结束的花括号");
            }
        }

        bool array()
        {
            ++pos;   // '['
            skipSpace();

            if (pos < src.size() && src[pos] == ']') { ++pos; return true; }

            while (true)
            {
                if (! value())
                    return false;

                skipSpace();

                if (pos < src.size() && src[pos] == ',') { ++pos; continue; }
                if (pos < src.size() && src[pos] == ']') { ++pos; return true; }

                return fail ("期望逗号或数组结束的方括号");
            }
        }
    };

    bool validateJson (const std::string& text, const fs::path& file)
    {
        JsonValidator v (text);

        if (! v.value() || ! v.atEnd())
        {
            std::printf ("错误：%s 不是合法的 JSON —— %s\n",
                         file.filename().string().c_str(),
                         v.error.empty() ? "文件末尾有多余内容" : v.error.c_str());
            return false;
        }

        return true;
    }

    // ---------------------------------------------------------------
    // 生成代码
    // ---------------------------------------------------------------

    std::string identifierFor (int index)
    {
        return "file_" + std::to_string (index);
    }

    std::string toCppPath (const fs::path& relative)
    {
        std::string s = relative.generic_string();

        std::string out;
        out.reserve (s.size());

        for (char c : s)
            out += c;

        return out;
    }
}

int main (int argc, char* argv[])
{
    if (argc < 3)
    {
        std::printf ("用法：skin_pack <皮肤目录> <输出目录>\n");
        return 2;
    }

    const fs::path skinDir = argv[1];
    const fs::path outDir  = argv[2];

    if (! fs::is_directory (skinDir))
    {
        std::printf ("错误：皮肤目录不存在：%s\n", skinDir.string().c_str());
        return 1;
    }

    const fs::path manifest = skinDir / "skin.json";

    if (! fs::is_regular_file (manifest))
    {
        std::printf ("错误：皮肤目录里缺少 skin.json：%s\n", skinDir.string().c_str());
        return 1;
    }

    if (! validateJson (readText (manifest), manifest))
        return 1;

    // 收集文件（排序保证生成结果稳定，避免无谓的重编译）
    std::vector<fs::path> files;

    for (const auto& entry : fs::recursive_directory_iterator (skinDir))
        if (entry.is_regular_file() && hasAllowedExtension (entry.path()))
            files.push_back (fs::relative (entry.path(), skinDir));

    std::sort (files.begin(), files.end());

    if (files.empty())
    {
        std::printf ("错误：皮肤目录里没有可打包的文件\n");
        return 1;
    }

    std::error_code ec;
    fs::create_directories (outDir, ec);

    // ---- SkinBinaryData.h ----
    {
        std::ofstream out (outDir / "SkinBinaryData.h");

        out << "// 自动生成，请勿手改（由 tools/skin_pack.cpp 生成）。\n"
               "// 皮肤源目录：" << skinDir.generic_string() << "\n\n"
               "#pragma once\n\n"
               "namespace skin_binary\n"
               "{\n"
               "    struct File\n"
               "    {\n"
               "        const char* path;   // 相对皮肤目录，正斜杠分隔\n"
               "        const void* data;\n"
               "        int         size;\n"
               "    };\n\n"
               "    extern const File files[];\n"
               "    extern const int  numFiles;\n"
               "}\n";
    }

    // ---- SkinBinaryData.cpp ----
    {
        std::ofstream out (outDir / "SkinBinaryData.cpp");

        out << "// 自动生成，请勿手改（由 tools/skin_pack.cpp 生成）。\n\n"
               "#include \"SkinBinaryData.h\"\n\n"
               "namespace skin_binary\n{\n";

        for (size_t i = 0; i < files.size(); ++i)
        {
            const auto data = readBinary (skinDir / files[i]);

            out << "    static const unsigned char " << identifierFor (static_cast<int> (i))
                << "[] = {";

            if (data.empty())
            {
                out << " 0";
            }
            else
            {
                for (size_t b = 0; b < data.size(); ++b)
                {
                    if (b % 16 == 0)
                        out << "\n        ";

                    out << static_cast<int> (data[b]);

                    if (b + 1 < data.size())
                        out << ", ";
                }
            }

            out << "\n    };\n\n";
        }

        out << "    const File files[] =\n    {\n";

        for (size_t i = 0; i < files.size(); ++i)
        {
            const auto size = fs::file_size (skinDir / files[i]);

            out << "        { \"" << toCppPath (files[i]) << "\", "
                << identifierFor (static_cast<int> (i)) << ", "
                << static_cast<unsigned long long> (size) << " },\n";
        }

        out << "    };\n\n"
               "    const int numFiles = " << files.size() << ";\n"
               "}\n";
    }

    std::printf ("skin_pack：%s -> %s（%d 个文件）\n",
                 skinDir.generic_string().c_str(), outDir.generic_string().c_str(),
                 static_cast<int> (files.size()));

    for (const auto& f : files)
        std::printf ("    %-40s %8llu 字节\n", f.generic_string().c_str(),
                     static_cast<unsigned long long> (fs::file_size (skinDir / f)));

    return 0;
}