// 框选放大（Z）与放大镜（L）：看图纸、小字表格时直接放大到某个区域，或用跟随指针的放大镜查看细节。
// 两者都只改变显示；逻辑在 PdfCanvas 中，这里负责快捷键 / 菜单入口、状态栏提示与冒烟测试。
#include "application.h"
#include "ui.h"
#include <chrono>
#include <cstdio>
#include <thread>

namespace lpdf {
using namespace lumen;

namespace {
std::wstring Factor(float f){wchar_t text[16]{};swprintf_s(text,L"%.1f",f);std::wstring s=text;if(s.size()>2&&s.ends_with(L".0"))s.resize(s.size()-2);return s+L"×";}
}

bool Application::ZoomToolsAvailable()const{return loaded_&&!home_&&mode_!=3&&!presenting_&&!window_.DialogActive();}

void Application::ToggleZoomBox(){
    if(!ZoomToolsAvailable())return;
    if(textEditor_.Active()){FinishText(true,[this]{ToggleZoomBox();});return;}
    if(mode_==2)Mode(0);   // 框选放大在阅读 / 批注视图中进行
    canvas_->ZoomBox(!canvas_->ZoomBoxActive());canvas_->Focus();
}

void Application::ZoomBackKey(){
    if(!ZoomToolsAvailable())return;
    if(!canvas_->ZoomBack())status_->Text(L"没有可返回的框选缩放");
    else status_->Text(L"已返回框选前的缩放");
}

void Application::ToggleMagnifier(){
    if(!ZoomToolsAvailable())return;
    if(mode_==2)Mode(0);
    canvas_->Magnifier(!canvas_->MagnifierActive());canvas_->Focus();
}

std::wstring Application::MagnifierHint()const{
    return L"放大镜 "+Factor(canvas_->MagnifierFactor())+L"：移动鼠标查看细节（工具照常可用）  ·  Alt+滚轮调整倍率  ·  L 或 Esc 关闭";
}

void Application::WireZoomTools(){
    canvas_->zoom_box_changed=[this](bool on){status_->Text(on?L"框选放大：在页面上拖一个框放大到该区域（单击 = 放大 2 倍）  ·  Esc 取消":L"");};
    canvas_->box_zoomed=[this](float zoom){status_->Text(L"已放大到 "+std::to_wstring(static_cast<int>(std::lround(zoom*100)))+L"%  ·  Shift+Z 返回之前的缩放");};
    canvas_->magnifier_changed=[this](bool on){status_->Text(on?MagnifierHint():std::wstring(L"放大镜已关闭"));};
}

// 冒烟：LPDF_ZOOM_SMOKE 指定输出目录。LPDF_ZOOM_HOLD=box 放大后停住，=lens 打开放大镜后停住（供截图）。
void Application::ZoomSmoke(const fs::path& dir,int step){
    auto report=[dir](const std::wstring& line){FILE* f=nullptr;if(_wfopen_s(&f,(dir/L"report.txt").c_str(),L"a, ccs=UTF-8")==0&&f){fwprintf(f,L"%ls\n",line.c_str());fclose(f);}};
    auto later=[this,dir](float delay,int next){
        auto post=window_.Dispatcher();auto alive=alive_;
        std::thread([post,alive,delay,next,this,dir]{std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(delay*1000)));post.Post([alive,next,this,dir]{if(alive->load())ZoomSmoke(dir,next);});}).detach();
    };
    if(!loaded_||busy_){later(.3f,step);return;}
    wchar_t hold[8]{};GetEnvironmentVariableW(L"LPDF_ZOOM_HOLD",hold,8);const std::wstring holding=hold;
    auto pct=[](float z){return std::to_wstring(static_cast<int>(std::lround(z*100)));};
    auto has=[this](const wchar_t* text){return std::wstring(status_->Text()).find(text)!=std::wstring::npos?L"1":L"0";};
    switch(step){
    case 0:{
        Mode(0);canvas_->FitPage();
        const float start=canvas_->ZoomValue();report(L"start.zoom="+pct(start));
        ToggleZoomBox();
        report(L"box.active="+std::wstring(canvas_->ZoomBoxActive()?L"1":L"0"));report(L"box.hint="+std::wstring(has(L"框选放大")));
        // 与松开鼠标相同的顺序：先退出框选模式，再放大到框。
        canvas_->ZoomBox(false);canvas_->ZoomToRect({220,160,160,110});
        report(L"box.after="+pct(canvas_->ZoomValue()));
        report(L"box.larger="+std::wstring(canvas_->ZoomValue()>start*2?L"1":L"0"));
        report(L"box.button="+std::wstring(zoomButton_->Text()));
        report(L"box.status="+std::wstring(has(L"Shift+Z")));
        report(L"box.back="+std::wstring(canvas_->CanZoomBack()?L"1":L"0"));
        if(holding==L"box")return;
        ZoomBackKey();
        report(L"back.zoom="+pct(canvas_->ZoomValue()));report(L"back.same="+std::wstring(std::abs(canvas_->ZoomValue()-start)<.002f?L"1":L"0"));
        ZoomBackKey();report(L"back.none="+std::wstring(has(L"没有可返回")));
        ToggleMagnifier();
        report(L"lens.on="+std::wstring(canvas_->MagnifierActive()?L"1":L"0"));report(L"lens.hint="+std::wstring(has(L"放大镜 2.5×")));
        canvas_->MagnifierAt({420,300});
        if(holding==L"lens")return;
        return later(1.2f,1);
    }
    case 1:
        report(L"lens.rendered="+std::wstring(canvas_->MagnifierRendered()?L"1":L"0"));
        canvas_->MagnifierFactor(4);report(L"lens.factor="+std::wstring(has(L"放大镜 4×")));
        ToggleMagnifier();report(L"lens.off="+std::wstring(canvas_->MagnifierActive()?L"0":L"1"));
        report(L"dirty="+std::wstring(info_.dirty?L"1":L"0"));
        report(L"done=1");closing_=true;window_.Close();
        return;
    default:return;
    }
}
}
