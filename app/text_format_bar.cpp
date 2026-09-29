#include "text_format_bar.h"
#include "core/font_catalog.h"
#include "ui.h"
#include <lumen/ComboBox.h>
#include <lumen/NumberBox.h>
#include <lumen/ToggleButton.h>
#include <lumen/ColorPicker.h>
#include <lumen/Label.h>
#include <lumen/Separator.h>
#include <lumen/Painter.h>
#include <lumen/Icons.h>
#include <algorithm>
#include <cmath>

namespace lpdf {
namespace {
constexpr float Height=160;
constexpr wchar_t BoldIcon[]=L"\uF111",ItalicIcon[]=L"\uF112",UnderlineIcon[]=L"\uF113",
    LeftIcon[]=L"\uF114",CenterIcon[]=L"\uF115",RightIcon[]=L"\uF116",FitIcon[]=L"\uF117";
void RegisterIcons(){
    using lumen::icon::Register;
    Register(BoldIcon[0],"M70 36 H136 C194 36 194 124 136 124 H70 Z M70 124 H143 C208 124 208 220 143 220 H70 Z",false,false);
    Register(ItalicIcon[0],"M100 40 H202 M54 216 H156 M158 40 L98 216",false,false);
    Register(UnderlineIcon[0],"M64 36 V128 C64 200 192 200 192 128 V36 M48 224 H208",false,false);
    Register(LeftIcon[0],"M40 52 H216 M40 104 H152 M40 156 H216 M40 208 H152",false,false);
    Register(CenterIcon[0],"M40 52 H216 M72 104 H184 M40 156 H216 M72 208 H184",false,false);
    Register(RightIcon[0],"M40 52 H216 M104 104 H216 M40 156 H216 M104 208 H216",false,false);
    Register(FitIcon[0],"M40 36 H216 M40 220 H216 M128 60 V196 M96 92 L128 60 L160 92 M96 164 L128 196 L160 164",false,false);
}
class InkButton final:public lumen::Button {
public:
    InkButton():Button(L"A",lumen::ButtonKind::Subtle){}
    void Ink(uint32_t rgb){rgb_=rgb;Invalidate();}
protected:
    void Draw(lumen::Painter& p,const lumen::Theme& t)override{
        Button::Draw(p,t);
        const lumen::Rect r{absolute_.x+(absolute_.w-16)/2,absolute_.Bottom()-6,16,3};
        p.FillRoundedRect(r,1,lumen::Color::Hex(rgb_));
        p.StrokeRoundedRect(r,1,t.text_secondary,.6f);
    }
private:uint32_t rgb_{};
};
class Swatch final:public lumen::Button {
public:
    explicit Swatch(uint32_t rgb):Button(L"",lumen::ButtonKind::Subtle),rgb_(rgb){}
protected:
    void Draw(lumen::Painter& p,const lumen::Theme& t)override{
        Button::Draw(p,t);
        const lumen::Rect r{absolute_.x+(absolute_.w-18)/2,absolute_.y+(absolute_.h-18)/2,18,18};
        p.FillRoundedRect(r,9,lumen::Color::Hex(rgb_));p.StrokeRoundedRect(r,9,t.text_secondary,1);
    }
private:uint32_t rgb_{};
};
uint32_t Rgb(lumen::Color c){
    const auto byte=[](float v){return static_cast<uint32_t>(std::lround(std::clamp(v,0.0f,1.0f)*255));};
    return byte(c.r)<<16|byte(c.g)<<8|byte(c.b);
}
}
TextFormatBar::~TextFormatBar()=default;
void TextFormatBar::Build(HWND owner){
    using namespace lumen;
    RegisterIcons();
    WindowSpec spec;spec.title=L"文字格式 · 整条批注";spec.size={480,Height};spec.titleBar=false;
    spec.backdrop=Backdrop::None;spec.owner=owner;spec.matchDpiHwnd=owner;spec.cornerRadius=12;
    window_=std::make_unique<Window>(spec);window_->MinSize({320,Height});window_->GlowIntensity(ui::GlowIntensity);
    auto& root=window_->Root();root.Padding(12);root.Spacing(8);root.Card(Panel::CardStyle::Flyout,12);
    ThemeOverride compact{};compact.input_height=32;compact.button_height=32;root.Style(compact);
    auto& heading=root.Add<Row>();heading.MinSize({0,20}).MaxSize({100000,20});heading.AlignCross(CrossAlign::Center);heading.Spacing(8);
    heading.Add<Label>(L"文字格式").Role(TextRole::CaptionStrong);
    heading.Add<Label>(L"整条批注").Role(TextRole::Caption).Secondary(true).Grow();
    heading.Add<Button>(L"",ButtonKind::Subtle).Glyph(icon::kClose).SizeClass(ButtonSize::Small)
        .MinSize({24,20}).MaxSize({24,20}).AccessibleName(L"取消文字编辑").ToolTip(L"取消文字编辑 · Esc")
        .OnClick([this]{Finish(false);});
    auto& fields=root.Add<Row>();fields.Spacing(8);fields.MinSize({0,32}).MaxSize({100000,32});
    family_=&fields.Add<ComboBox>();family_->Items(InstalledFontFamilies()).Editable(true).MaxDropDownRows(8);
    family_->Grow().AccessibleName(L"字体").ToolTip(L"搜索或输入本机字体名称；字体将嵌入 PDF");
    family_->Editor().AccessibleName(L"字体名称").Placeholder(L"搜索本机字体");
    family_->OnTextCommitted([this](std::wstring_view){if(!syncing_&&CommitFields()&&!(GetKeyState(VK_TAB)&0x8000))ReturnFocus();});
    family_->OnSelectionChanged([this](ptrdiff_t,ptrdiff_t){if(!syncing_)CommitFields();});
    // Deferring avoids treating transfer into the dropdown as leaving the field.
    family_->Editor().OnFocused([this](bool focused){if(!focused&&active_&&!syncing_){
        const auto session=session_;window_->Post([this,session]{
            if(active_&&session_==session&&!window_->PopupActive()&&!family_->Editor().HasFocus())CommitFields();
        });
    }});
    sizeField_=&fields.Add<NumberBox>();sizeField_->Range(6,96).Step(.5).Decimals(1).Unit(L"pt").ClampOnCommit(false);
    sizeField_->MinSize({108,32}).MaxSize({108,32}).AccessibleName(L"字号").ToolTip(L"6–96 pt；可输入小数，↑↓ 每次调整 0.5 pt");
    sizeField_->OnValueChanged([this](double){if(!syncing_)CommitFields();});
    sizeField_->OnSubmit([this]{if(CommitFields())ReturnFocus();});
    auto& tools=root.Add<Row>();tools.MinSize({0,32}).MaxSize({100000,32});tools.Spacing(4);tools.AlignCross(CrossAlign::Center);
    const wchar_t* names[]={L"加粗",L"斜体",L"下划线"};
    const wchar_t* glyphs[]={BoldIcon,ItalicIcon,UnderlineIcon};
    const wchar_t* tips[]={L"加粗 · Ctrl+B",L"斜体 · Ctrl+I",L"下划线 · Ctrl+U"};
    for(size_t i=0;i<3;++i){
        auto& button=tools.Add<ToggleButton>();styles_[i]=&button;
        button.Glyph(glyphs[i]).MinSize({30,32}).MaxSize({30,32}).AccessibleName(names[i]).ToolTip(tips[i]);
        button.OnToggled([this,i](bool){if(!CommitFields()){State();return;}
            auto next=format_;if(i==0)next.bold=!next.bold;if(i==1)next.italic=!next.italic;if(i==2)next.underline=!next.underline;
            Apply(std::move(next),!styles_[i]->FocusVisible());});
    }
    tools.Add<Separator>().MinSize({1,20}).MaxSize({1,20}).Margin(4,0);
    const wchar_t* alignment[]={L"左对齐",L"居中",L"右对齐"};
    const wchar_t* alignGlyphs[]={LeftIcon,CenterIcon,RightIcon};
    for(size_t i=0;i<3;++i){
        auto& button=tools.Add<ToggleButton>();align_[i]=&button;
        button.Glyph(alignGlyphs[i]).MinSize({30,32}).MaxSize({30,32}).AccessibleName(alignment[i]).ToolTip(alignment[i]);
        button.OnToggled([this,i](bool){if(!CommitFields()){State();return;}
            auto next=format_;next.alignment=static_cast<int>(i);Apply(std::move(next),!align_[i]->FocusVisible());});
    }
    tools.Add<Separator>().MinSize({1,20}).MaxSize({1,20}).Margin(4,0);
    color_=&tools.Add<InkButton>();color_->MinSize({32,32}).MaxSize({32,32}).AccessibleName(L"文字颜色").ToolTip(L"文字颜色 · 色板与十六进制输入");
    color_->OnClick([this]{ChooseColor();});
    tools.Add<Panel>().Grow();
    auto& fit=tools.Add<Button>(L"",ButtonKind::Subtle);
    fit.Glyph(FitIcon).MinSize({32,32}).MaxSize({32,32}).AccessibleName(L"适合文字").ToolTip(L"调整高度以容纳全部文字，宽度不变");
    fit.OnClick([this,&fit]{if(CommitFields()){if(fit_)fit_();if(!fit.FocusVisible())ReturnFocus();}});
    auto& footer=root.Add<Row>();footer.MinSize({0,28}).MaxSize({100000,28});footer.Spacing(8);footer.AlignCross(CrossAlign::Center);
    message_=&footer.Add<Label>();message_->Role(TextRole::Caption).Secondary(true).Grow();
    footer.Add<Button>(L"完成",ButtonKind::Primary).SizeClass(ButtonSize::Small).MinSize({60,28}).MaxSize({60,28})
        .AccessibleName(L"完成文字编辑").ToolTip(L"完成文字编辑 · Ctrl+Enter").OnClick([this]{Finish(true);});
    window_->BindShortcut(L"Escape",[this]{Finish(false);});
    for(const auto key:{L'B',L'I',L'U',L'L',L'E',L'R'}){
        window_->BindShortcut(std::wstring(L"Ctrl+")+key,[this,key]{Toggle(key);});
    }
    window_->OnNativeMessage([this](uint32_t msg,std::uintptr_t w,std::intptr_t){
        if(msg==WM_KEYDOWN&&w==VK_RETURN&&(GetKeyState(VK_CONTROL)&0x8000)&&
            !window_->PopupActive()&&!family_->Editor().Composing()){
            const auto session=session_;window_->Post([this,session]{if(active_&&session_==session)Finish(true);});
        }
    });
    window_->OnClosing([this]{Finish(false);return false;});
}
void TextFormatBar::Begin(HWND owner,const Annotation& a,std::function<void(TextFormat,float)> changed,
    std::function<void(bool)> finish,std::function<void()> focus,std::function<void()> fit){
    owner_=owner;changed_=std::move(changed);finish_=std::move(finish);focus_=std::move(focus);fit_=std::move(fit);
    if(!window_)Build(owner);++session_;active_=true;overflow_=false;error_=false;Sync(a);Overflow(false);
}
void TextFormatBar::State(){
    if(!window_)return;
    styles_[0]->Checked(format_.bold);styles_[1]->Checked(format_.italic);styles_[2]->Checked(format_.underline);
    for(size_t i=0;i<3;++i)align_[i]->Checked(static_cast<int>(i)==format_.alignment);
    static_cast<InkButton*>(color_)->Ink(format_.color);
}
void TextFormatBar::Sync(const Annotation& a){
    format_=a.textFormat;size_=a.fontSize;if(!window_)return;syncing_=true;
    family_->Text(format_.family);family_->CommitText();sizeField_->Value(size_);sizeDisplay_=sizeField_->Text();State();syncing_=false;
}
bool TextFormatBar::CommitFields(){
    if(!active_||syncing_||committing_)return true;
    if(choosing_||family_->Editor().Composing())return false;
    auto next=format_;next.family=family_->Text();double size=0;
    if(!sizeField_->TryParse(size)||size<6||size>96){Message(L"字号请输入 6–96 pt；草稿已保留。",true);return false;}
    try{if(next.family!=format_.family)(void)ResolveSystemFont(next.family,next.bold,next.italic);}
    catch(...){Message(L"字体不可用或不允许嵌入；请重新选择。",true);return false;}
    // Display precision is not a document edit. Preserve imported fractional
    // point sizes until the user actually changes the NumberBox's text/value.
    const float effectiveSize=sizeField_->Text()==sizeDisplay_?size_:static_cast<float>(size);
    const bool changed=next!=format_||effectiveSize!=size_;
    format_=std::move(next);size_=effectiveSize;sizeDisplay_=sizeField_->Text();error_=false;committing_=true;
    // Commit the field's baseline too: subsequent Escape must not restore a stale family.
    syncing_=true;family_->CommitText();syncing_=false;State();Overflow(overflow_);
    if(changed&&changed_)changed_(format_,size_);committing_=false;return true;
}
void TextFormatBar::Message(std::wstring text,bool error){
    error_=error;if(!message_)return;message_->Text(text).Secondary(!error);message_->ToolTip(text);message_->AccessibleName(text);
}
void TextFormatBar::Overflow(bool value){
    overflow_=value;if(error_)return;
    Message(value?L"内容超出框 · 可点“适合文字”":L"Ctrl+Enter 完成 · Esc 取消");
}
void TextFormatBar::ReturnFocus(){
    const auto session=session_;window_->Post([this,session]{
        if(active_&&session_==session&&!choosing_&&!window_->PopupActive()){
            window_->ClearFocus();if(focus_)focus_();
        }
    });
}
void TextFormatBar::Apply(TextFormat next,bool returnFocus){
    try{if(next.family!=format_.family||next.bold!=format_.bold||next.italic!=format_.italic)
        (void)ResolveSystemFont(next.family,next.bold,next.italic);}
    catch(...){Message(L"此字体样式不允许嵌入；请换一个字体。",true);State();return;}
    const bool changed=next!=format_;format_=std::move(next);State();
    if(changed&&changed_)changed_(format_,size_);if(returnFocus)ReturnFocus();
}
void TextFormatBar::Toggle(wchar_t key){
    if(!active_||choosing_||!CommitFields())return;auto next=format_;
    if(key==L'B')next.bold=!next.bold;if(key==L'I')next.italic=!next.italic;if(key==L'U')next.underline=!next.underline;
    if(key==L'L')next.alignment=0;if(key==L'E')next.alignment=1;if(key==L'R')next.alignment=2;
    Apply(std::move(next),true);
}
void TextFormatBar::ChooseColor(){
    using namespace lumen;
    if(!active_||choosing_||window_->PopupActive()||!CommitFields())return;
    const auto session=session_;const auto before=format_.color;bool accepted=false;
    // ShowPopup owns the outer surface, corner radius, shadow and border.
    // Its content is layout only; adding a Flyout card here draws a second shell.
    Column content;content.Spacing(8);content.Padding(8);
    content.Add<Label>(L"文字颜色").Role(TextRole::CaptionStrong);
    auto& picker=content.Add<ColorPicker>();picker.Color(Color::Hex(before)).AccessibleName(L"文字颜色选择器");
    picker.OnColorChanged([this,session](Color value){if(active_&&session_==session){
        auto next=format_;next.color=Rgb(value);Apply(std::move(next),false);
    }});
    auto& swatches=content.Add<Row>();swatches.Spacing(5);
    const uint32_t colors[]={0x171717,0x64748b,0x2456a8,0x7c3aed,0xd43c42,0x24844b};
    const wchar_t* names[]={L"黑色",L"灰色",L"蓝色",L"紫色",L"红色",L"绿色"};
    for(size_t i=0;i<std::size(colors);++i){const auto rgb=colors[i];
        swatches.Add<Swatch>(rgb).MinSize({32,30}).MaxSize({32,30}).AccessibleName(names[i]).ToolTip(names[i])
            .OnClick([&picker,this,rgb]{picker.Color(Color::Hex(rgb));auto next=format_;next.color=rgb;Apply(std::move(next),false);});
    }
    auto& footer=content.Add<Row>();footer.Spacing(8);footer.AlignMain(MainAlign::End);
    footer.Add<Button>(L"取消",ButtonKind::Subtle).SizeClass(ButtonSize::Small).OnClick([this]{window_->ClosePopup();});
    footer.Add<Button>(L"使用颜色",ButtonKind::Primary).SizeClass(ButtonSize::Small)
        .OnClick([this,&accepted]{accepted=true;window_->ClosePopup();});
    choosing_=true;
    window_->Post([weak=std::make_shared<WeakRef<ColorPicker>>(&picker)]{if(auto* value=weak->Get())value->Focus();});
    window_->ShowPopup(content,color_,280);
    choosing_=false;
    if(!active_||session_!=session)return;
    if(!accepted){auto next=format_;next.color=before;Apply(std::move(next),false);}
    ReturnFocus();
}
void TextFormatBar::Finish(bool apply){
    if(!active_||choosing_)return;if(apply&&!CommitFields())return;
    const auto finish=finish_;if(finish)finish(apply);
}
void TextFormatBar::Place(lumen::Rect box,lumen::Rect viewport,float dpi){
    if(!active_||!window_)return;
    if(box.Bottom()<viewport.y||box.y>viewport.Bottom()||box.Right()<viewport.x||box.x>viewport.Right()){
        window_->Hide();return;
    }
    const float width=std::min(480.0f,std::max(320.0f,viewport.w-24));
    const float x=std::clamp(box.x-6,viewport.x+12,std::max(viewport.x+12,viewport.Right()-width-12));
    float y=box.y-Height-18;if(y<viewport.y+8)y=box.Bottom()+18;
    y=std::clamp(y,viewport.y+8,std::max(viewport.y+8,viewport.Bottom()-Height-8));
    POINT origin{};ClientToScreen(owner_,&origin);
    SetWindowPos(Handle(),HWND_TOP,origin.x+static_cast<int>(std::lround(x*dpi)),origin.y+static_cast<int>(std::lround(y*dpi)),
        static_cast<int>(std::lround(width*dpi)),static_cast<int>(std::lround(Height*dpi)),SWP_NOACTIVATE);
    if(!window_->Visible())window_->Show(false);
}
void TextFormatBar::End(){
    active_=false;++session_;if(window_){window_->ClosePopup();window_->ClearFocus();window_->Hide();}
}
}
