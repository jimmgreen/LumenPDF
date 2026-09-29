#pragma once
#include "core/document.h"
#include <lumen/Window.h>
#include <windows.h>
#include <array>
#include <functional>
#include <memory>
namespace lumen { class ComboBox; class NumberBox; class ToggleButton; class Button; class Label; }
namespace lpdf {
// A modeless LUMEN tool window; only the page's text-input service is native.
// Formatting applies to the whole annotation, not a mixed-format selection.
class TextFormatBar {
public:
    ~TextFormatBar();
    void Begin(HWND,const Annotation&,std::function<void(TextFormat,float)> changed,
        std::function<void(bool)> finish,std::function<void()> focus,std::function<void()> fit);
    void Place(lumen::Rect,lumen::Rect,float dpi);
    bool CommitFields();
    void Sync(const Annotation&);
    void Overflow(bool);
    void Toggle(wchar_t);
    void End();
    HWND Handle()const{return window_?static_cast<HWND>(window_->NativeHandle()):nullptr;}
    lumen::Window* View()const{return window_.get();}
private:
    void Build(HWND);
    void State();
    void Message(std::wstring,bool error=false);
    void Apply(TextFormat,bool returnFocus);
    void ReturnFocus();
    void ChooseColor();
    void Finish(bool);
    HWND owner_{};
    std::unique_ptr<lumen::Window> window_;
    lumen::ComboBox* family_{};
    lumen::NumberBox* sizeField_{};
    std::array<lumen::ToggleButton*,3> styles_{},align_{};
    lumen::Button* color_{};
    lumen::Label* message_{};
    TextFormat format_;
    float size_{12};
    std::wstring sizeDisplay_;
    uint64_t session_{};
    bool active_{},syncing_{},committing_{},overflow_{},choosing_{},error_{};
    std::function<void(TextFormat,float)> changed_;
    std::function<void(bool)> finish_;
    std::function<void()> focus_,fit_;
};
}
