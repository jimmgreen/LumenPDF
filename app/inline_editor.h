#pragma once
#include "core/document.h"
#include <lumen/TextBox.h>
#include <lumen/Panel.h>
#include <windows.h>
#include <functional>
namespace lpdf {
// A page-attached LUMEN TextBox: no RichEdit HWND, separate font renderer or
// opaque native window. Display, caret, selection and IME share TextLayout.
class InlineEditor {
public:
    InlineEditor()=default;
    ~InlineEditor();
    void Attach(lumen::Panel& parent);
    void Begin(HWND owner,const Annotation&,std::function<void(bool)> finish);
    void Place(lumen::Rect bounds,lumen::Rect viewport,float scale,float dpi);
    void Style(float fontSize,const TextFormat&);
    void Opacity(float value);
    void CaretAt(lumen::Point clientPoint,float dpi);
    lumen::Size FittedSize(float maximumWidth)const;
    float RequiredHeight()const;
    void ScrollToTop();
    std::wstring Text()const;
    void End();
    void Focus();
    void UndoText();
    void RedoText();
    std::function<void()> save,changed;
    std::function<void(wchar_t)> format_shortcut;
    bool Active()const{return active_&&control_;}
    bool Composing()const{return control_&&control_->Composing();}
    lumen::TextBox* View()const{return control_.Get();}
    int Lines()const;
private:
    class EditorControl;
    void ApplyFont();
    lumen::WeakRef<lumen::TextBox> control_;
    lumen::WeakRef<lumen::Panel> parent_;
    HWND owner_{};
    bool active_{};
    float fontSize_{12},scale_{1},opacity_{1};
    TextFormat format_;
    std::function<void(bool)> finish_;
};
}
