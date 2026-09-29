// 临时视图旋转 --ui：启动真实 LumenPDF，旋转 90°（缩放按钮显示 ↻90°、文件不变脏）→ 页面视图 / 返回阅读保持 →
// 连转一整圈归零、逆时针、恢复 → 按视图方向写入文件（可撤销、页面变横）→ 另存后重新打开验证。
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
        if(argc!=3)throw std::runtime_error("usage: rotation_tests <LumenPDF.exe> <output>");
        const fs::path exe=fs::absolute(argv[1]),out=fs::absolute(argv[2]);
        std::error_code error;fs::remove_all(out,error);fs::create_directories(out);
        const auto fixture=out/L"portrait.pdf";Document::TextToPdf(L"Rotation fixture\nPage one\n\f\nPage two",fixture);
        Document source;source.Open(fixture);const auto original=source.Info().pages[0];
        SetEnvironmentVariableW(L"LPDF_ROTATE_SMOKE",out.c_str());SetEnvironmentVariableW(L"LPDF_ROTATE_HOLD",nullptr);
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
        Require(wait==WAIT_OBJECT_0,"LumenPDF rotation smoke timed out");
        auto is=[&](const wchar_t* key,const std::wstring& value){Require(r[key]==value,Utf8(std::wstring(key)+L" = "+r[key]+L", expected "+value));};
        const auto w=std::to_wstring(std::lround(original.width)),h=std::to_wstring(std::lround(original.height));
        is(L"start.size",w+L"x"+h);is(L"rotation",L"90");
        Require(r[L"zoom"].find(L"↻90°")!=std::wstring::npos,"zoom button shows the view rotation: "+Utf8(r[L"zoom"]));
        is(L"dirty",L"0");is(L"grid.rotation",L"90");is(L"reading.rotation",L"90");
        is(L"full.turn",L"0");is(L"ccw",L"270");is(L"reset",L"0");
        is(L"applied.rotation",L"0");is(L"applied.size",h+L"x"+w);is(L"applied.undo",L"1");is(L"applied.dirty",L"1");is(L"done",L"1");
        Document rotated;rotated.Open(out/L"rotated.pdf");const auto info=rotated.Info();
        Require(info.pages.size()==source.Info().pages.size(),"page count kept");
        for(const auto& p:info.pages)Require(p.width>p.height,"every page rotated in the saved file");
        Require(source.Info().pages[0].height>source.Info().pages[0].width,"source file untouched");
        std::cout<<"PASS rotation UI: "<<assertions<<" assertions; view-only rotation, indicator, grid/readback, full turn, reset, apply-to-file + save.\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL rotation UI after "<<assertions<<" assertions: "<<e.what()<<"\n";return 1;}
}
