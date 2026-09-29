// 填写与签名：签名库（手写 / 文字 / 图片，透明 PNG）、一键放置签名、日期与 ✓ / ✗ 标记。
// 签名作为标准图片批注写入 PDF；日期是文字批注；✓ / ✗ 是手绘批注——都可移动、缩放、撤销，其它阅读器也能正常显示。
#include "application.h"
#include "ui.h"
#include "app_log.h"
#include "text_layout.h"
#include "core/signature.h"
#include <lumen/Dialog.h>
#include <lumen/Menu.h>
#include <wrl/client.h>
#include <d2d1_1.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <ctime>

namespace lpdf {
using namespace lumen;
namespace {
constexpr uint32_t InkColors[]{0x1f1f1f,0x1f4fbf,0xc62828};
constexpr const wchar_t* InkNames[]{L"黑色",L"蓝色",L"红色"};
constexpr float PadPen=2.6f;

// 签名板：左键拖动书写，每次按下开始一笔。坐标为控件内 DIP。
class SignaturePad final:public Control{
public:
    std::vector<std::vector<lpdf::Point>> strokes;
    uint32_t rgb{InkColors[0]};
    std::function<void()> changed;
    void Clear(){strokes.clear();drawing_=false;Invalidate();if(changed)changed();}
    void UndoStroke(){if(!strokes.empty())strokes.pop_back();drawing_=false;Invalidate();if(changed)changed();}
    void Ink(uint32_t value){rgb=value;Invalidate();}
protected:
    Size Measure(Size available,const Theme&)override{return {std::max(320.0f,std::min(available.w,480.0f)),200};}
    void Draw(Painter& p,const Theme&)override{
        p.FillRoundedRect(absolute_,10,Color::Hex(0xfbfbf8));
        p.StrokeRoundedRect(absolute_.Inset(.5f,.5f),10,Color::Hex(0x6a6a6a),1);
        const float base=absolute_.y+absolute_.h*.74f;
        p.DrawDashedLine({absolute_.x+28,base},{absolute_.Right()-28,base},Color::Hex(0xc4c4c4),1);
        p.DrawLine({absolute_.x+30,base-14},{absolute_.x+40,base-4},Color::Hex(0xb0b0b0),1.4f);
        p.DrawLine({absolute_.x+40,base-14},{absolute_.x+30,base-4},Color::Hex(0xb0b0b0),1.4f);
        p.PushClip(absolute_);
        const auto ink=Color::Hex(rgb);
        for(const auto& s:strokes){
            if(s.empty())continue;
            if(s.size()==1){p.FillRoundedRect({absolute_.x+s[0].x-PadPen/2,absolute_.y+s[0].y-PadPen/2,PadPen,PadPen},PadPen/2,ink);continue;}
            std::vector<lumen::Point> pts;pts.reserve(s.size());
            for(auto q:s)pts.push_back({absolute_.x+q.x,absolute_.y+q.y});
            p.StrokeOpenPolyline(pts.data(),static_cast<int>(pts.size()),ink,PadPen);
        }
        p.PopClip();
    }
    void OnMouseDown(lumen::Point local,uint32_t)override{
        strokes.push_back({Clamp(local)});drawing_=true;Invalidate();
    }
    void OnMouseMove(lumen::Point local,uint32_t buttons)override{
        if(!drawing_)return;
        if(!buttons){Finish();return;}
        auto& s=strokes.back();const auto q=Clamp(local);
        if(std::hypot(q.x-s.back().x,q.y-s.back().y)>=.8f){s.push_back(q);Invalidate();}
    }
    void OnMouseUp(lumen::Point local,uint32_t)override{
        if(!drawing_)return;
        auto& s=strokes.back();const auto q=Clamp(local);
        if(std::hypot(q.x-s.back().x,q.y-s.back().y)>=.3f)s.push_back(q);
        Finish();
    }
    bool PrefersDragOverPan()const noexcept override{return true;}
    CursorShape CursorAt(lumen::Point)const override{return CursorShape::Cross;}
    std::wstring AutomationName()const override{return L"签名板";}
private:
    bool drawing_{};
    lpdf::Point Clamp(lumen::Point q)const{return {std::clamp(q.x,0.0f,absolute_.w),std::clamp(q.y,0.0f,absolute_.h)};}
    void Finish(){drawing_=false;Invalidate();if(changed)changed();}
};

// 签名预览：在浅色底板上按比例居中显示透明 PNG。
class SignaturePreview final:public Control{
public:
    SignaturePreview(float w,float h):w_(w),h_(h){}
    void Image(const Bitmap& b){
        pixels_=b.bgra.empty()?Bitmap{}:Premultiplied(ScaledToFit(b,static_cast<int>(w_*2),static_cast<int>(h_*2)));
        bitmap_.Reset();Invalidate();
    }
protected:
    Size Measure(Size available,const Theme&)override{return {w_<=0?available.w:w_,h_};}
    void Draw(Painter& p,const Theme&)override{
        p.FillRoundedRect(absolute_,8,Color::Hex(0xf4f4f1));
        p.StrokeRoundedRect(absolute_.Inset(.5f,.5f),8,Color::Hex(0x4a4a4a),1);
        if(pixels_.bgra.empty())return;
        if(!bitmap_||device_!=p.DeviceIdentity()){
            bitmap_.Attach(p.CreateBitmapBgra(pixels_.width,pixels_.height,pixels_.bgra.data(),pixels_.stride));device_=p.DeviceIdentity();
        }
        if(!bitmap_)return;
        const auto box=absolute_.Inset(10,8);
        const float f=std::min({box.w/pixels_.width,box.h/pixels_.height,1.0f});
        const float w=pixels_.width*f,h=pixels_.height*f;
        p.DrawBitmap(bitmap_.Get(),{box.x+(box.w-w)/2,box.y+(box.h-h)/2,w,h},true);
    }
    bool HitTransparent()const noexcept override{return true;}
private:
    float w_,h_;Bitmap pixels_;Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap_;void* device_{};
};

Row& InkRow(Panel& parent,std::function<void(uint32_t)> pick){
    auto& row=parent.Add<Row>();row.Spacing(6);row.AlignCross(CrossAlign::Center);
    row.Add<Label>(L"颜色",TextRole::Caption).Secondary(true).MinSize({36,30});
    auto swatches=std::make_shared<std::vector<ui::ColorSwatch*>>();
    for(size_t i=0;i<std::size(InkColors);++i){
        auto& s=row.Add<ui::ColorSwatch>(InkColors[i]);s.AccessibleName(InkNames[i]).ToolTip(InkNames[i]);
        s.Active(i==0);swatches->push_back(&s);
        s.OnClick([pick,i,swatches]{for(size_t k=0;k<swatches->size();++k)(*swatches)[k]->Active(k==i);pick(InkColors[i]);});
    }
    return row;
}
std::wstring KindLabel(SignatureKind kind){
    return kind==SignatureKind::Drawn?L"手写签名":kind==SignatureKind::Typed?L"文字签名":L"图片签名";
}
std::wstring EntryLabel(const SignatureEntry& e){
    std::wstring label=KindLabel(e.kind);
    const time_t seconds=static_cast<time_t>(e.created/1000);tm local{};
    if(localtime_s(&local,&seconds)==0){wchar_t text[32]{};wcsftime(text,32,L" · %m-%d %H:%M",&local);label+=text;}
    return label;
}
std::wstring Today(bool chinese){
    const time_t now=time(nullptr);tm local{};localtime_s(&local,&now);wchar_t text[32]{};
    if(chinese)swprintf_s(text,L"%d年%d月%d日",local.tm_year+1900,local.tm_mon+1,local.tm_mday);
    else swprintf_s(text,L"%04d-%02d-%02d",local.tm_year+1900,local.tm_mon+1,local.tm_mday);
    return text;
}
std::wstring Friendly(const std::exception& e){
    const std::string what=e.what();
    if(what.find("full")!=std::string::npos)return L"签名库已满（最多 "+std::to_wstring(MaxSignatures)+L" 个）。请先在“管理签名”中删除不用的签名。";
    if(what.find("No signature strokes")!=std::string::npos)return L"图片中没有找到签名笔迹。请换一张对比更清楚的图片，或取消“去除背景”。";
    if(what.find("Draw your signature")!=std::string::npos)return L"请先在签名板上书写。";
    if(what.find("Enter the signature")!=std::string::npos)return L"请输入签名文字。";
    if(what.find("too long")!=std::string::npos)return L"签名文字最多 40 个字符。";
    if(what.find("read the image")!=std::string::npos)return L"无法读取这张图片。支持 PNG、JPG、BMP。";
    return L"操作失败："+Wide(what);
}
struct TypedFont{const wchar_t* family;const wchar_t* label;};
constexpr TypedFont TypedFonts[]{{L"STXingkai",L"华文行楷"},{L"STKaiti",L"华文楷体"},{L"KaiTi",L"楷体"},{L"FZShuTi",L"方正舒体"},
    {L"LiSu",L"隶书"},{L"Segoe Script",L"Segoe Script"},{L"Ink Free",L"Ink Free"},{L"Microsoft YaHei",L"微软雅黑"}};
}

fs::path Application::SignatureFolder()const{
    wchar_t forced[MAX_PATH]{};
    if(GetEnvironmentVariableW(L"LPDF_SIGNATURE_DIR",forced,MAX_PATH))return forced;
    if(smoke_)return {};
    PWSTR local=nullptr;fs::path folder;
    if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local))){folder=fs::path(local)/L"LumenPDF/Signatures";CoTaskMemFree(local);}
    return folder;
}

