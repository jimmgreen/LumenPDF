#pragma once
// 剪贴板读取：截图 / 复制的图片（PNG、DIBV5、DIB）与资源管理器中复制的文件（图片、PDF）。
#include "core/document.h"
#include <windows.h>
namespace lpdf {
struct ClipboardContent {
    fs::path image;                 // 图片文件：剪贴板位图写出的临时 PNG，或复制的图片文件本身
    bool temporary{};               // image 为临时文件，用完应删除
    std::vector<fs::path> pdfs;     // 复制的 PDF 文件
    int width{}, height{};          // 位图来源时的像素尺寸（文件来源为 0）
    std::wstring source;            // L"PNG" / L"DIB" / L"文件"
};
// 剪贴板上是否有可粘贴的图片或文件（不打开剪贴板，开销很小）。
bool ClipboardHasContent();
// 读取剪贴板：优先 PNG（保留透明），其次 DIBV5 / DIB，最后复制的文件（图片 / PDF）。
// 位图写成 tempDir 下的临时 PNG。没有可用内容返回 false 且 error 为空；出错返回 false 且 error 非空。
bool ReadClipboard(HWND owner,const fs::path& tempDir,ClipboardContent& out,std::wstring& error);
// 打包 DIB（BITMAPINFOHEADER / V4 / V5 + 调色板 + 像素）→ 非预乘 BGRA。32 位保留 alpha（全为 0 时视为不透明），
// 其它位深（含调色板、16 位、RLE）经 GDI 转换。数据不完整或尺寸异常时抛出异常。
Bitmap DibToBitmap(const void* dib,size_t size);
bool IsImageFile(const fs::path& file);
// Ctrl+V 的默认意图：无文档 / 主页 / 合并视图 → 新建 PDF；页面视图 → 插入为新页；阅读 / 批注视图 → 图片批注。
enum class PasteIntent { Auto, NewDocument, Annotation, NewPage };
PasteIntent ResolvePasteIntent(PasteIntent requested,bool loaded,bool home,int mode);
}
