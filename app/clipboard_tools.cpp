// 剪贴板粘贴：截图后 Ctrl+V —— 无文档（或在主页）时直接新建 PDF；阅读 / 批注视图粘贴为图片批注（放在视图中央并选中，
// 可直接拖动 / 调整大小）；页面视图插入为新页。复制的 PDF 文件直接打开。文本框有焦点时 Ctrl+V 照常粘贴文字。
#include "application.h"
#include "clipboard_image.h"
#include "ui.h"
#include <lumen/TextBox.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace lpdf {
using namespace lumen;
namespace {
fs::path ClipboardTempDir(){
    wchar_t temp[MAX_PATH]{};const DWORD n=GetTempPathW(MAX_PATH,temp);
    return (n?fs::path(temp):fs::temp_directory_path())/L"LumenPDF"/L"clipboard";
}
}

bool Application::PasteShortcut(){
    // 文本框 / 行内文字编辑 / 对话框中：交给它们处理（粘贴文字）。
    if(window_.DialogActive()||textEditor_.Active()||dynamic_cast<TextBox*>(window_.Focused()))return false;
    PasteFromClipboard(PasteIntent::Auto);return true;
}

void Application::PasteFromClipboard(PasteIntent requested){
    if(busy_||window_.DialogActive())return;
    if(textEditor_.Active()){FinishText(true,[this,requested]{PasteFromClipboard(requested);});return;}
    ClipboardContent clip;std::wstring error;
    if(!ReadClipboard(static_cast<HWND>(window_.NativeHandle()),ClipboardTempDir(),clip,error)){
        if(!error.empty())Fail(error);
        else status_->Text(L"剪贴板中没有图片。先截图（Win+Shift+S）或复制图片，再按 Ctrl+V。");
        return;
    }
    if(clip.image.empty()){   // 只复制了 PDF 文件：打开
        status_->Text(L"正在打开剪贴板中的 "+std::to_wstring(clip.pdfs.size())+L" 个 PDF…");
        OpenFiles(clip.pdfs);return;
    }
    const auto file=clip.image;const bool temporary=clip.temporary;
    auto cleanup=[file,temporary]{if(temporary){std::error_code e;fs::remove(file,e);}};
    auto failed=[this,cleanup](std::wstring message){cleanup();Fail(L"无法粘贴图片："+message);};
    const auto intent=ResolvePasteIntent(requested,loaded_,home_,mode_);
    if(intent==PasteIntent::NewDocument){
        if(!BeginDocument()){cleanup();return;}
        Task(L"从剪贴板新建 PDF",[file](Engine& e,const Cancel&){e.document.NewFromImage(file);e.open=true;},
            [this,cleanup]{cleanup();name_->Text(L"剪贴板图片.pdf");home_=false;Mode(0);canvas_->FitPage();UpdateTitle();
                status_->Text(L"已从剪贴板新建 PDF（尚未保存）  ·  Ctrl+S 保存");},{},failed);
        return;
    }
    if(!loaded_||info_.pages.empty()){cleanup();return;}
    if(intent==PasteIntent::NewPage){
        int after=page_;
        if(mode_==2){const auto pages=SelectedPages();if(!pages.empty())after=*std::max_element(pages.begin(),pages.end());}
        after=std::clamp(after,-1,static_cast<int>(info_.pages.size())-1);
        Task(L"插入图片页",[file,after](Engine& e,const Cancel&){e.document.InsertImagePage(after,file);},
            [this,cleanup,after]{cleanup();Page(after+1,true);status_->Text(L"已在第 "+std::to_wstring(after+1)+L" 页后插入剪贴板图片  ·  Ctrl+Z 撤销");},{},failed);
        return;
    }
    // 图片批注：放在当前页的视图中央，最大占页面 60%，不放大超过 96 DPI 原尺寸。
    const int p=std::clamp(page_,0,static_cast<int>(info_.pages.size())-1);
    const auto page=info_.pages[static_cast<size_t>(p)];
    Point center=canvas_->ViewCenter(p);
    center.x=std::clamp(center.x,page.originX,page.originX+page.width);center.y=std::clamp(center.y,page.originY,page.originY+page.height);
    const float opacity=toolOpacities_[static_cast<size_t>(Tool::Image)];const auto style=toolStyles_[static_cast<size_t>(Tool::Image)];
    Task(L"粘贴图片",[file,p,page,center,opacity,style](Engine& e,const Cancel&){
        const auto [iw,ih]=Document::ImageSize(file);
        if(iw<=0||ih<=0)throw std::runtime_error("Invalid image dimensions");
        float w=iw*0.75f,h=ih*0.75f;const float k=std::min({1.0f,page.width*0.6f/w,page.height*0.6f/h});w=std::max(4.0f,w*k);h=std::max(4.0f,h*k);
        Rect box{center.x-w/2,center.y-h/2,w,h};
        box.x=std::clamp(box.x,page.originX,page.originX+page.width-box.w);box.y=std::clamp(box.y,page.originY,page.originY+page.height-box.h);
        e.document.AddAnnotation(p,Tool::Image,box,{},file,{},12,opacity,style);
    },[this,cleanup,p]{cleanup();if(mode_!=1&&mode_!=0)Mode(0);SelectCreated(p,false);
        status_->Text(L"已粘贴图片  ·  拖动移动，拖动控制点调整大小，Delete 删除，Ctrl+Z 撤销");},{},failed);
}