void Application::SignatureMenu(){
    if(!loaded_||busy_)return;
    Menu menu;
    menu.AddHeader(L"我的签名");
    const auto entries=ListSignatures(SignatureFolder());
    if(entries.empty())menu.AddItem(L"还没有保存的签名",{}).Disabled(true);
    for(const auto& e:entries)menu.AddItem(EntryLabel(e),[this,e]{
        try{
            const auto b=LoadSignaturePng(e.file);
            Placement p;p.kind=Placement::Signature;p.file=e.file;p.width=b.width;p.height=b.height;ArmPlacement(std::move(p));
        }catch(const std::exception&){window_.Alert(L"签名文件无法读取",L"该签名文件可能已损坏或被删除，请在“管理签名”中删除后重新创建。");}
    }).Glyph(ui::Sign);
    menu.AddSeparator();
    const bool full=entries.size()>=MaxSignatures;
    menu.AddItem(L"手写新签名…",[this]{DrawSignature();}).Disabled(full).Glyph(ui::Ink);
    menu.AddItem(L"输入文字签名…",[this]{TypeSignature();}).Disabled(full).Glyph(ui::Text);
    menu.AddItem(L"从图片导入签名…",[this]{ImportSignature();}).Disabled(full).Glyph(icon::kImage);
    menu.AddItem(L"管理签名…",[this]{ManageSignatures();}).Disabled(entries.empty()).Glyph(icon::kSettings);
    menu.AddSeparator();
    menu.AddHeader(L"快速填写");
    for(bool chinese:{false,true}){
        const auto text=Today(chinese);
        menu.AddItem(L"今天日期  "+text,[this,text]{Placement p;p.kind=Placement::Date;p.text=text;ArmPlacement(std::move(p));}).Glyph(icon::kCalendar);
    }
    menu.AddItem(L"对勾 ✓",[this]{Placement p;p.kind=Placement::Check;ArmPlacement(std::move(p));}).Glyph(icon::kCheckMark);
    menu.AddItem(L"叉号 ✗",[this]{Placement p;p.kind=Placement::Cross;ArmPlacement(std::move(p));}).Glyph(icon::kClose);
    menu.PopupTo(*signButton_);
}

