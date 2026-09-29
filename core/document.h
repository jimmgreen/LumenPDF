#pragma once
#include "platform.h"
#include "progress.h"
#include <memory>
#include <optional>
#include <cmath>
namespace lpdf {
struct Point { float x{}, y{}; };
struct Quad { Point ul, ur, ll, lr; };
enum class SnapUnit { Word, Paragraph };
struct Rect {
    float x{}, y{}, w{}, h{};
    bool Contains(Point p) const { return p.x>=x && p.x<=x+w && p.y>=y && p.y<=y+h; }
};
struct PageInfo { float width{}, height{}, originX{}, originY{}; };
struct Bitmap { int width{}, height{}, stride{}; std::vector<unsigned char> bgra; };
// 单个批注的外观位图（预乘 BGRA、透明背景），bounds 为其在页面坐标中的覆盖范围。
struct Sprite { Rect bounds; Bitmap pixels; };
// y：目标页内纵坐标（页面坐标，自上而下）；NaN 表示页顶。
struct OutlineItem { std::wstring title; int page{}; int depth{}; float y{NAN}; };
// 新工具只能追加在末尾：界面按下标保存各工具的样式与提示。
enum class Tool { Select, Text, Note, Highlight, Rectangle, Arrow, Ink, Image, Underline, StrikeOut, Ellipse, Line, Stamp };
inline constexpr size_t ToolCount = 13;
inline bool IsLineTool(Tool t) { return t==Tool::Arrow||t==Tool::Line; }
inline bool IsMarkupTool(Tool t) { return t==Tool::Highlight||t==Tool::Underline||t==Tool::StrikeOut; }
inline bool IsBoxShape(Tool t) { return t==Tool::Rectangle||t==Tool::Ellipse; }
inline bool IsStrokedTool(Tool t) { return IsBoxShape(t)||IsLineTool(t)||t==Tool::Ink; }
struct AnnotationStyle {
    uint32_t color{0xd33445}, fillColor{0xffe3e5};
    float lineWidth{1.5f};
    bool filled{}, dashed{};
    // PDF line-ending values: 0=None, 4=OpenArrow, 5=ClosedArrow.
    int startEnding{}, endEnding{4};
    bool operator==(const AnnotationStyle&) const = default;
};
inline AnnotationStyle DefaultAnnotationStyle(Tool tool) {
    AnnotationStyle s;
    if(tool==Tool::Highlight||tool==Tool::Note)s.color=0xffd54a;
    if(tool==Tool::Ink)s.color=0x2864dc;
    if(tool==Tool::Text)s.color=0x1f1f1f;
    if(tool==Tool::Underline)s.color=0x2864dc;
    if(tool==Tool::Line)s.endEnding=0;
    return s;
}
struct TextFormat {
    std::wstring family{L"Microsoft YaHei"};
    uint32_t color{0x1f1f1f};
    bool bold{}, italic{}, underline{};
    int alignment{}; // 0=left, 1=center, 2=right
    bool operator==(const TextFormat&) const = default;
};
enum class TextSizing { Auto, Width, Fixed };
struct Annotation {
    int id{}; Tool type{}; Rect bounds; std::wstring text;
    float opacity{1}; float fontSize{12}; int rotation{}; bool fixedTextWidth{};
    AnnotationStyle style;
    std::vector<Point> points;
    std::vector<int> strokes;
    std::vector<Quad> quads;
    bool geometryEdited{}, areaHighlight{}, readOnly{};
    TextFormat textFormat;
    bool fixedTextBox{};
    TextSizing textSizing{TextSizing::Auto};
    std::wstring author;
    int64_t modified{};
    bool lumenText{}; // Uniform, unrotated Lumen text suitable for the UI text layer.
};
// 朗读：按阅读顺序切出的一句话，boxes 为逐行包围框（页面坐标，与文字选择一致）。
struct ReadingSentence { std::wstring text; std::vector<Rect> boxes; };
struct SearchHit { int page{}; Rect bounds; std::wstring context; int annotation{-1}; };
// 真正的涂黑（Redaction）：永久删除标记区域下的文字、被覆盖的图片像素与图形，并画上黑块。
struct RedactionMark { int page{}; Rect bounds; };
struct RedactionOptions {
    bool removeWholeImages{};    // true：与标记相交的图片整张删除；false：只把被覆盖的像素涂黑
    bool removeLineArt{true};    // 删除被标记完全覆盖的线条 / 形状
    bool clearMetadata{true};    // 清除标题、作者、主题、关键词、创建程序与 XMP 元数据
};
struct RedactionResult { int pages{}, marks{}, annotationsRemoved{}, fieldsCleared{}; };
struct SearchOptions { bool matchCase{}, wholeWord{}, annotations{}, bookmarks{}; };
// 页面链接：内部链接给出目标页（及可选的目标纵坐标），外部链接给出 URI。
struct Link {
    Rect bounds;
    int page{-1};            // 内部目标页；外部链接为 -1
    float targetY{NAN};      // 目标页内纵坐标（页面坐标），未指定为 NaN
    std::wstring uri;        // 外部链接地址
    bool External() const { return page<0; }
};
// AcroForm 表单字段（每个控件一项；同名单选按钮组的各个按钮分别列出）。id 为控件对象号，在文档内稳定。
enum class FieldType { Text, CheckBox, Radio, ComboBox, ListBox, Button, Signature, Unknown };
struct FormField {
    int page{}, id{};
    FieldType type{FieldType::Unknown};
    std::wstring name, label, value;   // label：提示文字（TU），没有时等于名称
    Rect bounds;
    bool readOnly{}, required{}, multiline{}, password{}, editable{}, multiSelect{}, checked{}, signedField{};
    int maxLength{};                   // 0 = 不限
    std::vector<std::wstring> options, exports;   // 下拉 / 列表：显示文字与导出值（一一对应）
    std::wstring onState;              // 复选框 / 单选按钮的“选中”状态名
    bool Fillable() const { return !readOnly && (type==FieldType::Text||type==FieldType::CheckBox||type==FieldType::Radio||type==FieldType::ComboBox||type==FieldType::ListBox); }
};
struct DocumentMetadata {
    std::wstring title,author,subject,keywords,creator,producer,created,modified,format;
};
struct DocumentInfo {
    std::vector<PageInfo> pages;
    std::vector<std::wstring> labels;
    std::vector<OutlineItem> outline;
    int outlineSkipped{};    // 指向外部链接等、无法按页编辑的书签数量（编辑目录时会被移除）
    bool canUndo{}, canRedo{}, dirty{};
    bool encrypted{};        // 源文件带有加密（保存时默认保持原加密）
    bool hasForm{};          // 含 AcroForm 字段（可填写表单）
};
// 导出副本的安全设置。口令都为空且不限制权限表示“去除加密”；只限制权限时用随机权限口令加密。
struct Security {
    std::wstring userPassword;   // 打开口令；为空则无需口令即可打开
    std::wstring ownerPassword;  // 权限口令；为空时自动生成随机口令，使权限限制有效
    bool allowPrint{true}, allowCopy{true}, allowModify{true}, allowAnnotate{true};
};
enum class CompressLevel { Lossless, Balanced, Smallest };
struct CompressResult { uint64_t before{}, after{}; };
enum class SplitMode { EveryPages, TopBookmarks, MaxSize };
// 页眉、页脚（含页码）与文字水印。文字中的 {page} / {total} 按页替换。
struct PageDecoration {
    std::wstring header, footer;
    int headerAlign{1}, footerAlign{1};   // 0=左 1=中 2=右
    float fontSize{10}, margin{28};
    uint32_t color{0x333333};
    int startNumber{1};
    std::wstring watermark;
    float watermarkSize{64}, watermarkOpacity{.15f}, watermarkAngle{45};
    uint32_t watermarkColor{0x9a9a9a};
    bool watermarkBehind{};
    std::vector<int> pages;               // 空 = 全部页面
};
struct MergeInput { fs::path path; std::wstring range; std::wstring password; };
std::vector<int> ParsePageRange(std::wstring_view text, int count);
class PasswordRequired : public std::runtime_error {
public: PasswordRequired() : std::runtime_error("Password required or incorrect") {}
};
// Owned and called exclusively by one worker thread.
class Document {
public:
    Document();
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    void Open(const fs::path& path, std::wstring_view password = {});
    void Reload(const fs::path& path); // Reopen with the current document password; failure leaves current state intact.
    void New();
    // 关闭当前文档并释放全部缓存；之后需要重新 Open/New。
    void Close();
    bool IsOpen() const;
    DocumentInfo Info();
    DocumentMetadata Metadata();
    void SetMetadata(const DocumentMetadata& metadata);
    void SetPageLabels(int first,int style,std::wstring_view prefix,int start);
    // Tiles replay a cached per-page display list; the cache is dropped on every edit,
    // undo/redo and open, so results always match the current document.
    Bitmap Render(int page, float scale, std::optional<Rect> clip = {},int hiddenAnnotation=-1,bool hideLumenText=false,bool hideAnnotations=false);
    // 只绘制一个批注的当前外观，供拖动时在界面层实时跟随；不修改文档。
    Sprite RenderAnnotation(int page, int id, float scale);
    std::vector<Annotation> Annotations(int page);
    // fromStart / toEnd：跨页选择时该端取本页文字的开头 / 结尾（中间页两端都取），此时不做“空白处不吸附”检查。
    std::vector<Quad> HighlightQuads(int page, Point start, Point end, bool fromStart = false, bool toEnd = false);
    std::wstring Text(int page, std::optional<Rect> selection = {});
    // 按阅读顺序取两点之间的文字（与 HighlightQuads 使用相同的选择规则）。
    std::wstring SelectionText(int page, Point start, Point end, bool fromStart = false, bool toEnd = false);
    // 朗读用：页面正文（不含批注）按句切分；中文按 。！？；… 断句，英文按 . ! ? ; 后跟空白断句，段落结束也断句；
    // 过长的句子在逗号 / 空格处再切（约 240 字）。行尾连字符合并，英文跨行补空格。
    std::vector<ReadingSentence> ReadingSentences(int page);
    // 把跨页端点换算为本页首 / 末字符上的点（供按起止点创建高亮批注）；本页没有文字时返回 false。
    bool ResolveSelection(int page, Point& start, Point& end, bool fromStart, bool toEnd);
    // 双击选词 / 三击选段：返回 p 所在单词（中文用系统 ICU 分词，不可用时取连续汉字）或文本块的起止点。
    // p 不在文字上（空白、行间）时返回空。
    std::optional<std::pair<Point, Point>> SnapSelection(int page, Point p, SnapUnit unit);
    std::vector<Link> Links(int page);
    // AcroForm：列出一页的表单控件；填写文本 / 选项、切换复选框 / 单选按钮、重置整个表单。
    // 每次填写是一次可撤销操作并立即重建该控件外观。只读字段、超出最大长度、不在选项中的值会被拒绝且不改动文档。
    // 本构建未启用 JavaScript：字段的计算 / 格式化 / 校验脚本不会运行。
    std::vector<FormField> FormFields(int page);
    void SetFieldValue(int page,int id,std::wstring_view value);
    void ToggleField(int page,int id);
    void ResetForm();
    // 下一个可填写的字段（文本 / 下拉 / 列表），按页内控件顺序，跨页，末尾回到开头；没有时返回空。
    std::optional<FormField> NextFillableField(int page,int id,bool textOnly=false);
    std::vector<SearchHit> Search(std::wstring_view query, const Cancel& cancel = {});
    std::vector<SearchHit> SearchPage(int page,std::wstring_view query,const SearchOptions& options={},const Cancel& cancel={});
    void AddAnnotation(int page, Tool type, Rect rect, std::wstring_view text = {},
                       const fs::path& image = {}, const std::vector<Point>& ink = {},float fontSize=12,float opacity=1,
                       std::optional<AnnotationStyle> style = {}, bool textHighlight=false,
                       std::optional<TextFormat> textFormat = {}, bool fixedTextBox=false, TextSizing textSizing=TextSizing::Auto);
    void AddTextAnnotation(int page,const Annotation& value);
    // 多笔画手绘批注（不平滑，保持原样），一次可撤销的操作；用于 ✓ / ✗ 等标记。
    void AddInkStrokes(int page,const std::vector<std::vector<Point>>& strokes,const AnnotationStyle& style,float opacity=1);
    void UpdateAnnotation(int page, const Annotation& value);
    void DeleteAnnotation(int page, int id);
    void RotatePage(int page,int degrees=90);
    void RotatePages(const std::vector<int>& pages,int degrees);
    void DuplicatePages(const std::vector<int>& pages);
    void CropPages(const std::vector<int>& pages,float left,float top,float right,float bottom);
    void ReplacePages(const std::vector<int>& pages,const fs::path& source,std::wstring_view password={});
    void DeletePages(const std::vector<int>& pages);
    void Reorder(const std::vector<int>& pages);
    void InsertBlank(int after);
    void InsertPdf(int after, const fs::path& source, std::wstring_view password = {});
    void Undo();
    void Redo();
    void Save(const fs::path& destination,const Cancel& cancel = {},const ProgressSink& progress = {});
    void Snapshot(const fs::path& destination);
    void MarkDirty();
    void Extract(const std::vector<int>& pages, const fs::path& destination);
    // 另存一份加密（AES-256）或去除加密的副本；写入并校验成功后才替换目标，不改变当前文档的未保存状态。
    void ExportSecured(const fs::path& destination, const Security& security, const Cancel& cancel = {});
    // 逐页导出 PNG：文件名为 <stem>-<页码>.png；每页先写临时文件再原子替换。返回写出的文件。
    std::vector<fs::path> ExportImages(const std::vector<int>& pages, const fs::path& folder, std::wstring_view stem,
                                       float dpi, const Cancel& cancel = {}, const std::function<void(int,int)>& progress = {});
    // 导出全文纯文本（UTF-8 带 BOM，页间以换页符分隔）。
    void ExportText(const fs::path& destination, const Cancel& cancel = {});
    // 压缩另存：重写并去重对象、子集化字体，按级别降采样与重压缩图片；校验后才替换目标。
    CompressResult Compress(const fs::path& destination, CompressLevel level, const Cancel& cancel = {});
    // 拆分为多个 PDF：每 amount 页 / 按顶层书签 / 每份不超过 amount MB（近似）。文件名为 <stem>-序号 说明.pdf。
    std::vector<fs::path> Split(SplitMode mode, int amount, const fs::path& folder, std::wstring_view stem,
                                const Cancel& cancel = {}, const std::function<void(int,int)>& progress = {});
    // 把页眉页脚与水印写入页面内容（一次可撤销的操作）。
    void Decorate(const PageDecoration& decoration, const Cancel& cancel = {});
    // 用扁平列表（按顺序，depth 表示层级）整体替换文档书签；一次可撤销的操作。
    void SetOutline(const std::vector<OutlineItem>& items);
    // 在本文档上直接执行涂黑（不可撤销的内容删除；通常只在副本上调用）。与标记重叠的批注（及其弹出注释）被删除，
    // 重叠的表单字段清空值与外观并隐藏。
    RedactionResult ApplyRedactions(const std::vector<RedactionMark>& marks,const RedactionOptions& options={});
    // 把当前文档（含未保存修改）涂黑后另存为 destination：当前文档与原文件都不变。
    // 发布前重新打开结果逐个标记校验：区域内残留任何可提取文字即失败，且不会留下输出文件。
    RedactionResult ExportRedacted(const std::vector<RedactionMark>& marks,const RedactionOptions& options,const fs::path& destination,const Cancel& cancel={});
    static void TextToPdf(std::wstring_view text, const fs::path& output, const Cancel& cancel = {},const ProgressSink& progress = {});
    static void ImageToPdf(const fs::path& image, const fs::path& output, bool a4,const Cancel& cancel = {},const ProgressSink& progress = {});
    // 图片像素尺寸。
    static std::pair<int,int> ImageSize(const fs::path& image);
    // 以一张图片新建文档（替换当前文档）：页面按图片像素 × 0.75 点（96 DPI），过大时等比缩到 200 英寸以内。
    // 新文档视为未保存（dirty），图片页本身不在撤销记录中。
    void NewFromImage(const fs::path& image);
    // 在 after 之后插入图片页（可撤销）：页面尺寸取相邻页（方向随图片），图片等比居中、不放大超过 96 DPI 原尺寸。
    void InsertImagePage(int after, const fs::path& image);
    static void Merge(const std::vector<MergeInput>& inputs, const fs::path& output, bool bookmarks,
                      const Cancel& cancel, const std::function<void(int,int)>& progress = {},const ProgressSink& detail = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}