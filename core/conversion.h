#pragma once
#include "document.h"
namespace lpdf {
enum class InputFileKind { Pdf, Word, Excel, PowerPoint, Text, Image, Other };
inline bool IsOfficeKind(InputFileKind kind){return kind==InputFileKind::Word||kind==InputFileKind::Excel||kind==InputFileKind::PowerPoint;}
InputFileKind FileKind(const fs::path& path);
enum class TextEncoding { Auto, Utf8, Utf16LE, Utf16BE, GB18030 };
enum class OfficeBackend { Automatic, Word, LibreOffice };   // Word = Microsoft Office（Word / Excel / PowerPoint 按文件类型选择）
struct ConversionOptions {
    TextEncoding encoding{TextEncoding::Auto};
    OfficeBackend office{OfficeBackend::Automatic};
    bool imageA4{true};
    fs::path libreOffice;
};
struct ConversionResult { fs::path pdf; std::wstring backend; };
class Converter {
public:
    Converter() = default;
    ConversionResult Convert(const fs::path& input, const ConversionOptions& options, const Cancel& cancel,const ProgressSink& progress = {});
    static bool WordAvailable();
    // 对应的 Office 组件（Word / Excel / PowerPoint）是否已注册。
    static bool OfficeAvailable(InputFileKind kind);
    static fs::path FindLibreOffice();
    static std::wstring DecodeText(const std::vector<unsigned char>& data, TextEncoding encoding);
private:
    TempDirectory temporary_;
};
int WordWorker(const fs::path& source, const fs::path& output);
int OfficeWorker(InputFileKind kind, const fs::path& source, const fs::path& output);
// 子进程入口：--word-worker / --excel-worker / --powerpoint-worker <源> <输出>。flag 不是 worker 参数时返回 -1。
int OfficeWorkerMain(std::wstring_view flag, const fs::path& source, const fs::path& output);
const wchar_t* OfficeWorkerFlag(InputFileKind kind);
}