void Application::ArmPlacement(Placement placement){
    if(!loaded_||busy_)return;
    if(textEditor_.Active()){FinishText(true,[this,placement]{ArmPlacement(placement);});return;}
    ChooseTool(Tool::Image);   // 借用图片工具的“单击 / 拖框”放置交互
    placement_=std::move(placement);
    const wchar_t* what=placement_.kind==Placement::Signature?L"签名":placement_.kind==Placement::Date?L"日期":placement_.kind==Placement::Check?L"对勾":L"叉号";
    status_->Text(std::wstring(L"单击页面放置")+what+L"，或拖出方框指定大小  ·  按住 Shift 可连续放置  ·  Esc 取消");
}

bool Application::PlacePending(int p,Rect r){
    if(placement_.kind==Placement::None||p<0||p>=static_cast<int>(info_.pages.size()))return false;
    const auto placement=placement_;
    const bool keep=(GetKeyState(VK_SHIFT)&0x8000)!=0;
    if(!keep)placement_={};
    // 画布对“单击”给出 160×100 的默认框；拖框时使用用户框。
    const bool clicked=std::abs(r.w-160)<.01f&&std::abs(r.h-100)<.01f;
    const auto& page=info_.pages[p];
    auto fit=[&](Rect box){
        box.w=std::min(box.w,page.width);box.h=std::min(box.h,page.height);
        box.x=std::clamp(box.x,page.originX,page.originX+page.width-box.w);box.y=std::clamp(box.y,page.originY,page.originY+page.height-box.h);
        return box;
    };
    auto done=[this,p,keep]{
        SelectCreated(p,keep);
        if(keep&&placement_.kind!=Placement::None)status_->Text(L"已放置，可继续单击放置  ·  松开 Shift 单击放置最后一个  ·  Esc 结束");
    };
    switch(placement.kind){
    case Placement::Signature:{
        const float aspect=placement.height>0?static_cast<float>(placement.width)/placement.height:3.0f;
        Rect box=r;
        if(clicked){
            box.w=150;box.h=box.w/aspect;
            if(box.h>60){box.h=60;box.w=box.h*aspect;}
            box.x=r.x-box.w/2;box.y=r.y-box.h/2;
        }
        box=fit(box);
        const auto file=placement.file;const float opacity=toolOpacities_[static_cast<size_t>(Tool::Image)];
        const auto style=toolStyles_[static_cast<size_t>(Tool::Image)];
        Task(L"放置签名",[p,box,file,opacity,style](Engine& e,const Cancel&){e.document.AddAnnotation(p,Tool::Image,box,{},file,{},12,opacity,style);},done);
        return true;
    }
    case Placement::Date:{
        Annotation a;a.id=-1;a.type=Tool::Text;a.text=placement.text;a.fontSize=defaultFont_;a.textFormat=defaultTextFormat_;
        a.opacity=toolOpacities_[static_cast<size_t>(Tool::Text)];a.textSizing=TextSizing::Auto;a.fixedTextBox=true;a.fixedTextWidth=true;a.lumenText=true;
        a.bounds={r.x,r.y-a.fontSize*.7f,20,20};
        if(!clicked){a.bounds={r.x,r.y,r.w,r.h};a.textSizing=TextSizing::Width;}
        lumen::TextLayout layout;a.bounds=FitTextBounds(a,page,layout);a.bounds=fit(a.bounds);
        Task(L"填写日期",[p,a](Engine& e,const Cancel&){e.document.AddTextAnnotation(p,a);},done);
        return true;
    }
    case Placement::Check:case Placement::Cross:{
        float side=clicked?14:std::max(6.0f,std::min(r.w,r.h));
        Rect box=clicked?Rect{r.x-side/2,r.y-side/2,side,side}:Rect{r.x+(r.w-side)/2,r.y+(r.h-side)/2,side,side};
        box=fit(box);side=box.w;
        auto style=toolStyles_[static_cast<size_t>(Tool::Ink)];style.dashed=false;style.lineWidth=std::clamp(side*.11f,1.2f,6.0f);
        const auto strokes=placement.kind==Placement::Check?CheckMarkStrokes(box):CrossMarkStrokes(box);
        const float opacity=toolOpacities_[static_cast<size_t>(Tool::Ink)];
        Task(placement.kind==Placement::Check?L"放置对勾":L"放置叉号",[p,strokes,style,opacity](Engine& e,const Cancel&){e.document.AddInkStrokes(p,strokes,style,opacity);},done);
        return true;
    }
    default:return false;
    }
}

