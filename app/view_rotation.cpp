// 临时视图旋转：只改变阅读视图的显示方向（扫描件方向不对时），不修改文件；每个标签页各自记住。
// 需要永久生效时可「按此方向旋转页面（写入文件）」，走普通的可撤销页面旋转。
#include "application.h"
#include "ui.h"
#include <chrono>
#include <cstdio>
#include <numeric>
#include <thread>

namespace lpdf {
using namespace lumen;

void Application::RotateView(int delta){
    if(!loaded_||home_||mode_==3||window_.DialogActive())return;
    if(textEditor_.Active()){FinishText(true,[this,delta]{RotateView(delta);});return;}
    if(mode_==2)Mode(0);   // 页面视图显示真实方向；旋转视图在阅读 / 批注视图中进行
    const int next=delta==0?0:((canvas_->ViewRotation()+delta)%360+360)%360;
    canvas_->ViewRotation(next);
    status_->Text(next?L"视图已旋转 "+std::to_wstring(next)+L"°（只影响显示，不修改文件）  ·  Ctrl+Shift+= / Ctrl+Shift+- 继续旋转，右键可恢复"
                      :std::wstring(L"已恢复视图方向"));
}

void Application::ApplyViewRotation(){
    const int degrees=canvas_->ViewRotation();
    if(!degrees||!loaded_||busy_||info_.pages.empty())return;
    std::vector<int> pages(info_.pages.size());std::iota(pages.begin(),pages.end(),0);
    // 页面真正旋转后视图归零，画面方向保持不变。
    Task(L"旋转页面",[pages,degrees](Engine& e,const Cancel&){e.document.RotatePages(pages,degrees);},[this,degrees]{
        canvas_->ViewRotation(0);
        status_->Text(L"已把全部 "+std::to_wstring(info_.pages.size())+L" 页旋转 "+std::to_wstring(degrees)+L"°（写入文件，保存后生效）  ·  Ctrl+Z 撤销");
    });
}

// 视图旋转冒烟：LPDF_ROTATE_SMOKE 指定输出目录。LPDF_ROTATE_HOLD=1 时只旋转 90° 并停住（供截图）。
void Application::RotationSmoke(const fs::path& dir,int step){
    auto report=[dir](const std::wstring& line){FILE* f=nullptr;if(_wfopen_s(&f,(dir/L"report.txt").c_str(),L"a, ccs=UTF-8")==0&&f){fwprintf(f,L"%ls\n",line.c_str());fclose(f);}};
    auto later=[this,dir](float delay,int next){
        auto post=window_.Dispatcher();auto alive=alive_;
        std::thread([post,alive,delay,next,this,dir]{std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(delay*1000)));post.Post([alive,next,this,dir]{if(alive->load())RotationSmoke(dir,next);});}).detach();
    };
    if(!loaded_||busy_){later(.3f,step);return;}
    auto size=[](const PageInfo& p){return std::to_wstring(std::lround(p.width))+L"x"+std::to_wstring(std::lround(p.height));};
    wchar_t hold[4]{};const bool holding=GetEnvironmentVariableW(L"LPDF_ROTATE_HOLD",hold,4)>0;
    switch(step){
    case 0:
        Mode(0);report(L"start.size="+size(info_.pages[0]));
        RotateView(90);
        report(L"rotation="+std::to_wstring(canvas_->ViewRotation()));
        report(L"zoom="+std::wstring(zoomButton_->Text()));
        report(L"dirty="+std::wstring(info_.dirty?L"1":L"0"));
        if(holding)return;
        Mode(2);report(L"grid.rotation="+std::to_wstring(canvas_->ViewRotation()));
        Mode(0);report(L"reading.rotation="+std::to_wstring(canvas_->ViewRotation()));
        RotateView(90);RotateView(90);RotateView(90);report(L"full.turn="+std::to_wstring(canvas_->ViewRotation()));
        RotateView(-90);report(L"ccw="+std::to_wstring(canvas_->ViewRotation()));
        RotateView(0);report(L"reset="+std::to_wstring(canvas_->ViewRotation()));
        RotateView(90);ApplyViewRotation();
        return later(.6f,1);
    case 1:
        report(L"applied.rotation="+std::to_wstring(canvas_->ViewRotation()));
        report(L"applied.size="+size(info_.pages[0]));
        report(L"applied.undo="+std::wstring(info_.canUndo?L"1":L"0"));
        report(L"applied.dirty="+std::wstring(info_.dirty?L"1":L"0"));
        Task(L"保存测试副本",[out=dir/L"rotated.pdf"](Engine& e,const Cancel&){e.document.Save(out);},[this,dir,report]{
            report(L"done=1");closing_=true;window_.Close();
        });
        return;
    default:return;
    }
}
}
