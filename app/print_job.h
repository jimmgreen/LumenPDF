#pragma once
// 打印：界面线程选择打印机并开始作业，工作线程逐页渲染并送往打印机。
#include "core/document.h"
#include <windows.h>
namespace lpdf {
struct PrintSettings {
    std::vector<int> pages;      // 0 起页码，按打印顺序
    bool fitToPaper{true};       // true：按可打印区域缩放（放大或缩小）；false：仅缩小超出的页面
    bool autoRotate{true};       // 页面与纸张方向不一致时旋转 90° 以占满纸张
    bool annotations{true}, grayscale{}, reverse{}, booklet{};
    int parity{};               // 0=all, 1=odd document pages, 2=even document pages
    int pagesPerSheet{1};        // 1, 2, 4, 6, 9, 16
    int bookletSide{};           // 0=both sides (duplex printer), 1=fronts, 2=backs
    int maxDpi{300};             // 渲染分辨率上限，打印机更高时按上限渲染后由 GDI 放大
    int orientation{};           // 纸张方向：0=自动（按版面），1=纵向，2=横向
    int rangeMode{};             // 预览对话框中的页码选择：0=全部，1=当前页，2=自定义
    std::wstring range;          // 自定义页码，例如 1-3,5
};
using PrintSheet = std::vector<int>; // -1 is a blank booklet slot
std::vector<PrintSheet> PlanPrint(const PrintSettings& settings);
std::pair<int,int> PrintGrid(int pagesPerSheet);
// 界面线程：显示系统打印对话框，返回可用的打印机 DC（调用方负责 DeleteDC）。
// 取消或出错时返回 nullptr；出错信息写入 error。
// settings.pages 非空时预置为对话框的页码范围；landscape 预置纸张方向（用户仍可在对话框中更改）。
HDC ChoosePrinter(HWND owner,int pageCount,int currentPage,PrintSettings& settings,std::wstring& error,bool landscape=false);
// 界面线程：开始打印作业。output 非空时输出到文件（用于“打印到 PDF”的自动化测试）。
bool BeginPrintJob(HDC dc,std::wstring_view title,const fs::path& output={});
// 工作线程：渲染并输出全部页面，然后结束作业。取消或失败时中止作业（打印队列中不会留下半份文档）。
void PrintPages(Document& document,HDC dc,const PrintSettings& settings,const Cancel& cancel,
                const std::function<void(int done,int total)>& progress={});
// 纯计算：给定页面尺寸（点）与可打印区域（设备像素、DPI），返回目标矩形（设备像素）与是否旋转。
struct PrintPlacement { int x{},y{},width{},height{}; bool rotate{}; float scale{}; };
PrintPlacement PlacePage(float pageWidth,float pageHeight,int areaWidth,int areaHeight,int dpiX,int dpiY,bool fit,bool autoRotate);

// 一面纸上各页的位置（设备像素，相对可打印区域左上角）。打印与预览共用，保证预览与实际输出一致。
struct SlotPlacement { int slot{}, page{}; PrintPlacement place; };
std::vector<SlotPlacement> LayoutSheet(const PrintSettings& settings,const PrintSheet& sheet,const std::vector<PageInfo>& pages,
                                       int areaWidth,int areaHeight,int dpiX,int dpiY);
// 纸张（点，纵向）与不可打印边距。查询失败时为 A4、四边 0.25 英寸。
struct PaperInfo {
    float width{595.28f}, height{841.89f};
    float left{18}, top{18}, right{18}, bottom{18};
    std::wstring printer;        // 打印机名；空 = 未找到打印机
    bool fromPrinter{};
};
std::wstring DefaultPrinterName();
// 查询打印机（空 = 默认打印机）的默认纸张；可能较慢（网络打印机），应在工作线程调用。
PaperInfo QueryPaper(const std::wstring& printer={});
PaperInfo Oriented(const PaperInfo& portrait,bool landscape);
std::wstring PaperName(const PaperInfo& paper);   // “A4”“Letter”或“210 × 297 毫米”
// 自动方向：按首个要打印页面的宽高与每面网格，选择能让版面最大的纸张方向。
bool AutoLandscape(const PrintSettings& settings,const std::vector<PageInfo>& pages);
bool ResolveLandscape(const PrintSettings& settings,const std::vector<PageInfo>& pages);
// 预览：按纸张长边 longSide 像素渲染一面纸（白纸、虚线标出可打印区域、页面细框）。
Bitmap RenderSheetPreview(Document& document,const PrintSettings& settings,const PrintSheet& sheet,const PaperInfo& paper,int longSide);
// 以指定方向创建打印机 DC（自动化“打印到 PDF”用）。失败返回 nullptr。
HDC CreatePrinterDC(const std::wstring& printer,bool landscape);
// 页码列表压缩为区间（1 起），用于预置系统打印对话框。
std::vector<std::pair<int,int>> PageRuns(const std::vector<int>& pages);
}