// 剪贴板冒烟：LPDF_CLIPBOARD_SMOKE 指定输出目录（测试程序已把位图放到剪贴板）。
// 阅读视图粘贴 → 图片批注；页面视图粘贴 → 新页；另存 pasted.pdf；显式新建 → 新文档，另存 new.pdf；写 report.txt 后退出。
void Application::ClipboardSmoke(const fs::path& dir,int step,int waited){
    auto later=[this](float delay,std::function<void()> fn){
        auto post=window_.Dispatcher();auto alive=alive_;
        std::thread([post,alive,delay,fn]{std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(delay*1000)));post.Post([alive,fn]{if(alive->load())fn();});}).detach();
    };
    auto report=[dir](const std::wstring& line){FILE* f=nullptr;if(_wfopen_s(&f,(dir/L"report.txt").c_str(),L"a, ccs=UTF-8")==0&&f){fwprintf(f,L"%ls\n",line.c_str());fclose(f);}};
    auto fail=[this,report](const std::wstring& why){report(L"FAIL "+why+L" | 状态栏："+std::wstring(status_->Text()));closing_=true;window_.Close();};
    auto again=[&](float delay=.25f){
        if(waited>80){fail(L"等待超时，步骤 "+std::to_wstring(step));return;}
        later(delay,[this,dir,step,waited]{ClipboardSmoke(dir,step,waited+1);});
    };
    auto next=[&](int s,float delay=.4f){later(delay,[this,dir,s]{ClipboardSmoke(dir,s,0);});};
    auto size=[](const PageInfo& p){return std::to_wstring(std::lround(p.width))+L"x"+std::to_wstring(std::lround(p.height));};
    if(!loaded_||busy_){again(.3f);return;}
    switch(step){
    case 0:
        if(mode_!=0)Mode(0);
        report(L"start.pages="+std::to_wstring(info_.pages.size()));
        if(!PasteShortcut()){fail(L"Ctrl+V 未处理");return;}
        return next(1);
    case 1:{
        int images=0;Rect bounds{};
        for(const auto& a:annotations_)if(a.type==Tool::Image){++images;bounds=a.bounds;}
        report(L"annotation.images="+std::to_wstring(images));
        report(L"annotation.selected="+std::wstring(annotation_>=0?L"1":L"0"));
        report(L"annotation.size="+std::to_wstring(std::lround(bounds.w))+L"x"+std::to_wstring(std::lround(bounds.h)));
        const auto& page=info_.pages[0];
        report(L"annotation.centered="+std::wstring(std::abs(bounds.x+bounds.w/2-(page.originX+page.width/2))<page.width*0.3f?L"1":L"0"));
        Mode(2);
        if(!PasteShortcut()){fail(L"页面视图 Ctrl+V 未处理");return;}
        return next(2);
    }
    case 2:{
        report(L"page.count="+std::to_wstring(info_.pages.size()));
        if(info_.pages.size()>=2)report(L"page.size="+size(info_.pages[1]));
        report(L"page.dirty="+std::wstring(info_.dirty?L"1":L"0"));
        const auto out=dir/L"pasted.pdf";
        Task(L"保存测试副本",[out](Engine& e,const Cancel&){e.document.Save(out);},[this,dir,later]{
            later(.3f,[this,dir,later]{PasteFromClipboard(PasteIntent::NewDocument);later(.5f,[this,dir]{ClipboardSmoke(dir,3,0);});});
        });
        return;
    }
    case 3:{
        if(info_.pages.size()!=1||name_->Text()!=L"剪贴板图片.pdf"){again();return;}
        report(L"new.pages="+std::to_wstring(info_.pages.size()));
        report(L"new.size="+size(info_.pages[0]));
        report(L"new.title="+std::wstring(name_->Text()));
        report(L"new.untitled="+std::wstring(source_.empty()?L"1":L"0"));
        report(L"new.dirty="+std::wstring(info_.dirty?L"1":L"0"));
        report(L"tabs="+std::to_wstring(documents_.size()));
        const auto out=dir/L"new.pdf";
        Task(L"保存测试副本",[out](Engine& e,const Cancel&){e.document.Save(out);},[this,dir]{ClipboardSmoke(dir,4,0);});
        return;
    }
    case 4:
        report(L"done=1");
        later(.4f,[this]{closing_=true;window_.Close();});
        return;
    default:return;
    }
}
}
