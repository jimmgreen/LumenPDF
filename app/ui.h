#pragma once
#include <lumen/Button.h>
#include "core/conversion.h"
#include <algorithm>
#include <iterator>
#include <lumen/Painter.h>
#include <lumen/Icons.h>
#include <lumen/IconView.h>
namespace lpdf::ui {
inline constexpr float GlowIntensity=.18f;
inline constexpr wchar_t Cursor[]=L"\uF101",Text[]=L"\uF102",Marker[]=L"\uF103",Rectangle[]=L"\uF104",
    Arrow[]=L"\uF105",Ink[]=L"\uF106",Fit[]=L"\uF107",Merge[]=L"\uF108",
    PdfFile[]=L"\uF109",WordFile[]=L"\uF10A",TextFile[]=L"\uF10B",Underline[]=L"\uF10C",StrikeOut[]=L"\uF10D",
    Ellipse[]=L"\uF10E",Line[]=L"\uF10F",Stamp[]=L"\uF110",Compress[]=L"\uF111",Split[]=L"\uF112",Watermark[]=L"\uF113",Hand[]=L"\uF114",Sign[]=L"\uF115",ExcelFile[]=L"\uF116",SlidesFile[]=L"\uF117";
inline void RegisterIcons(){
    using lumen::icon::Register;
    Register(Cursor[0],"M48 28 L198 136 L126 148 L92 216 Z M126 148 L166 212",false,false);
    Register(Text[0],"M48 60 V40 H208 V60 M128 40 V216 M92 216 H164",false,false);
    Register(Marker[0],"M64 160 L152 40 L216 88 L128 208 Z M84 132 L152 180 M64 160 L44 204 L88 212 M36 232 H216",false,false);
    Register(Rectangle[0],"M48 56 H208 V200 H48 Z",false,false);
    Register(Arrow[0],"M48 208 L204 52 M112 52 H204 V144",false,false);
    Register(Ink[0],"M36 188 C60 76 96 60 104 96 C112 132 52 204 112 188 C140 180 140 124 156 140 C168 152 144 188 172 184 L220 168",false,false);
    Register(Fit[0],"M40 48 V208 M216 48 V208 M56 128 H200 M88 96 L56 128 L88 160 M168 96 L200 128 L168 160",false,false);
    Register(Merge[0],"M64 32 V80 C64 112 128 100 128 140 M192 32 V80 C192 112 128 100 128 140 V216 M88 180 L128 220 L168 180",false,false);
    Register(PdfFile[0],"M48 80 V24 H160 L208 72 V224 H48 V208 M160 24 V72 H208 M28 180 V116 H52 C82 116 82 148 52 148 H28 M100 180 V116 H116 C150 116 150 180 116 180 Z M170 180 V116 H210 M170 148 H202",false,false);
    Register(WordFile[0],"M48 24 H160 L208 72 V232 H48 Z M160 24 V72 H208 M72 112 L88 184 L124 124 L152 184 L176 112 M76 208 H176",false,false);
    Register(Underline[0],"M76 40 V120 C76 188 180 188 180 120 V40 M56 220 H200",false,false);
    Register(StrikeOut[0],"M180 64 C164 36 92 32 84 76 C76 120 176 116 180 168 C184 216 100 224 72 188 M40 128 H216",false,false);
    Register(Ellipse[0],"M128 52 C200 52 228 92 228 128 C228 164 200 204 128 204 C56 204 28 164 28 128 C28 92 56 52 128 52 Z",false,false);
    Register(Line[0],"M48 208 L208 48",false,false);
    Register(Stamp[0],"M100 128 V96 C100 76 88 72 88 52 C88 32 104 24 128 24 C152 24 168 32 168 52 C168 72 156 76 156 96 V128 M40 128 H216 V176 H40 Z M56 212 H200",false,false);
    Register(Compress[0],"M128 24 V100 M96 68 L128 100 L160 68 M128 232 V156 M96 188 L128 156 L160 188 M40 128 H216",false,false);
    Register(Split[0],"M48 32 H128 V224 H48 Z M160 32 H208 V224 H160 M144 32 V224",false,false);
    Register(Watermark[0],"M48 24 H160 L208 72 V232 H48 Z M160 24 V72 H208 M88 192 L168 104 M84 104 H116 M140 192 H172",false,false);
    Register(Hand[0],"M84 140 V64 C84 48 108 48 108 64 V124 M108 124 V48 C108 32 132 32 132 48 V124 M132 124 V56 C132 40 156 40 156 56 V128 M156 128 V80 C156 64 180 64 180 80 V160 C180 204 152 228 118 228 C88 228 72 214 58 192 L30 150 C22 136 40 124 54 136 L84 164",false,false);
    Register(Sign[0],"M28 168 C52 88 84 56 92 88 C100 120 60 176 92 164 C124 152 132 104 148 120 C160 132 152 164 176 152 L228 128 M28 212 H228",false,false);
    Register(ExcelFile[0],"M48 24 H160 L208 72 V232 H48 Z M160 24 V72 H208 M84 112 L164 200 M164 112 L84 200",false,false);
    Register(SlidesFile[0],"M48 24 H160 L208 72 V232 H48 Z M160 24 V72 H208 M96 204 V112 H136 C176 112 176 164 136 164 H96",false,false);
    Register(TextFile[0],"M48 24 H160 L208 72 V232 H48 Z M160 24 V72 H208 M80 112 H176 M80 144 H176 M80 176 H176 M80 208 H140",false,false);
}
inline std::wstring_view FileGlyph(InputFileKind kind){
    switch(kind){case InputFileKind::Pdf:return PdfFile;case InputFileKind::Word:return WordFile;
        case InputFileKind::Excel:return ExcelFile;case InputFileKind::PowerPoint:return SlidesFile;
        case InputFileKind::Text:return TextFile;case InputFileKind::Image:return lumen::icon::kImage;default:return lumen::icon::kFile;}
}
// 文件类型配色：PDF 红、Word 蓝、Excel 深绿、PPT 橙、文本浅灰、图片绿，与常见办公软件的习惯一致，列表里一眼可分。
inline uint32_t FileColor(InputFileKind kind){
    switch(kind){case InputFileKind::Pdf:return 0xf0564a;case InputFileKind::Word:return 0x4f8ff7;
        case InputFileKind::Excel:return 0x21a366;case InputFileKind::PowerPoint:return 0xf0883e;
        case InputFileKind::Text:return 0xc4c4c4;case InputFileKind::Image:return 0x3dbf7f;default:return 0x9a9a9a;}
}
// 带淡色底块的文件图标（合并列表、最近文件行共用）。
inline void DrawFileIcon(lumen::Painter& p,InputFileKind kind,const lumen::Rect& r,float tile,float glyph,bool dim=false){
    const float s=std::min({r.w,r.h,tile});
    const lumen::Rect box{r.x+(r.w-s)/2,r.y+(r.h-s)/2,s,s};
    const uint32_t rgb=dim?0x6a6a6a:FileColor(kind);
    p.FillRoundedRect(box,s*.24f,lumen::Color::Hex(rgb,.15f));
    p.StrokeRoundedRect(box.Inset(.5f,.5f),s*.24f,lumen::Color::Hex(rgb,.28f),1);
    p.DrawIcon(FileGlyph(kind),box,glyph,lumen::Color::Hex(rgb));
}
inline std::wstring_view ToolGlyph(Tool tool){
    using namespace lumen::icon;
    switch(tool){
    case Tool::Select:return Cursor;case Tool::Text:return Text;case Tool::Note:return kChat;case Tool::Highlight:return Marker;
    case Tool::Rectangle:return Rectangle;case Tool::Arrow:return Arrow;case Tool::Ink:return Ink;case Tool::Image:return kImage;
    case Tool::Underline:return Underline;case Tool::StrikeOut:return StrikeOut;case Tool::Ellipse:return Ellipse;
    case Tool::Line:return Line;case Tool::Stamp:return Stamp;
    }
    return {};
}
inline const wchar_t* ToolName(Tool tool){
    static const wchar_t* names[]={L"选择",L"文字",L"便签",L"高亮",L"矩形",L"箭头",L"手绘",L"图片",L"下划线",L"删除线",L"椭圆",L"直线",L"印章"};
    static_assert(std::size(names)==ToolCount);
    return names[static_cast<size_t>(tool)];
}
inline std::wstring_view Glyph(std::wstring_view name){
    using namespace lumen::icon;
    if(name==L"阅读")return kFile;if(name==L"批注")return kEdit;if(name==L"页面"||name==L"缩略图")return kGrid;
    if(name==L"合并"||name==L"合并并导出")return Merge;
    if(name==L"打开"||name==L"打开文档")return kFolderOpen;
    if(name==L"新建"||name==L"添加文件"||name==L"插入空白页")return kAdd;
    if(name==L"另存为")return kSave;if(name==L"撤销")return kUndo;if(name==L"重做")return kRedo;
    if(name==L"−")return kRemove;if(name==L"＋")return kAdd;if(name==L"适合页面")return Fit;if(name==L"适合宽度")return kMaximize;if(name==L"手型")return Hand;
    if(name==L"复制本页")return kCopy;if(name==L"打印")return kPrint;if(name==L"页面配色")return kSun;if(name==L"查找")return kSearch;
    if(name==L"上一处")return kChevronUp;if(name==L"下一处")return kChevronDown;
    if(name==L"选择")return Cursor;if(name==L"文字")return Text;if(name==L"高亮")return Marker;
    if(name==L"矩形")return Rectangle;if(name==L"箭头")return Arrow;if(name==L"手绘")return Ink;
    if(name==L"下划线")return Underline;if(name==L"删除线")return StrikeOut;if(name==L"椭圆")return Ellipse;
    if(name==L"直线")return Line;if(name==L"印章")return Stamp;if(name==L"签名")return Sign;
    if(name==L"便签")return kChat;if(name==L"图片")return kImage;if(name==L"目录")return kBookmark;
    if(name==L"旋转 90°"||name==L"图片旋转 90°")return kRefresh;
    if(name==L"插入 PDF")return kFile;if(name==L"提取选中页")return kDownload;
    if(name==L"删除选中页"||name==L"删除批注"||name==L"移除")return kDelete;
    if(name==L"应用修改")return kCheckMark;if(name==L"上移")return kArrowUp;if(name==L"下移")return kArrowDown;
    if(name==L"生成预览")return kView;if(name==L"取消任务")return kClose;
    return {};
}
class ColorSwatch final:public lumen::Button{
public:
    explicit ColorSwatch(uint32_t rgb):Button(L"",lumen::ButtonKind::Subtle),rgb_(rgb){Height(30);MinSize({30,30});MaxSize({30,30});}
    void Active(bool value){active_=value;Invalidate();}
protected:
    void Draw(lumen::Painter& p,const lumen::Theme& t)override{
        Button::Draw(p,t);
        const lumen::Rect r{absolute_.x+(absolute_.w-18)/2,absolute_.y+(absolute_.h-18)/2,18,18};
        p.FillRoundedRect(r,9,lumen::Color::Hex(rgb_));
        if(active_)p.StrokeRoundedRect(r.Inset(-3,-3),12,lumen::Color::Hex(0xf0f0f0),1.5f);
    }
private:uint32_t rgb_;bool active_{};
};
class Tab final:public lumen::Button{
public:
    explicit Tab(std::wstring_view label):Button(label,lumen::ButtonKind::Subtle){}
    void Active(bool value){active_=value;Role(value?lumen::TextRole::CaptionStrong:lumen::TextRole::Caption);Invalidate();}
protected:
    void Draw(lumen::Painter& p,const lumen::Theme& t)override{
        Button::Draw(p,t);
        if(active_)p.FillRoundedRect({absolute_.x+12,absolute_.Bottom()-2,absolute_.w-24,2},1,lumen::Color::Hex(0xefefef));
    }
private:bool active_{};
};
class DocumentArt final:public lumen::Control{
protected:
    lumen::Size Measure(lumen::Size,const lumen::Theme&)override{return {120,120};}
    void Draw(lumen::Painter& p,const lumen::Theme&)override{
        const float x=absolute_.x+(absolute_.w-120)/2,y=absolute_.y;
        p.FillRoundedRect({x+9,y+24,66,86},7,lumen::Color::Hex(0x171717));
        p.StrokeRoundedRect({x+9,y+24,66,86},7,lumen::Color::Hex(0x353535),1);
        p.FillRoundedRect({x+36,y+9,70,90},7,lumen::Color::Hex(0x202020));
        p.StrokeRoundedRect({x+36,y+9,70,90},7,lumen::Color::Hex(0x737373),1);
        p.DrawLine({x+52,y+37},{x+89,y+37},lumen::Color::Hex(0xe6e6e6),2);
        p.DrawLine({x+52,y+51},{x+89,y+51},lumen::Color::Hex(0x8d8d8d),2);
        p.DrawLine({x+52,y+65},{x+78,y+65},lumen::Color::Hex(0x8d8d8d),2);
        p.DrawIcon(lumen::icon::kAdd,{x+78,y+78,30,30},18,lumen::Color::Hex(0xffffff));
    }
    bool HitTransparent()const noexcept override{return true;}
};
}