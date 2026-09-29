#include "application.h"
#include "ui.h"
#include <lumen/Dialog.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <sstream>

namespace lpdf {
using namespace lumen;
namespace {
std::wstring Name(Tool t){return ui::ToolName(t);}
std::wstring Number(float n){std::wostringstream s;s<<n;return s.str();}
std::wstring Hex(uint32_t rgb){wchar_t text[8]{};swprintf_s(text,L"#%06X",rgb&0xffffff);return text;}
float Numeric(const std::wstring& text,float lo,float hi){
    size_t end=0;const float value=std::stof(text,&end);
    while(end<text.size()&&std::iswspace(text[end]))++end;
    if(end!=text.size()||!std::isfinite(value)||value<lo||value>hi)throw std::runtime_error("range");
    return value;
}
uint32_t ParseColor(std::wstring text){
    if(!text.empty()&&text.front()==L'#')text.erase(text.begin());
    if(text.size()!=6||!std::all_of(text.begin(),text.end(),[](wchar_t c){return std::iswxdigit(c)!=0;}))throw std::runtime_error("color");
    return static_cast<uint32_t>(std::stoul(text,nullptr,16));
}
Button& Action(Panel& parent,std::wstring_view title,std::function<void()> fn,ButtonKind kind=ButtonKind::Standard){
    return parent.Add<Button>(title,kind).Height(34).Role(TextRole::Caption).SizeClass(ButtonSize::Small)
        .Glyph(ui::Glyph(title)).AccessibleName(title).OnClick(std::move(fn));
}
Column& Group(Panel& parent,std::wstring_view label){
    auto& group=parent.Add<Column>();group.Spacing(6);
    group.Add<Label>(label,TextRole::Caption).Secondary(true);return group;
}
constexpr int Endings[]{0,4,5};
ptrdiff_t EndingIndex(int ending){for(int i=0;i<3;++i)if(Endings[i]==ending)return i;return -1;}
}
void Application::BuildAnnotationProperties(){
    propLabel_=&properties_->Add<Label>(L"批注属性",TextRole::BodyStrong).MinSize({0,24});
    propEmpty_=&properties_->Add<Column>();propEmpty_->Spacing(14);propEmpty_->Padding(0,28);propEmpty_->AlignCross(CrossAlign::Center);
    propEmpty_->Add<IconView>(ui::Cursor).Box(48).IconSize(24).Background(Color::Hex(0x202020)).CornerRadius(12);
    propEmpty_->Add<Label>(L"选中批注，精细调整",TextRole::BodyStrong).MinSize({0,24});
    propEmpty_->Add<Label>(L"选择上方工具开始标记。\n颜色、线宽与透明度均可编辑。",TextRole::Caption).Wrap(true).Secondary(true).MinSize({208,52}).Alignment(Align::Center);
    propEditor_=&properties_->Add<Column>();propEditor_->Spacing(12);propEditor_->Visible(false);
    propHint_=&propEditor_->Add<Label>(L"",TextRole::Caption).Wrap(true).Secondary(true).MinSize({0,48});
    contentGroup_=&Group(*propEditor_,L"便签内容");
    content_=&contentGroup_->Add<TextBox>().Multiline(true).Role(TextRole::Caption).AccessibleName(L"便签内容").MinSize({0,116});
    editText_=&Action(*propEditor_,L"编辑文字",[this]{
        auto it=std::find_if(annotations_.begin(),annotations_.end(),[this](const auto& a){return a.id==annotation_;});
        if(it!=annotations_.end()){if(it->type==Tool::Text)BeginText(page_,*it);else if(it->type==Tool::Note)EditNote(page_,*it);else if(it->type==Tool::Stamp)EditStamp(page_,*it);}
    });
    colorGroup_=&Group(*propEditor_,L"颜色");
    auto& palette=colorGroup_->Add<Row>();palette.Spacing(6);
    for(auto rgb:{0xd33445u,0xf2a33cu,0xffd54au,0x36a56du,0x2864dcu,0x6d4fd3u}){
        auto& swatch=palette.Add<ui::ColorSwatch>(rgb);swatch.AccessibleName(L"颜色 "+Hex(rgb)).ToolTip(Hex(rgb));
        swatch.OnClick([this,rgb]{color_->Text(Hex(rgb));for(auto [button,value]:colorButtons_)static_cast<ui::ColorSwatch*>(button)->Active(value==rgb);});
        colorButtons_.push_back({&swatch,rgb});
    }
    auto& colorRow=colorGroup_->Add<Row>();colorRow.Spacing(8);
    color_=&colorRow.Add<TextBox>().Text(L"#D33445").Role(TextRole::Caption).AccessibleName(L"批注颜色 HEX").Grow();
    Action(colorRow,L"调色",[this]{PickAnnotationColor(false);},ButtonKind::Subtle).AccessibleName(L"选择批注颜色").ToolTip(L"选择自定义颜色");
    auto& dimensions=propEditor_->Add<Row>();dimensions.Spacing(12);
    fontGroup_=&Group(dimensions,L"字号 · pt");fontGroup_->Grow();
    size_=&fontGroup_->Add<TextBox>().Text(L"12").Role(TextRole::Caption).AccessibleName(L"批注字号");
    auto& alpha=Group(dimensions,L"不透明度 · %");alpha.Grow();
    opacity_=&alpha.Add<TextBox>().Text(L"100").Role(TextRole::Caption).AccessibleName(L"批注不透明度");
    strokeGroup_=&Group(*propEditor_,L"线宽 · pt / 线型");
    auto& strokeRow=strokeGroup_->Add<Row>();strokeRow.Spacing(8);
    lineWidth_=&strokeRow.Add<TextBox>().Text(L"1.5").Role(TextRole::Caption).AccessibleName(L"批注线宽").Grow();
    lineStyle_=&strokeRow.Add<ComboBox>().AddItems({L"实线",L"虚线"}).SelectedIndex(0).AccessibleName(L"批注线型").Grow();
    fillGroup_=&propEditor_->Add<Column>();fillGroup_->Spacing(6);
    fill_=&fillGroup_->Add<CheckBox>(L"填充矩形");
    auto& fillRow=fillGroup_->Add<Row>();fillRow.Spacing(8);
    fillColor_=&fillRow.Add<TextBox>().Text(L"#FFE3E5").Role(TextRole::Caption).AccessibleName(L"填充颜色 HEX").Grow();
    Action(fillRow,L"调色",[this]{PickAnnotationColor(true);},ButtonKind::Subtle).AccessibleName(L"选择填充颜色").ToolTip(L"选择填充颜色");
    arrowGroup_=&propEditor_->Add<Column>();arrowGroup_->Spacing(6);
    auto& start=Group(*arrowGroup_,L"起点 / 终点");
    auto& ends=start.Add<Row>();ends.Spacing(8);
    startEnding_=&ends.Add<ComboBox>().AddItems({L"无箭头",L"开放箭头",L"实心箭头"}).SelectedIndex(0).AccessibleName(L"箭头起点").Grow();
    endEnding_=&ends.Add<ComboBox>().AddItems({L"无箭头",L"开放箭头",L"实心箭头"}).SelectedIndex(1).AccessibleName(L"箭头终点").Grow();
    applyProps_=&Action(*propEditor_,L"应用修改",[this]{Props();});
    imageRotate_=&Action(*propEditor_,L"图片旋转 90°",[this]{
        auto it=std::find_if(annotations_.begin(),annotations_.end(),[this](const auto& a){return a.id==annotation_;});
        if(it==annotations_.end()||it->type!=Tool::Image||it->readOnly)return;
        auto a=*it;const float cx=a.bounds.x+a.bounds.w/2,cy=a.bounds.y+a.bounds.h/2;
        a.rotation=(a.rotation+90)%360;std::swap(a.bounds.w,a.bounds.h);
        const auto& page=info_.pages[page_];
        const float factor=std::min({1.0f,page.width/a.bounds.w,page.height/a.bounds.h});a.bounds.w*=factor;a.bounds.h*=factor;
        a.bounds.x=std::clamp(cx-a.bounds.w/2,page.originX,page.originX+page.width-a.bounds.w);
        a.bounds.y=std::clamp(cy-a.bounds.h/2,page.originY,page.originY+page.height-a.bounds.h);
        const int p=page_;Task(L"旋转图片",[p,a](Engine& e,const Cancel&){e.document.UpdateAnnotation(p,a);});
    });
    deleteText_=&Action(*propEditor_,L"删除批注",[this]{DeleteAnnotation(page_,annotation_);},ButtonKind::Subtle);
    properties_->Add<Label>(L"拖动移动 · 控制点调整\nDelete 删除 · Ctrl+Z 撤销\nEsc 取消绘制 / 返回选择",TextRole::Caption).Wrap(true).Secondary(true).MinSize({0,60}).Margin(0,12);
}
void Application::ShowProperties(const Annotation& a,bool existing){
    const bool text=a.type==Tool::Text,note=a.type==Tool::Note,image=a.type==Tool::Image,stamp=a.type==Tool::Stamp;
    propEditor_->Visible(true);propEmpty_->Visible(false);
    propLabel_->Text(Name(a.type)+(existing?L"属性":L" · 新建属性"));
    const wchar_t* hint=L"颜色、线宽按 PDF 页面单位保存。\n同类工具沿用最近一次应用的属性。";
    if(note)hint=L"双击便签图标编辑多行评论。\n取消编辑不会更改文档。";
    if(text)hint=L"单击选中，双击进入文字编辑。\n输入和改字号时，文字框自动排版。\n拖边框移动；拖控制点调整尺寸。";
    if(image)hint=L"保持图片原始比例与透明背景。\n拖动四角等比缩放，可旋转 90°。";
    if(a.type==Tool::Highlight)hint=L"沿文字拖动，逐行贴合文本。\nCtrl 拖动可标记扫描件或图片区域。";
    if(a.type==Tool::Arrow)hint=L"拖动两个端点精确调整方向。\nShift 约束到水平、垂直或 45°。";
    if(a.type==Tool::Ink)hint=L"圆头、圆角与平滑路径。\n连续绘制，Esc 返回选择。";
    if(a.type==Tool::Underline||a.type==Tool::StrikeOut)hint=L"沿文字拖动，逐行贴合文本。\n颜色与不透明度可随时修改。";
    if(a.type==Tool::Ellipse)hint=L"拖动绘制；Shift 保持正圆。\n可设置线宽、虚线与填充。";
    if(a.type==Tool::Line)hint=L"拖动两个端点精确调整。\nShift 约束到水平、垂直或 45°。";
    if(stamp)hint=L"点击放置或拖框确定大小。\n双击或“编辑印章”修改文字；支持中文。";
    propHint_->Text(a.readOnly?L"此批注被锁定，不能修改。":hint);
    fontGroup_->Visible(text&&!textEditor_.Active());size_->Enabled(text);
    contentGroup_->Visible(note&&existing);content_->Text(a.text);
    editText_->Visible(existing&&(text||note||stamp)&&!textEditor_.Active());
    const wchar_t* editLabel=note?L"编辑便签":stamp?L"编辑印章":L"编辑文字";
    editText_->Text(editLabel).AccessibleName(editLabel);
    colorGroup_->Visible(!text&&!image);
    strokeGroup_->Visible(IsStrokedTool(a.type));
    fillGroup_->Visible(IsBoxShape(a.type));if(IsBoxShape(a.type)){const auto label=a.type==Tool::Ellipse?L"填充椭圆":L"填充矩形";fill_->Text(label);fill_->AccessibleName(label);}arrowGroup_->Visible(a.type==Tool::Arrow);
    imageRotate_->Visible(image&&existing);deleteText_->Visible(existing);deleteText_->Enabled(!a.readOnly&&!textEditor_.Active());
    applyProps_->Text(existing||textEditor_.Active()?L"应用修改":L"设为新建属性").AccessibleName(existing||textEditor_.Active()?L"应用修改":L"设为新建属性");
    applyProps_->Enabled(!a.readOnly);editText_->Enabled(!a.readOnly);imageRotate_->Enabled(!a.readOnly);
    size_->Text(Number(a.fontSize));opacity_->Text(Number(a.opacity*100));
    color_->Text(Hex(a.style.color));fillColor_->Text(Hex(a.style.fillColor));lineWidth_->Text(Number(a.style.lineWidth));
    lineStyle_->SelectedIndex(a.style.dashed?1:0);fill_->Checked(a.style.filled);
    startEnding_->SelectedIndex(EndingIndex(a.style.startEnding));endEnding_->SelectedIndex(EndingIndex(a.style.endEnding));
    for(auto [button,rgb]:colorButtons_)static_cast<ui::ColorSwatch*>(button)->Active(rgb==a.style.color);
}
void Application::Select(int p,int id){
    page_=p;annotation_=id;
    auto it=std::find_if(annotations_.begin(),annotations_.end(),[id](const auto& a){return a.id==id&&a.type!=Tool::Select;});
    if(it!=annotations_.end()){ShowProperties(*it,true);return;}
    annotation_=-1;
    if(currentTool_!=Tool::Select){
        Annotation a;a.type=currentTool_;a.id=-1;a.style=toolStyles_[static_cast<size_t>(a.type)];a.opacity=toolOpacities_[static_cast<size_t>(a.type)];a.fontSize=defaultFont_;
        ShowProperties(a,false);
    }else{propEditor_->Visible(false);propEmpty_->Visible(true);propLabel_->Text(L"批注属性");}
}
void Application::PickAnnotationColor(bool fill){
    static COLORREF custom[16]{};
    uint32_t rgb=0xd33445;try{rgb=ParseColor((fill?fillColor_:color_)->Text());}catch(...){}
    CHOOSECOLORW chooser{};chooser.lStructSize=sizeof(chooser);chooser.hwndOwner=static_cast<HWND>(window_.NativeHandle());
    chooser.rgbResult=RGB((rgb>>16)&255,(rgb>>8)&255,rgb&255);chooser.lpCustColors=custom;chooser.Flags=CC_FULLOPEN|CC_RGBINIT;
    if(!ChooseColorW(&chooser))return;
    rgb=(GetRValue(chooser.rgbResult)<<16)|(GetGValue(chooser.rgbResult)<<8)|GetBValue(chooser.rgbResult);
    (fill?fillColor_:color_)->Text(Hex(rgb));
    if(!fill)for(auto [button,value]:colorButtons_)static_cast<ui::ColorSwatch*>(button)->Active(value==rgb);
}
void Application::Props(){
    if(busy_)return;
    if(textEditor_.Active()&&textDraft_){
        try{
            const float font=Numeric(size_->Text(),6,96),opacity=Numeric(opacity_->Text(),5,100)/100;
            textDraft_->fontSize=font;textDraft_->opacity=opacity;
            textSync_=true;textEditor_.Style(textDraft_->fontSize,textDraft_->textFormat);textEditor_.Opacity(opacity);textSync_=false;
            textFormatBar_.Sync(*textDraft_);TextChanged();textEditor_.Focus();
        }catch(...){status_->Text(L"字号应为 6–96，不透明度应为 5–100；草稿已保留");textEditor_.Focus();}
        return;
    }
    auto it=std::find_if(annotations_.begin(),annotations_.end(),[this](const auto& a){return a.id==annotation_;});
    if(it==annotations_.end()&&currentTool_==Tool::Select)return;
    try{
        Annotation a;
        if(it!=annotations_.end()){a=*it;if(a.readOnly)return;}
        else{a.type=currentTool_;a.id=-1;a.style=toolStyles_[static_cast<size_t>(a.type)];}
        a.opacity=Numeric(opacity_->Text(),5,100)/100;
        if(a.type==Tool::Text)a.fontSize=Numeric(size_->Text(),6,96);
        if(a.type==Tool::Note&&it!=annotations_.end())a.text=content_->Text();
        if(a.type!=Tool::Text&&a.type!=Tool::Image)a.style.color=ParseColor(color_->Text());
        if(IsStrokedTool(a.type)){a.style.lineWidth=Numeric(lineWidth_->Text(),.25f,24);a.style.dashed=lineStyle_->SelectedIndex()==1;}
        if(IsBoxShape(a.type)){a.style.filled=fill_->Checked();a.style.fillColor=ParseColor(fillColor_->Text());}
        if(a.type==Tool::Arrow){
            if(startEnding_->SelectedIndex()>=0)a.style.startEnding=Endings[startEnding_->SelectedIndex()];
            if(endEnding_->SelectedIndex()>=0)a.style.endEnding=Endings[endEnding_->SelectedIndex()];
        }
        toolStyles_[static_cast<size_t>(a.type)]=a.style;toolOpacities_[static_cast<size_t>(a.type)]=a.opacity;
        if(a.type==Tool::Text)defaultFont_=a.fontSize;
        if(currentTool_==a.type)canvas_->DrawingStyle(a.style,a.opacity);
        if(it==annotations_.end()){status_->Text(L"已设置 "+Name(a.type)+L" 的新建属性");return;}
        if(a.style==it->style&&a.opacity==it->opacity&&a.fontSize==it->fontSize&&a.text==it->text){status_->Text(L"属性未变化");return;}
        const int p=page_;Task(L"修改批注属性",[p,a](Engine& e,const Cancel&){e.document.UpdateAnnotation(p,a);});
    }catch(...){window_.Alert(L"属性格式不正确",L"颜色用六位 HEX；线宽 0.25–24 pt，字号 6–96 pt，不透明度 5–100%。");}
}
void Application::DeleteAnnotation(int p,int id){
    if(id<0||busy_||textEditor_.Active())return;
    auto it=std::find_if(annotations_.begin(),annotations_.end(),[id](const auto& a){return a.id==id;});
    if(it==annotations_.end()||it->readOnly)return;
    annotation_=-1;Task(L"删除批注",[p,id](Engine& e,const Cancel&){e.document.DeleteAnnotation(p,id);});
}
void Application::SelectCreated(int p,bool keepTool){
    annotation_=annotations_.empty()?-1:annotations_.back().id;
    if(!keepTool)ChooseTool(Tool::Select);
    Select(p,annotation_);canvas_->Selection(p,annotations_,annotation_);canvas_->Focus();
}
void Application::EditNote(int p,Annotation a,bool retry){
    if(busy_||a.readOnly||window_.DialogActive())return;
    // Read the actual control at commit, not an OnTextChanged mirror: UIA,
    // paste and programmatic updates must be as durable as ordinary typing.
    auto editor=std::make_shared<TextBox*>(nullptr);
    DialogSpec dialog;dialog.title=a.id<0?L"添加便签":L"编辑便签";
    dialog.message=L"在此输入评论，可换行。保存后可继续移动、改色或撤销。";
    dialog.size=DialogSize::Standard;dialog.default_button=DialogCommand::None;dialog.cancel_button=DialogCommand::Close;
    dialog.content=[editor,text=a.text](Panel& panel){
        *editor=&panel.Add<TextBox>().Multiline(true).Text(text).Placeholder(L"写下需要确认或修改的内容…")
            .AccessibleName(L"便签编辑内容").MinSize({0,168});
    };
    dialog.primary={L"保存便签",{}};dialog.close={L"取消",{}};
    dialog.on_result=[this,p,a,editor,retry](DialogResult result)mutable{
        if(result!=DialogResult::Primary){status_->Text(L"已取消便签编辑，文档未更改");return;}
        const auto text=(*editor)->Text();
        if(a.id<0&&text.find_first_not_of(L" \t\r\n")==std::wstring::npos){status_->Text(L"空便签未加入文档");return;}
        if(a.id>=0&&text==a.text&&!retry){status_->Text(L"便签内容未变化");return;}
        a.text=text;
        Task(L"保存便签",[p,a](Engine& e,const Cancel&){
            if(a.id<0)e.document.AddAnnotation(p,Tool::Note,a.bounds,a.text,{},{},12,a.opacity,a.style);
            else e.document.UpdateAnnotation(p,a);
        },[this,p,a]{if(a.id<0)SelectCreated(p,false);else{annotation_=a.id;Select(p,a.id);canvas_->Selection(p,annotations_,a.id);}},
        {},[this,p,a](std::wstring error){EditNote(p,a,true);status_->Text(L"未能保存，便签草稿已保留："+error);});
    };
    window_.ShowDialog(std::move(dialog));
}
void Application::EditStamp(int p,Annotation a){
    if(busy_||a.readOnly||window_.DialogActive())return;
    struct Controls{ComboBox* preset{};TextBox* text{};};
    auto c=std::make_shared<Controls>();
    static const wchar_t* presets[]={L"已批准",L"已审核",L"机密",L"草稿",L"作废",L"已完成",L"仅供参考",L"加急"};
    static const uint32_t colors[]={0x2e8b57,0x2864dc,0xd33445,0x6f6f6f,0xd33445,0x2e8b57,0x2864dc,0xe07b00};
    DialogSpec dialog;dialog.title=a.id<0?L"添加印章":L"编辑印章";
    dialog.message=L"选择常用印章或输入自定义文字（最多 40 字）。印章是标准 PDF 批注，可移动、缩放、改色和撤销。";
    dialog.size=DialogSize::Standard;dialog.default_button=DialogCommand::Primary;
    dialog.content=[c,text=a.text](Panel& panel){
        c->preset=&panel.Add<ComboBox>();c->preset->AccessibleName(L"常用印章");
        for(auto* name:presets)c->preset->AddItem(name);
        c->preset->AddItem(L"自定义…");
        ptrdiff_t index=text.empty()?0:static_cast<ptrdiff_t>(std::size(presets));
        for(size_t i=0;i<std::size(presets);++i)if(text==presets[i])index=static_cast<ptrdiff_t>(i);
        c->preset->SelectedIndex(index);
        c->text=&panel.Add<TextBox>().Text(text.empty()?presets[0]:text).Placeholder(L"印章文字").AccessibleName(L"印章文字");
        c->preset->OnSelectionChanged([c](ptrdiff_t,ptrdiff_t now){
            if(now>=0&&now<static_cast<ptrdiff_t>(std::size(presets)))c->text->Text(presets[now]);else c->text->Focus();
        });
    };
    dialog.primary={a.id<0?L"放置印章":L"保存印章",{}};dialog.close={L"取消",{}};
    dialog.on_result=[this,p,a,c](DialogResult result)mutable{
        if(result!=DialogResult::Primary){status_->Text(L"已取消印章，文档未更改");return;}
        auto text=c->text->Text();
        while(!text.empty()&&iswspace(text.back()))text.pop_back();
        while(!text.empty()&&iswspace(text.front()))text.erase(text.begin());
        if(text.empty()||text.size()>40){window_.Alert(L"印章文字无效",L"请输入 1–40 个字符。");return;}
        const auto preset=c->preset->SelectedIndex();
        // 新建时，常用印章自带配色（例如“机密”为红色）；编辑时保留现有颜色。
        if(a.id<0&&preset>=0&&preset<static_cast<ptrdiff_t>(std::size(presets))&&text==presets[preset])a.style.color=colors[preset];
        if(a.id>=0&&text==a.text){status_->Text(L"印章内容未变化");return;}
        a.text=text;
        Task(a.id<0?L"添加印章":L"修改印章",[p,a](Engine& e,const Cancel&){
            if(a.id<0)e.document.AddAnnotation(p,Tool::Stamp,a.bounds,a.text,{},{},12,a.opacity,a.style);
            else e.document.UpdateAnnotation(p,a);
        },[this,p,a]{if(a.id<0)SelectCreated(p,false);else{annotation_=a.id;Select(p,a.id);canvas_->Selection(p,annotations_,a.id);}});
    };
    window_.ShowDialog(std::move(dialog));
}
void Application::Annotate(int p,Tool tool,Rect r,std::vector<Point> points){
    if(!loaded_||busy_||tool==Tool::Select)return;
    if(tool==Tool::Image&&PlacePending(p,r))return;
    Annotation a;a.id=-1;a.type=tool;a.bounds=r;a.fontSize=defaultFont_;
    a.style=toolStyles_[static_cast<size_t>(tool)];a.opacity=toolOpacities_[static_cast<size_t>(tool)];
    if(tool==Tool::Text){a.textFormat=defaultTextFormat_;a.textSizing=points.empty()?TextSizing::Auto:TextSizing::Width;a.fixedTextBox=true;a.lumenText=true;BeginText(p,a);return;}
    if(tool==Tool::Note){EditNote(p,a);return;}
    if(tool==Tool::Stamp){EditStamp(p,a);return;}
    fs::path image;
    if(tool==Tool::Image){
        static constexpr wchar_t filter[]=L"图片\0*.png;*.jpg;*.jpeg\0";
        auto files=Pick(false,filter);if(files.empty()){status_->Text(L"已取消图片放置，文档未更改");return;}image=files.front();
        const auto& page=info_.pages[p];r.w=std::min(r.w,page.width);r.h=std::min(r.h,page.height);
        r.x=std::clamp(r.x,page.originX,page.originX+page.width-r.w);r.y=std::clamp(r.y,page.originY,page.originY+page.height-r.h);
    }
    Task(L"添加"+Name(tool),[p,tool,r,a,image,points=std::move(points)](Engine& e,const Cancel&){
        e.document.AddAnnotation(p,tool,r,{},image,points,12,a.opacity,a.style,IsMarkupTool(tool)&&!points.empty());
    },[this,p,tool]{SelectCreated(p,tool==Tool::Ink||IsMarkupTool(tool));});
}
}