// 保存新签名后直接进入放置状态。
void Application::StoreSignature(SignatureKind kind,const Bitmap& bitmap){
    try{
        const auto folder=SignatureFolder();
        if(folder.empty())throw std::runtime_error("The signature folder is unavailable");
        const auto entry=AddSignature(folder,kind,bitmap);
        Placement p;p.kind=Placement::Signature;p.file=entry.file;p.width=bitmap.width;p.height=bitmap.height;
        ArmPlacement(std::move(p));
        status_->Text(L"签名已保存到签名库  ·  单击页面放置，或拖出方框指定大小  ·  Esc 取消");
    }catch(const std::exception& e){window_.Alert(L"无法保存签名",Friendly(e));}
}

void Application::DrawSignature(){
    if(!loaded_||busy_||window_.DialogActive())return;
    struct Controls{SignaturePad* pad{};Button* undo{};Button* clear{};};
    auto c=std::make_shared<Controls>();
    DialogSpec dialog;dialog.title=L"手写签名";
    dialog.message=L"按住鼠标左键（或用触控笔）在下方书写。保存后签名进入签名库，可反复放置；背景透明，只保留笔迹。";
    dialog.size=DialogSize::Standard;dialog.default_button=DialogCommand::Primary;
    dialog.content=[c](Panel& panel){
        c->pad=&panel.Add<SignaturePad>();
        auto& tools=InkRow(panel,[c](uint32_t rgb){c->pad->Ink(rgb);});
        tools.Add<Label>().Grow();
        c->undo=&tools.Add<Button>(L"撤销一笔",ButtonKind::Subtle);c->undo->Glyph(icon::kUndo).OnClick([c]{c->pad->UndoStroke();});
        c->clear=&tools.Add<Button>(L"清除",ButtonKind::Subtle);c->clear->Glyph(icon::kDelete).OnClick([c]{c->pad->Clear();});
    };
    dialog.primary={L"保存并放置",{}};dialog.close={L"取消",{}};
    dialog.on_result=[this,c](DialogResult result){
        if(result!=DialogResult::Primary){status_->Text(L"已取消手写签名");return;}
        if(c->pad->strokes.empty()){window_.Alert(L"签名为空",L"请先在签名板上书写。");return;}
        try{StoreSignature(SignatureKind::Drawn,RenderDrawnSignature(c->pad->strokes,PadPen,c->pad->rgb));}
        catch(const std::exception& e){window_.Alert(L"无法生成签名",Friendly(e));}
    };
    window_.ShowDialog(std::move(dialog));
}

