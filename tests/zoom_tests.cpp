// 框选放大 / 放大镜 --ui：启动真实 LumenPDF：Z 进入框选 → 放大到框（缩放按钮、状态栏提示、可返回）→ Shift+Z 返回 →
// 再按一次提示无可返回 → L 打开放大镜、高分辨率局部图到达、调整倍率 → 关闭；全程文件不变脏。
#include "core/document.h"
#include "core/platform.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
using namespace lpdf;
namespace {
int assertions=0;
void Require(bool okay,const std::string& why){++assertions;if(!okay)throw std::runtime_error(why);}
}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc!=3)throw std::runtime_error("usage: zoom_tests <LumenPDF.exe> <output>");
        const fs::path exe=fs::absolute(argv[1]),out=fs::absolute(argv[2]);
        std::error_code error;fs::remove_all(out,error);fs::create_directories(out);
        const auto fixture=out/L"portrait.pdf";Document::TextToPdf(L"Zoom fixture\nSmall print line one\nSmall print line two\n\f\nPage two",fixture);
        Document source;source.Open(fixture);const auto original=source.Info().pages[0];
        SetEnvironmentVariableW(L"LPDF_ZOOM_SMOKE",out.c_str());SetEnvironmentVariableW(L"LPDF_ZOOM_HOLD",nullptr);
        SetEnvironmentVariableW(L"LPDF_SMOKE_TIMEOUT",L"40");SetEnvironmentVariableW(L"LPDF_NO_DEFAULT_PROMPT",L"1");
        std::wstring command=L"\""+exe.wstring()+L"\" --smoke \""+fixture.wstring()+L"\"";
        STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
        Require(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,out.c_str(),&si,&pi)!=0,"cannot start LumenPDF");
        const DWORD wait=WaitForSingleObject(pi.hProcess,60000);if(wait!=WAIT_OBJECT_0)TerminateProcess(pi.hProcess,3);
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
        Require(fs::exists(out/L"report.txt"),"smoke wrote no report");
        std::string raw;{std::ifstream f(out/L"report.txt",std::ios::binary);raw.assign(std::istreambuf_iterator<char>(f),{});}
        if(raw.size()>=3&&static_cast<unsigned char>(raw[0])==0xEF)raw.erase(0,3);
        std::map<std::wstring,std::wstring> r;std::wstringstream text(Wide(raw));std::wstring line;
        while(std::getline(text,line)){if(!line.empty()&&line.back()==L'\r')line.pop_back();const auto eq=line.find(L'=');if(eq!=std::wstring::npos)r[line.substr(0,eq)]=line.substr(eq+1);}
        Require(wait==WAIT_OBJECT_0,"LumenPDF zoom smoke timed out");
        auto is=[&](const wchar_t* key,const std::wstring& value){Require(r[key]==value,Utf8(std::wstring(key)+L" = "+r[key]+L", expected "+value));};
        (void)original;
        for(const wchar_t* key:{L"box.active",L"box.hint",L"box.larger",L"box.status",L"box.back",L"back.same",L"back.none",L"lens.on",L"lens.hint",L"lens.rendered",L"lens.factor",L"lens.off",L"done"})is(key,L"1");
        is(L"dirty",L"0");
        Require(r[L"box.button"]==r[L"box.after"]+L"%","zoom button shows the box zoom: "+Utf8(r[L"box.button"]));
        Require(r[L"back.zoom"]==r[L"start.zoom"],"Shift+Z restores the starting zoom");
        std::cout<<"PASS zoom tools UI: "<<assertions<<" assertions; Z box zoom, indicator, Shift+Z back, magnifier render + factor, never dirty.\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL zoom tools UI after "<<assertions<<" assertions: "<<e.what()<<"\n";return 1;}
}
