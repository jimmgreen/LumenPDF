// 打印预览：共用排版（LayoutSheet）、自动方向、纸张方向换算、页码区间压缩、预览位图（多合一位置、
// 小册子横向、灰度、隐藏批注）；--ui 模式启动真实 LumenPDF 检查预览对话框并以“打印到 PDF”核对输出。
#include "app/print_job.h"
#include "core/platform.h"
#include "core/signature.h"
#include <windows.h>
#include <winspool.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>
using namespace lpdf;
namespace {
int assertions=0;
void Require(bool okay,const std::string& why){++assertions;if(!okay)throw std::runtime_error(why);}
// 5 页：第 1、2、4、5 页纵向，第 3 页旋转为横向；第 1 页有一个红色实心矩形批注。
fs::path MakeFixture(const fs::path& dir){
    const auto base=dir/L"base.pdf",fixture=dir/L"five.pdf";
    Document::TextToPdf(L"PRINT PREVIEW FIXTURE\nWWWWWWWWWWWWWWWWWWWW\nWWWWWWWWWWWWWWWWWWWW\nWWWWWWWWWWWWWWWWWWWW",base);
    Document d;d.Open(base);
    d.DuplicatePages({0});d.DuplicatePages({0});d.DuplicatePages({0});d.DuplicatePages({0});
    Require(d.Info().pages.size()==5,"fixture has 5 pages");
    d.RotatePage(2,90);
    AnnotationStyle style;style.color=0xff2020;style.fillColor=0xff2020;style.filled=true;style.lineWidth=4;
    d.AddAnnotation(0,Tool::Rectangle,{120,320,300,220},{},{},{},12,1,style);
    d.Save(fixture);return fixture;
}
bool DarkIn(const Bitmap& b,int x0,int y0,int x1,int y1){
    for(int y=std::max(0,y0);y<std::min(b.height,y1);++y)for(int x=std::max(0,x0);x<std::min(b.width,x1);++x){
        const auto* c=b.bgra.data()+static_cast<size_t>(y)*b.stride+x*4;if(c[0]<165&&c[1]<165&&c[2]<165)return true;}
    return false;
}
int Reddish(const Bitmap& b){
    int n=0;for(int y=0;y<b.height;++y)for(int x=0;x<b.width;++x){const auto* c=b.bgra.data()+static_cast<size_t>(y)*b.stride+x*4;if(c[2]>180&&c[1]<120&&c[0]<120)++n;}
    return n;
}
bool Gray(const Bitmap& b){
    for(int y=0;y<b.height;++y)for(int x=0;x<b.width;++x){const auto* c=b.bgra.data()+static_cast<size_t>(y)*b.stride+x*4;if(c[0]!=c[1]||c[1]!=c[2])return false;}
    return true;
}
std::map<std::wstring,std::wstring> ReadReport(const fs::path& file){
    std::map<std::wstring,std::wstring> values;std::wifstream in(file);std::wstring line;
    std::string raw;{std::ifstream b(file,std::ios::binary);raw.assign(std::istreambuf_iterator<char>(b),{});}
    if(raw.size()>=3&&static_cast<unsigned char>(raw[0])==0xEF)raw.erase(0,3);
    std::wstringstream text(Wide(raw));
    while(std::getline(text,line)){if(!line.empty()&&line.back()==L'\r')line.pop_back();const auto eq=line.find(L'=');
        if(line.rfind(L"FAIL",0)==0)throw std::runtime_error("UI smoke: "+Utf8(line));
        if(eq!=std::wstring::npos)values[line.substr(0,eq)]=line.substr(eq+1);}
    return values;
}
// Windows“管理默认打印机”开启时，打印到某台打印机会把它设为默认打印机。测试前记下，结束时（无论成败）恢复。
struct DefaultPrinterGuard {
    std::wstring name=DefaultPrinterName();
    ~DefaultPrinterGuard(){if(!name.empty()&&DefaultPrinterName()!=name)SetDefaultPrinterW(name.c_str());}
};
int RunUi(const fs::path& exe,const fs::path& out){
    DefaultPrinterGuard printerGuard;
    std::error_code error;fs::remove_all(out,error);fs::create_directories(out);
    const auto fixture=MakeFixture(out);
    SetEnvironmentVariableW(L"LPDF_PRINT_PREVIEW_SMOKE",out.c_str());
    SetEnvironmentVariableW(L"LPDF_SMOKE_TIMEOUT",L"60");
    SetEnvironmentVariableW(L"LPDF_NO_DEFAULT_PROMPT",L"1");
    std::wstring command=L"\""+exe.wstring()+L"\" --smoke \""+fixture.wstring()+L"\"";
    STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
    Require(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,out.c_str(),&si,&pi)!=0,"cannot start LumenPDF");
    const DWORD wait=WaitForSingleObject(pi.hProcess,75000);
    if(wait!=WAIT_OBJECT_0)TerminateProcess(pi.hProcess,3);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    const auto reportFile=out/L"report.txt";
    Require(fs::exists(reportFile),"smoke wrote no report");
    auto r=ReadReport(reportFile);
    Require(wait==WAIT_OBJECT_0,"LumenPDF print preview smoke timed out (last status: "+Utf8(r[L"status"])+")");
    auto is=[&](const wchar_t* key,const wchar_t* value){Require(r[key]==value,Utf8(std::wstring(key)+L" = "+r[key]+L", expected "+value));};
    is(L"default.sheets",L"5");is(L"default.landscape",L"0");
    is(L"nup4.sheets",L"2");is(L"nup4.landscape",L"0");
    is(L"booklet.sheets",L"4");is(L"booklet.landscape",L"1");is(L"booklet.step",L"第 2 / 4 面");
    is(L"invalid.error",L"1");is(L"invalid.sheets",L"0");
    is(L"final.sheets",L"2");is(L"final.landscape",L"1");is(L"accepted",L"1");is(L"done",L"1");
    Require(r[L"booklet.summary"].find(L"双面，2 张纸")!=std::wstring::npos&&r[L"booklet.summary"].find(L"3 个空白位")!=std::wstring::npos,"booklet summary: "+Utf8(r[L"booklet.summary"]));
    // 预览 PNG：方向与多合一位置。
    const auto portrait=LoadSignaturePng(out/L"default.png"),booklet=LoadSignaturePng(out/L"booklet.png"),final=LoadSignaturePng(out/L"final.png");
    Require(portrait.height>portrait.width&&booklet.width>booklet.height&&final.width>final.height,"preview orientation");
    const auto nup=LoadSignaturePng(out/L"nup4.png");
    const int w=nup.width,h=nup.height;
    Require(DarkIn(nup,0,0,w/2,h/2)&&DarkIn(nup,w/2,0,w,h/2)&&DarkIn(nup,0,h/2,w/2,h)&&DarkIn(nup,w/2,h/2,w,h),"4-up preview has a page in every quadrant");
    Require(Gray(final),"grayscale preview has no colour");
    // 打印到 PDF：假脱机可能在进程退出后才写完文件。
    const auto printed=out/L"print.pdf";
    for(int i=0;i<60&&!(fs::exists(printed)&&fs::file_size(printed,error)>0);++i)std::this_thread::sleep_for(std::chrono::milliseconds(250));
    Require(fs::exists(printed),"Microsoft Print to PDF produced no file (status: "+Utf8(r[L"status"])+")");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    Document p;p.Open(printed);const auto info=p.Info();
    // 假脱机完成后 Windows 可能已切换默认打印机：立即恢复并确认。
    for(int i=0;i<8&&DefaultPrinterName()!=printerGuard.name;++i){SetDefaultPrinterW(printerGuard.name.c_str());std::this_thread::sleep_for(std::chrono::milliseconds(250));}
    Require(printerGuard.name.empty()||DefaultPrinterName()==printerGuard.name,"default printer restored after test print");
    Require(info.pages.size()==2,"printed sheet count = preview sheet count ("+std::to_string(info.pages.size())+")");
    Require(info.pages[0].width>info.pages[0].height,"printed paper is landscape like the preview");
    std::cout<<"PASS print preview UI: "<<assertions<<" assertions; dialog, 1/4-up, booklet, invalid range, 2-up pages 1-3 landscape gray, printed "<<info.pages.size()<<" landscape sheets.\n";
    std::wcout<<L"  "<<r[L"paper"]<<L"\n";
    return 0;
}
}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc==4&&std::wstring_view(argv[1])==L"--ui")return RunUi(fs::absolute(argv[2]),fs::absolute(argv[3]));
        if(argc<2)throw std::runtime_error("usage: print_preview_tests <output> | --ui <LumenPDF.exe> <output>");
        const fs::path out=fs::absolute(argv[1]);
        std::error_code error;fs::remove_all(out,error);fs::create_directories(out);

        // ---- 纯计算 ----
        const std::vector<PageInfo> pages={{595,842},{595,842},{842,595},{595,842},{595,842}};
        PrintSettings s;s.pages={0,1,2,3,4};
        Require(!AutoLandscape(s,pages),"1-up portrait page -> portrait paper");
        s.pagesPerSheet=2;Require(AutoLandscape(s,pages),"2-up portrait pages -> landscape paper");
        s.pagesPerSheet=4;Require(!AutoLandscape(s,pages),"4-up keeps page orientation");
        s.pagesPerSheet=6;Require(!AutoLandscape(s,pages),"6-up (2x3) portrait");
        s.pagesPerSheet=1;s.pages={2};Require(AutoLandscape(s,pages),"landscape page -> landscape paper");
        s.pages={0,1,2,3,4};s.booklet=true;Require(AutoLandscape(s,pages),"booklet -> landscape");
        s.orientation=1;Require(!ResolveLandscape(s,pages),"explicit portrait wins");
        s.booklet=false;s.orientation=2;Require(ResolveLandscape(s,pages),"explicit landscape wins");
        s.orientation=0;s.parity=2;s.pages={2,1};s.pagesPerSheet=1;Require(!AutoLandscape(s,pages),"parity filter picks first printed page (2 -> page index 1, portrait)");
        s.parity=0;

        PaperInfo a4;a4.left=10;a4.top=20;a4.right=30;a4.bottom=40;
        const auto land=Oriented(a4,true);
        Require(land.width>land.height&&land.left==20&&land.top==30&&land.right==40&&land.bottom==10,"landscape margins rotate");
        Require(PaperName(a4)==L"A4"&&PaperName(land)==L"A4","A4 name");
        PaperInfo letter;letter.width=612;letter.height=792;Require(PaperName(letter)==L"Letter","Letter name");
        PaperInfo odd;odd.width=283.46f;odd.height=425.2f;Require(PaperName(odd)==L"100 × 150 毫米","custom paper name");

        Require(PageRuns({0,1,2,4,6,7})==std::vector<std::pair<int,int>>({{1,3},{5,5},{7,8}}),"page runs");
        Require(PageRuns({}).empty(),"empty runs");

        // 共用排版：4 合 1 的四个格子互不重叠且都在可打印区域内；2 合 1 横向页自动旋转。
        s=PrintSettings{};s.pages={0,1,2,3};s.pagesPerSheet=4;
        auto slots=LayoutSheet(s,{0,1,2,3},pages,4000,6000,600,600);
        Require(slots.size()==4,"4 slots");
        for(size_t i=0;i<slots.size();++i){const auto& p=slots[i].place;
            Require(p.x>=0&&p.y>=0&&p.x+p.width<=4000&&p.y+p.height<=6000,"slot inside printable area");
            for(size_t j=i+1;j<slots.size();++j){const auto& q=slots[j].place;Require(p.x+p.width<=q.x||q.x+q.width<=p.x||p.y+p.height<=q.y||q.y+q.height<=p.y,"slots do not overlap");}}
        Require(slots[2].place.rotate,"landscape page rotated into portrait cell");
        Require(slots[1].place.x>=2000&&slots[2].place.y>=3000,"reading order: left-right, top-bottom");
        Require(LayoutSheet(s,{-1,0},pages,4000,6000,600,600).size()==1,"blank booklet slot skipped");
        bool threw=false;try{LayoutSheet(s,{9},pages,4000,6000,600,600);}catch(...){threw=true;}Require(threw,"page out of range rejected");

        // ---- 预览位图 ----
        const auto fixture=MakeFixture(out);
        Document d;d.Open(fixture);
        PaperInfo paper;   // A4 纵向，四边 18 点
        s=PrintSettings{};s.pages={0,1,2,3,4};
        auto one=RenderSheetPreview(d,s,{0},paper,600);
        Require(one.height==600&&std::abs(one.width-424)<=1,"A4 preview size ("+std::to_string(one.width)+"x"+std::to_string(one.height)+")");
        Require(Reddish(one)>500,"annotation visible in preview");
        s.annotations=false;Require(Reddish(RenderSheetPreview(d,s,{0},paper,600))==0,"hidden annotations are not previewed");
        s.annotations=true;s.grayscale=true;auto gray=RenderSheetPreview(d,s,{0},paper,600);Require(Gray(gray)&&Reddish(gray)==0,"grayscale preview");
        s.grayscale=false;s.pagesPerSheet=4;
        auto four=RenderSheetPreview(d,s,{0,1,2,3},paper,600);
        const int w=four.width,h=four.height;
        Require(DarkIn(four,0,0,w/2,h/2)&&DarkIn(four,w/2,0,w,h/2)&&DarkIn(four,0,h/2,w/2,h)&&DarkIn(four,w/2,h/2,w,h),"4-up: every quadrant has page content");
        auto partial=RenderSheetPreview(d,s,{0},paper,600);
        Require(DarkIn(partial,0,0,w/2,h/2)&&!DarkIn(partial,w/2,0,w,h/2)&&!DarkIn(partial,0,h/2,w,h),"incomplete final 4-up sheet leaves empty cells blank");
        s.pagesPerSheet=1;s.booklet=true;
        auto book=RenderSheetPreview(d,s,{-1,0},Oriented(paper,true),600);
        Require(book.width==600&&book.height<book.width,"booklet preview landscape");
        Require(!DarkIn(book,0,0,book.width/2,book.height)&&DarkIn(book,book.width/2,0,book.width,book.height),"booklet blank slot on the left, page on the right");
        SaveSignaturePng(four,out/L"four.png");SaveSignaturePng(book,out/L"booklet.png");
        threw=false;try{RenderSheetPreview(d,s,{0},PaperInfo{0,0},600);}catch(...){threw=true;}Require(threw,"invalid paper rejected");
        std::cout<<"PASS print preview: "<<assertions<<" assertions; auto orientation, margins, paper names, page runs, shared layout, preview bitmaps (n-up, booklet, grayscale, annotations).\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL print preview after "<<assertions<<" assertions: "<<e.what()<<"\n";return 1;}
}