void Application::TypeSignature(){
    if(!loaded_||busy_||window_.DialogActive())return;
    struct Controls{TextBox* text{};ComboBox* font{};SignaturePreview* preview{};uint32_t rgb{InkColors[0]};std::vector<std::wstring> families;};
    auto c=std::make_shared<Controls>();
    std::vector<std::wstring> labels;
    for(const auto& f:TypedFonts)if(SignatureFontInstalled(f.family)){c->families.push_back(f.family);labels.push_back(f.label);}
    if(c->families.empty()){c->families.push_back(L"Microsoft YaHei");labels.push_back(L"微软雅黑");}
    DialogSpec dialog;dialog.title=L"文字签名";
    dialog.message=L"输入姓名并选择字体，生成透明背景的签名图片。只列出本机已安装的手写 / 书法风格字体。";
    dialog.size=DialogSize::Standard;dialog.default_button=DialogCommand::Primary;
    dialog.content=[c,labels](Panel& panel){
        c->text=&panel.Add<TextBox>().Placeholder(L"签名文字，例如姓名").AccessibleName(L"签名文字");
        c->font=&panel.Add<ComboBox>();c->font->AccessibleName(L"签名字体");
        for(const auto& l:labels)c->font->AddItem(l);
        c->font->SelectedIndex(0);
        InkRow(panel,[c](uint32_t rgb){c->rgb=rgb;if(c->text&&c->preview){
            try{c->preview->Image(RenderTypedSignature(c->text->Text(),c->families[static_cast<size_t>(std::max<ptrdiff_t>(0,c->font->SelectedIndex()))],c->rgb,96));}catch(...){c->preview->Image({});}
        }});
        c->preview=&panel.Add<SignaturePreview>(0.0f,110.0f);
        auto update=[c]{
            try{c->preview->Image(RenderTypedSignature(c->text->Text(),c->families[static_cast<size_t>(std::max<ptrdiff_t>(0,c->font->SelectedIndex()))],c->rgb,96));}
            catch(...){c->preview->Image({});}
        };
        c->text->OnTextChanged([update](std::wstring_view){update();});
        c->font->OnSelectionChanged([update](ptrdiff_t,ptrdiff_t){update();});
        c->text->Focus();
    };
    dialog.primary={L"保存并放置",{}};dialog.close={L"取消",{}};
    dialog.on_result=[this,c](DialogResult result){
        if(result!=DialogResult::Primary){status_->Text(L"已取消文字签名");return;}
        try{
            const auto family=c->families[static_cast<size_t>(std::max<ptrdiff_t>(0,c->font->SelectedIndex()))];
            StoreSignature(SignatureKind::Typed,RenderTypedSignature(c->text->Text(),family,c->rgb));
        }catch(const std::exception& e){window_.Alert(L"无法生成签名",Friendly(e));}
    };
    window_.ShowDialog(std::move(dialog));
}

