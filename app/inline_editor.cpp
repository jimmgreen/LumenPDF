#include "inline_editor.h"
#include "text_layout.h"
#include <lumen/Window.h>
#include <algorithm>
#include <cmath>
namespace lpdf {
class InlineEditor::EditorControl final : public lumen::TextBox {
public:
    InlineEditor* editor{};
    using TextBox::Undo;
    using TextBox::Redo;
protected:
    bool OnKey(uint32_t key)override{
        if(!editor||Composing())return TextBox::OnKey(key);
        const bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0;
        if(key==VK_ESCAPE||(ctrl&&key==VK_RETURN)){
            auto finish=editor->finish_;if(finish)finish(key!=VK_ESCAPE);return true;
        }
        if(ctrl&&key=='S'){auto action=editor->save;if(action)action();return true;}
        if(ctrl&&(key=='B'||key=='I'||key=='U'||key=='L'||key=='E'||key=='R')){
            auto apply=editor->format_shortcut;if(apply)apply(static_cast<wchar_t>(key));return true;
        }
        return TextBox::OnKey(key);
    }
    bool AutomationSetValue(std::wstring_view value)override{
        if(!TextBox::AutomationSetValue(value))return false;
        if(editor&&editor->active_&&editor->changed)editor->changed();
        return true;
    }
    bool AcceptsTextDrop()const noexcept override{return false;}
};
InlineEditor::~InlineEditor(){if(control_)static_cast<EditorControl*>(control_.Get())->editor=nullptr;}
void InlineEditor::Attach(lumen::Panel& parent){
    if(control_)return;
    auto& field=parent.Add<EditorControl>();field.editor=this;
    field.Multiline(true).WordWrap(true).Chrome(false).ContentPadding(2).MaxLength(65536)
        .TextBackdrop(lumen::Color::Hex(0xffffff)).SelectionFill(lumen::Color::Hex(0x4799ee,.30f))
        .AccessibleName(L"页内文字编辑器").Visible(false);
    field.OnTextChanged([this](std::wstring_view){if(active_&&changed)changed();});
    field.OnComposingChanged([this](bool){if(active_&&changed)changed();});
    control_=&field;parent_=&parent;
}
void InlineEditor::Begin(HWND owner,const Annotation& a,std::function<void(bool)> finish){
    if(!control_)throw std::runtime_error("Inline text editor is not attached to the page canvas");
    owner_=owner;active_=false;fontSize_=a.fontSize;format_=a.textFormat;opacity_=a.opacity;finish_=std::move(finish);
    control_->ResetDocument(a.text);ApplyFont();control_->ReadOnly(false);active_=true;
}
void InlineEditor::ApplyFont(){
    if(!control_)return;
    Annotation a;a.fontSize=fontSize_;a.textFormat=format_;
    control_->Typography(AnnotationTypography(a,scale_)).ContentPadding(2.0f*scale_)
        .Foreground(lumen::Color::Hex(format_.color,opacity_));
}
void InlineEditor::Style(float size,const TextFormat& format){
    if(size==fontSize_&&format==format_)return;
    fontSize_=std::clamp(size,6.0f,96.0f);format_=format;ApplyFont();
}
void InlineEditor::Opacity(float value){opacity_=value;if(control_)control_->Foreground(lumen::Color::Hex(format_.color,opacity_));}
void InlineEditor::Place(lumen::Rect bounds,lumen::Rect viewport,float scale,float dpi){
    (void)dpi;
    if(!Active()||!parent_)return;
    if(std::abs(scale-scale_)>.0001f){scale_=scale;ApplyFont();}
    const auto origin=parent_->AbsoluteBounds();
    parent_->SetChildBounds(*control_,{bounds.x-origin.x,bounds.y-origin.y,bounds.w,bounds.h});
    control_->Visible(!bounds.Intersect(viewport).IsEmpty());
}
void InlineEditor::CaretAt(lumen::Point point,float dpi){
    (void)dpi;if(!Active())return;
    const auto r=control_->AbsoluteBounds();control_->PlaceCaret({point.x-r.x,point.y-r.y});
}
lumen::Size InlineEditor::FittedSize(float maximumWidth)const{
    if(!control_||!(scale_>0))return {};
    auto size=control_->ContentSize(std::max(.5f,(maximumWidth-4.0f)*scale_));
    return {size.w/scale_+4.0f,size.h/scale_+4.0f};
}
float InlineEditor::RequiredHeight()const{
    return control_&&scale_>0?control_->ContentSize().h/scale_+4.0f:0;
}
void InlineEditor::ScrollToTop(){if(control_)control_->ScrollToStart();}
std::wstring InlineEditor::Text()const{return control_?control_->Text():std::wstring{};}
int InlineEditor::Lines()const{return control_?static_cast<int>(control_->VisualLineCount()):1;}
void InlineEditor::End(){active_=false;finish_={};if(control_){control_->Visible(false);control_->ReadOnly(true);}}
void InlineEditor::Focus(){if(Active()){SetFocus(owner_);control_->Focus();}}
void InlineEditor::UndoText(){if(control_)static_cast<EditorControl*>(control_.Get())->Undo();}
void InlineEditor::RedoText(){if(control_)static_cast<EditorControl*>(control_.Get())->Redo();}
}