void Application::ImportSignature(){
    if(!loaded_||busy_||window_.DialogActive())return;
    static constexpr wchar_t filter[]=L"图片\0*.png;*.jpg;*.jpeg;*.bmp\0";
    auto files=Pick(false,filter);if(files.empty()){status_->Text(L"已取消导入签名");return;}
    const auto file=files.front();
    struct Controls{CheckBox* remove{};SignaturePreview* preview{};Bitmap current;bool ok{};};
    auto c=std::make_shared<Controls>();
    auto process=[c,file](bool remove){
        try{c->current=ImportSignatureImage(file,remove);c->ok=true;}catch(...){c->current={};c->ok=false;}
        if(c->preview)c->preview->Image(c->current);
    };
    process(true);
    DialogSpec dialog;dialog.title=L"从图片导入签名";
    dialog.message=L"建议在白纸上用深色笔签名后拍照或扫描。“去除背景”会把纸张底色变为透明，并自动裁掉四周空白。";
    dialog.size=DialogSize::Standard;dialog.default_button=DialogCommand::Primary;
    dialog.content=[c,process](Panel& panel){
        c->preview=&panel.Add<SignaturePreview>(0.0f,150.0f);c->preview->Image(c->current);
        c->remove=&panel.Add<CheckBox>(L"去除背景（纸张底色变透明）").Checked(true);
        c->remove->OnToggled([process](bool on){process(on);});
    };
    dialog.primary={L"保存并放置",{}};dialog.close={L"取消",{}};
    dialog.on_result=[this,c](DialogResult result){
        if(result!=DialogResult::Primary){status_->Text(L"已取消导入签名");return;}
        if(!c->ok||c->current.bgra.empty()){window_.Alert(L"无法导入签名",L"图片中没有找到签名笔迹，或图片无法读取。请换一张对比更清楚的图片，或取消“去除背景”。");return;}
        StoreSignature(SignatureKind::Image,c->current);
    };
    window_.ShowDialog(std::move(dialog));
}

void Application::ManageSignatures(){
    if(window_.DialogActive())return;
    const auto entries=ListSignatures(SignatureFolder());
    if(entries.empty()){window_.Alert(L"签名库为空",L"还没有保存的签名。");return;}
    DialogSpec dialog;dialog.title=L"管理签名";
    dialog.message=L"签名只保存在本机（"+SignatureFolder().wstring()+L"），不会上传。删除后无法恢复，已放置到文档中的签名不受影响。";
    dialog.size=DialogSize::Standard;
    dialog.content=[this,entries](Panel& panel){
        for(const auto& e:entries){
            auto& row=panel.Add<Row>();row.Spacing(10);row.AlignCross(CrossAlign::Center);
            auto& preview=row.Add<SignaturePreview>(170.0f,58.0f);
            try{preview.Image(LoadSignaturePng(e.file));}catch(...){}
            row.Add<Label>(EntryLabel(e),TextRole::Caption).Secondary(true).Grow();
            auto* rowPtr=&row;
            row.Add<Button>(L"删除",ButtonKind::Subtle).Glyph(icon::kDelete).AccessibleName(L"删除 "+EntryLabel(e)).OnClick([this,e,rowPtr]{
                try{RemoveSignature(e);rowPtr->Visible(false);status_->Text(L"已删除签名："+EntryLabel(e));}
                catch(const std::exception&){window_.Alert(L"无法删除签名",L"文件可能正在被占用，请稍后重试。");}
            });
        }
    };
    dialog.close={L"完成",{}};
    window_.ShowDialog(std::move(dialog));
}

// 自动化冒烟（--smoke 且设置 LPDF_SIGNATURE_SMOKE=<输出 PDF>、LPDF_SIGNATURE_DIR=<签名库>）：
// 走真实的放置链路（ArmPlacement → 画布创建回调 Annotate → PlacePending → 后台任务），验证 Esc 取消，
// 打开手写签名对话框让自绘控件真实绘制，最后另存并关闭。失败时不写输出文件。
void Application::SignatureSmoke(const fs::path& output,int step){
    auto next=[this,output,step](float delay){window_.SetTimeout(delay,[this,output,step]{SignatureSmoke(output,step+1);});};
    auto fail=[this](const wchar_t* why){log::Info(std::wstring(L"签名冒烟失败：")+why);closing_=true;window_.Close();};
    if(!loaded_||busy_){window_.SetTimeout(.3f,[this,output,step]{SignatureSmoke(output,step);});return;}
    const auto& page=info_.pages[0];const float x=page.originX,y=page.originY;
    const size_t count=annotations_.size();
    switch(step){
    case 0:{
        Mode(1);
        try{
            const auto bitmap=RenderDrawnSignature({{{10,60},{40,20},{70,70},{100,25},{130,65}}},PadPen,InkColors[1]);
            StoreSignature(SignatureKind::Drawn,bitmap);
        }catch(...){fail(L"保存签名");return;}
        if(placement_.kind!=Placement::Signature||currentTool_!=Tool::Image){fail(L"未进入签名放置");return;}
        Annotate(0,Tool::Image,{x+200,y+200,160,100},{});
        return next(.2f);
    }
    case 1:
        if(annotations_.empty()||annotations_.back().type!=Tool::Image||placement_.kind!=Placement::None||currentTool_!=Tool::Select){fail(L"签名放置");return;}
        {Placement p;p.kind=Placement::Date;p.text=L"2026-09-28";ArmPlacement(std::move(p));}
        Annotate(0,Tool::Image,{x+200,y+300,160,100},{});
        return next(.2f);
    case 2:
        if(annotations_.empty()||annotations_.back().type!=Tool::Text||annotations_.back().text!=L"2026-09-28"){fail(L"日期放置");return;}
        {Placement p;p.kind=Placement::Check;ArmPlacement(std::move(p));}
        Annotate(0,Tool::Image,{x+200,y+350,160,100},{});
        return next(.2f);
    case 3:
        if(annotations_.empty()||annotations_.back().type!=Tool::Ink){fail(L"对勾放置");return;}
        {Placement p;p.kind=Placement::Cross;ArmPlacement(std::move(p));}
        Annotate(0,Tool::Image,{x+230,y+340,40,40},{});   // 拖框放置
        return next(.2f);
    case 4:{
        if(annotations_.empty()||annotations_.back().type!=Tool::Ink||annotations_.back().strokes.size()!=2){fail(L"叉号放置");return;}
        // Esc / 切换工具取消放置：之后再触发图片工具创建回调也不能放置标记。
        Placement p;p.kind=Placement::Check;ArmPlacement(std::move(p));
        ChooseTool(Tool::Select);
        if(placement_.kind!=Placement::None){fail(L"取消放置");return;}
        if(annotations_.size()!=count){fail(L"取消后仍然放置");return;}
        DrawSignature();
        return next(1.2f);
    }
    case 5:
        if(!window_.DialogActive()){fail(L"手写签名对话框未打开");return;}
        window_.CloseDialog();
        Task(L"保存冒烟结果",[output](Engine& e,const Cancel&){e.document.Save(output);},[this]{closing_=true;window_.Close();});
        return;
    default:return;
    }
}
}
