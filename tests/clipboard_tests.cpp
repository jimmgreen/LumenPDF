// 剪贴板 → PDF / 图片批注 / 新页：DIB 各格式解码（32 位 alpha、24 位自下而上、8 位调色板、V5 位域、截断数据）、
// 剪贴板读取优先级（PNG > DIB > 文件）、Ctrl+V 意图、核心 NewFromImage / InsertImagePage；--ui 启动真实 LumenPDF。
// 测试会临时占用系统剪贴板：开始前备份全部可复制的格式，结束时（无论成败）恢复。
#include "app/clipboard_image.h"
#include "core/platform.h"
#include "core/signature.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>
using namespace lpdf;
namespace {
int assertions=0;
void Require(bool okay,const std::string& why){++assertions;if(!okay)throw std::runtime_error(why);}
template<class F> bool Throws(F f){try{f();}catch(...){return true;}return false;}
bool Open(){for(int i=0;i<20;++i){if(OpenClipboard(nullptr))return true;Sleep(25);}return false;}

// 剪贴板备份：HGLOBAL 类格式按字节保存，GDI 句柄类（CF_BITMAP 等由系统从 DIB 合成）跳过。
struct ClipboardBackup {
    std::vector<std::pair<UINT,std::vector<unsigned char>>> items;bool ok{};
    ClipboardBackup(){
        if(!Open())return;
        for(UINT f=EnumClipboardFormats(0);f;f=EnumClipboardFormats(f)){
            if(f==CF_BITMAP||f==CF_ENHMETAFILE||f==CF_METAFILEPICT||f==CF_PALETTE||f==CF_OWNERDISPLAY||f==CF_DSPBITMAP||f==CF_DSPENHMETAFILE||f==CF_DSPMETAFILEPICT||(f>=CF_GDIOBJFIRST&&f<=CF_GDIOBJLAST))continue;
            HANDLE h=GetClipboardData(f);if(!h)continue;const SIZE_T n=GlobalSize(h);if(!n||n>(512u<<20))continue;
            const void* p=GlobalLock(h);if(!p)continue;
            items.push_back({f,std::vector<unsigned char>(static_cast<const unsigned char*>(p),static_cast<const unsigned char*>(p)+n)});GlobalUnlock(h);
        }
        CloseClipboard();ok=true;
    }
    ~ClipboardBackup(){
        if(!ok||!Open())return;EmptyClipboard();
        for(auto& [f,d]:items){HGLOBAL g=GlobalAlloc(GMEM_MOVEABLE,d.size());if(!g)continue;memcpy(GlobalLock(g),d.data(),d.size());GlobalUnlock(g);if(!SetClipboardData(f,g))GlobalFree(g);}
        CloseClipboard();
    }
};
void SetClipboard(const std::vector<std::pair<UINT,std::vector<unsigned char>>>& formats){
    Require(Open(),"open clipboard");EmptyClipboard();
    for(auto& [f,d]:formats){HGLOBAL g=GlobalAlloc(GMEM_MOVEABLE,d.size());memcpy(GlobalLock(g),d.data(),d.size());GlobalUnlock(g);if(!SetClipboardData(f,g))GlobalFree(g);}
    CloseClipboard();
}
// 图案（逻辑坐标自上而下）：左半红、右半蓝、左上 4×4 绿。
enum Colour{Red,Blue,Green};
Colour At(int x,int y,int w){return x<4&&y<4?Green:x<w/2?Red:Blue;}
void Rgb(Colour c,unsigned char& r,unsigned char& g,unsigned char& b){r=c==Red?220:0;g=c==Green?200:0;b=c==Blue?230:0;}
std::vector<unsigned char> Header(int w,int h,int bits,DWORD compression,DWORD size=sizeof(BITMAPINFOHEADER)){
    std::vector<unsigned char> v(size,0);BITMAPINFOHEADER hd{};hd.biSize=size;hd.biWidth=w;hd.biHeight=h;hd.biPlanes=1;hd.biBitCount=static_cast<WORD>(bits);hd.biCompression=compression;
    memcpy(v.data(),&hd,sizeof(hd));return v;
}
std::vector<unsigned char> Dib32(int w,int h,bool topDown,int alpha){
    auto v=Header(w,topDown?-h:h,32,BI_RGB);
    for(int row=0;row<h;++row){const int y=topDown?row:h-1-row;for(int x=0;x<w;++x){unsigned char r,g,b;Rgb(At(x,y,w),r,g,b);v.insert(v.end(),{b,g,r,static_cast<unsigned char>(alpha)});}}
    return v;
}
std::vector<unsigned char> Dib24(int w,int h){
    auto v=Header(w,h,24,BI_RGB);const int pad=(4-(w*3)%4)%4;
    for(int row=0;row<h;++row){const int y=h-1-row;for(int x=0;x<w;++x){unsigned char r,g,b;Rgb(At(x,y,w),r,g,b);v.insert(v.end(),{b,g,r});}for(int i=0;i<pad;++i)v.push_back(0);}
    return v;
}
std::vector<unsigned char> Dib8(int w,int h){
    auto v=Header(w,h,8,BI_RGB);BITMAPINFOHEADER hd;memcpy(&hd,v.data(),sizeof(hd));hd.biClrUsed=3;memcpy(v.data(),&hd,sizeof(hd));
    for(Colour c:{Red,Blue,Green}){unsigned char r,g,b;Rgb(c,r,g,b);v.insert(v.end(),{b,g,r,0});}
    const int pad=(4-w%4)%4;
    for(int row=0;row<h;++row){const int y=h-1-row;for(int x=0;x<w;++x)v.push_back(static_cast<unsigned char>(At(x,y,w)));for(int i=0;i<pad;++i)v.push_back(0);}
    return v;
}
// V5 头、BI_BITFIELDS，通道顺序 R G B A（非常规掩码），alpha=128。
std::vector<unsigned char> DibV5Fields(int w,int h){
    auto v=Header(w,-h,32,BI_BITFIELDS,sizeof(BITMAPV5HEADER));
    const uint32_t masks[4]={0x000000ff,0x0000ff00,0x00ff0000,0xff000000};memcpy(v.data()+40,masks,16);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){unsigned char r,g,b;Rgb(At(x,y,w),r,g,b);v.insert(v.end(),{r,g,b,128});}
    return v;
}
void CheckPattern(const Bitmap& b,int w,int h,int alpha,const std::string& what){
    Require(b.width==w&&b.height==h,what+": size");
    auto px=[&](int x,int y){return b.bgra.data()+static_cast<size_t>(y)*b.stride+x*4;};
    const auto* g=px(1,1);const auto* r=px(w/4,h-2);const auto* bl=px(w-2,h/2);
    Require(g[1]>150&&g[2]<60&&g[0]<60,what+": top-left green (orientation)");
    Require(r[2]>150&&r[1]<60&&r[0]<60,what+": left red");
    Require(bl[0]>150&&bl[1]<60&&bl[2]<60,what+": right blue (channel order)");
    Require(g[3]==alpha&&bl[3]==alpha,what+": alpha "+std::to_string(g[3]));
}
Bitmap PatternBitmap(int w,int h){
    Bitmap b;b.width=w;b.height=h;b.stride=w*4;b.bgra.resize(static_cast<size_t>(b.stride)*h);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){unsigned char r,g,bl;Rgb(At(x,y,w),r,g,bl);auto* p=b.bgra.data()+static_cast<size_t>(y)*b.stride+x*4;p[0]=bl;p[1]=g;p[2]=r;p[3]=255;}
    return b;
}
std::vector<unsigned char> Bytes(const fs::path& f){std::ifstream in(f,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
std::vector<unsigned char> DropFiles(const std::vector<fs::path>& files){
    std::vector<unsigned char> v(sizeof(DROPFILES),0);DROPFILES df{};df.pFiles=sizeof(DROPFILES);df.fWide=TRUE;memcpy(v.data(),&df,sizeof(df));
    for(const auto& f:files){const auto s=f.wstring();const auto* p=reinterpret_cast<const unsigned char*>(s.c_str());v.insert(v.end(),p,p+(s.size()+1)*sizeof(wchar_t));}
    v.push_back(0);v.push_back(0);return v;
}
// 页面渲染后检查图案（页面坐标按比例取点）。
void CheckRendered(Document& d,int page,const std::string& what){
    auto b=d.Render(page,1.0f);
    // 图片在页面中的位置：取页面内非白像素的包围框。
    int x0=b.width,y0=b.height,x1=-1,y1=-1;
    for(int y=0;y<b.height;++y)for(int x=0;x<b.width;++x){const auto* p=b.bgra.data()+static_cast<size_t>(y)*b.stride+x*4;if(p[0]<240||p[1]<240||p[2]<240){x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x);y1=std::max(y1,y);}}
    Require(x1>x0&&y1>y0,what+": image drawn");
    auto px=[&](float fx,float fy){const int x=x0+static_cast<int>((x1-x0)*fx),y=y0+static_cast<int>((y1-y0)*fy);return b.bgra.data()+static_cast<size_t>(y)*b.stride+x*4;};
    const auto* r=px(.25f,.6f);const auto* bl=px(.75f,.5f);
    Require(r[2]>150&&r[0]<90,what+": left red");Require(bl[0]>150&&bl[2]<90,what+": right blue");
}
std::map<std::wstring,std::wstring> ReadReport(const fs::path& file){
    std::map<std::wstring,std::wstring> values;std::string raw;{std::ifstream b(file,std::ios::binary);raw.assign(std::istreambuf_iterator<char>(b),{});}
    if(raw.size()>=3&&static_cast<unsigned char>(raw[0])==0xEF)raw.erase(0,3);
    std::wstringstream text(Wide(raw));std::wstring line;
    while(std::getline(text,line)){if(!line.empty()&&line.back()==L'\r')line.pop_back();
        if(line.rfind(L"FAIL",0)==0)throw std::runtime_error("UI smoke: "+Utf8(line));
        const auto eq=line.find(L'=');if(eq!=std::wstring::npos)values[line.substr(0,eq)]=line.substr(eq+1);}
    return values;
}
int RunUi(const fs::path& exe,const fs::path& out){
    std::error_code error;fs::remove_all(out,error);fs::create_directories(out);
    const auto fixture=out/L"source.pdf";Document::TextToPdf(L"Clipboard paste fixture\nPage one",fixture);
    ClipboardBackup backup;Require(backup.ok,"clipboard backup");
    SetClipboard({{CF_DIB,Dib32(320,200,false,0)}});
    SetEnvironmentVariableW(L"LPDF_CLIPBOARD_SMOKE",out.c_str());
    SetEnvironmentVariableW(L"LPDF_SMOKE_TIMEOUT",L"50");
    SetEnvironmentVariableW(L"LPDF_NO_DEFAULT_PROMPT",L"1");
    std::wstring command=L"\""+exe.wstring()+L"\" --smoke \""+fixture.wstring()+L"\"";
    STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
    Require(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,out.c_str(),&si,&pi)!=0,"cannot start LumenPDF");
    const DWORD wait=WaitForSingleObject(pi.hProcess,70000);
    if(wait!=WAIT_OBJECT_0)TerminateProcess(pi.hProcess,3);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    Require(fs::exists(out/L"report.txt"),"smoke wrote no report");
    auto r=ReadReport(out/L"report.txt");
    Require(wait==WAIT_OBJECT_0,"LumenPDF clipboard smoke timed out");
    auto is=[&](const wchar_t* key,const wchar_t* value){Require(r[key]==value,Utf8(std::wstring(key)+L" = "+r[key]+L", expected "+value));};
    is(L"start.pages",L"1");
    is(L"annotation.images",L"1");is(L"annotation.selected",L"1");is(L"annotation.size",L"240x150");is(L"annotation.centered",L"1");
    is(L"page.count",L"2");is(L"page.size",L"842x595");is(L"page.dirty",L"1");
    is(L"new.pages",L"1");is(L"new.size",L"240x150");is(L"new.title",L"剪贴板图片.pdf");is(L"new.untitled",L"1");is(L"new.dirty",L"1");
    is(L"tabs",L"2");is(L"done",L"1");
    Document pasted;pasted.Open(out/L"pasted.pdf");
    Require(pasted.Info().pages.size()==2,"pasted.pdf has the inserted page");
    const auto annots=pasted.Annotations(0);
    Require(std::count_if(annots.begin(),annots.end(),[](const Annotation& a){return a.type==Tool::Image;})==1,"image annotation saved on page 1");
    CheckRendered(pasted,1,"inserted page");
    Document created;created.Open(out/L"new.pdf");CheckRendered(created,0,"new document");
    std::cout<<"PASS clipboard UI: "<<assertions<<" assertions; Ctrl+V annotation (centred, selected), page view new page, new PDF from clipboard, saved output verified.\n";
    return 0;
}
}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc==4&&std::wstring_view(argv[1])==L"--ui")return RunUi(fs::absolute(argv[2]),fs::absolute(argv[3]));
        if(argc<2)throw std::runtime_error("usage: clipboard_tests <output> | --ui <LumenPDF.exe> <output>");
        const fs::path out=fs::absolute(argv[1]);std::error_code error;fs::remove_all(out,error);fs::create_directories(out);

        // ---- Ctrl+V 意图 ----
        Require(ResolvePasteIntent(PasteIntent::Auto,false,false,0)==PasteIntent::NewDocument,"no document -> new");
        Require(ResolvePasteIntent(PasteIntent::Auto,true,true,0)==PasteIntent::NewDocument,"home -> new");
        Require(ResolvePasteIntent(PasteIntent::Auto,true,false,3)==PasteIntent::NewDocument,"merge view -> new");
        Require(ResolvePasteIntent(PasteIntent::Auto,true,false,0)==PasteIntent::Annotation,"reading -> annotation");
        Require(ResolvePasteIntent(PasteIntent::Auto,true,false,1)==PasteIntent::Annotation,"annotate -> annotation");
        Require(ResolvePasteIntent(PasteIntent::Auto,true,false,2)==PasteIntent::NewPage,"pages -> new page");
        Require(ResolvePasteIntent(PasteIntent::NewDocument,true,false,0)==PasteIntent::NewDocument,"explicit wins");

        // ---- DIB 解码 ----
        auto d32=Dib32(40,30,false,0);CheckPattern(DibToBitmap(d32.data(),d32.size()),40,30,255,"32-bit bottom-up, zero alpha -> opaque");
        auto t32=Dib32(40,30,true,200);CheckPattern(DibToBitmap(t32.data(),t32.size()),40,30,200,"32-bit top-down alpha kept");
        auto d24=Dib24(37,21);CheckPattern(DibToBitmap(d24.data(),d24.size()),37,21,255,"24-bit padded rows");
        auto d8=Dib8(33,20);CheckPattern(DibToBitmap(d8.data(),d8.size()),33,20,255,"8-bit palette");
        auto v5=DibV5Fields(40,30);CheckPattern(DibToBitmap(v5.data(),v5.size()),40,30,128,"V5 bitfields RGBA");
        Require(Throws([&]{DibToBitmap(d32.data(),d32.size()-10);}),"truncated pixels rejected");
        Require(Throws([&]{DibToBitmap(d32.data(),20);}),"truncated header rejected");
        auto huge=Header(100000,100000,32,BI_RGB);Require(Throws([&]{DibToBitmap(huge.data(),huge.size());}),"absurd size rejected");
        Require(IsImageFile(L"a.PNG")&&IsImageFile(L"b.jpeg")&&!IsImageFile(L"c.pdf")&&!IsImageFile(L"d.txt"),"image extensions");

        const fs::path temp=out/L"temp";
        {
            ClipboardBackup backup;Require(backup.ok,"clipboard backup");
            ClipboardContent c;std::wstring why;
            // PNG 优先于 DIB。
            const auto png=out/L"pattern.png";SaveSignaturePng(PatternBitmap(320,200),png);
            SetClipboard({{RegisterClipboardFormatW(L"PNG"),Bytes(png)},{CF_DIB,Dib32(10,10,false,0)}});
            Require(ClipboardHasContent(),"has content (PNG)");
            Require(ReadClipboard(nullptr,temp,c,why)&&c.source==L"PNG"&&c.temporary&&fs::exists(c.image),"PNG preferred");
            Require(Document::ImageSize(c.image)==std::pair<int,int>(320,200),"PNG bytes written unchanged");
            const auto pngTemp=c.image;
            // 只有 DIB。
            SetClipboard({{CF_DIB,Dib24(64,48)}});
            Require(ReadClipboard(nullptr,temp,c,why)&&c.source==L"DIB"&&c.width==64&&c.height==48,"DIB read");
            CheckPattern(LoadSignaturePng(c.image),64,48,255,"DIB -> temp PNG");
            Require(c.image!=pngTemp,"unique temp names");
            // 资源管理器复制的文件：图片 + PDF + 其它。
            const auto pdf=out/L"doc.pdf";Document::TextToPdf(L"x",pdf);
            SetClipboard({{CF_HDROP,DropFiles({png,pdf,out/L"note.txt"})}});
            Require(ReadClipboard(nullptr,temp,c,why)&&c.source==L"文件"&&c.image==png&&!c.temporary&&c.pdfs==std::vector<fs::path>{pdf},"copied files");
            // 只有文字 / 空剪贴板：无内容、无错误。
            const std::wstring text=L"hello";std::vector<unsigned char> tb(reinterpret_cast<const unsigned char*>(text.c_str()),reinterpret_cast<const unsigned char*>(text.c_str())+(text.size()+1)*2);
            SetClipboard({{CF_UNICODETEXT,tb}});
            Require(!ClipboardHasContent()&&!ReadClipboard(nullptr,temp,c,why)&&why.empty(),"text only: nothing to paste");
            SetClipboard({});
            Require(!ReadClipboard(nullptr,temp,c,why)&&why.empty(),"empty clipboard");
            // 损坏的 DIB：报错且不留临时文件。
            const auto before=std::distance(fs::directory_iterator(temp),fs::directory_iterator());
            auto broken=Dib32(40,30,false,0);broken.resize(60);SetClipboard({{CF_DIB,broken}});
            Require(!ReadClipboard(nullptr,temp,c,why)&&!why.empty(),"broken DIB reports an error");
            Require(std::distance(fs::directory_iterator(temp),fs::directory_iterator())==before,"no temp file left behind");
        }

        // ---- 核心 ----
        const auto png=out/L"pattern.png";
        Document n;n.NewFromImage(png);auto info=n.Info();
        Require(info.pages.size()==1&&std::lround(info.pages[0].width)==240&&std::lround(info.pages[0].height)==150,"new document page = 96 DPI image size");
        Require(info.dirty&&!info.canUndo,"new document is unsaved; the image page is not undoable");
        CheckRendered(n,0,"NewFromImage");
        const auto tiny=out/L"tiny.png";SaveSignaturePng(PatternBitmap(1,1),tiny);
        Document t;t.NewFromImage(tiny);Require(t.Info().pages[0].width>=3,"tiny image page has a minimum size");
        Require(Throws([&]{Document x;x.NewFromImage(out/L"missing.png");}),"missing image rejected");
        Document a;a.Open(out/L"doc.pdf");a.InsertImagePage(0,png);info=a.Info();
        Require(info.pages.size()==2&&info.pages[1].width>info.pages[1].height,"landscape image -> landscape page after page 1");
        Require(std::lround(info.pages[1].width)==std::lround(info.pages[0].height),"inserted page matches neighbour size");
        Require(info.canUndo&&info.dirty,"insert is undoable");
        CheckRendered(a,1,"InsertImagePage");
        a.Undo();Require(a.Info().pages.size()==1,"undo removes the image page");
        a.InsertImagePage(-1,png);Require(a.Info().pages.size()==2&&a.Info().pages[0].width>a.Info().pages[0].height,"insert before first page");
        std::cout<<"PASS clipboard: "<<assertions<<" assertions; paste intent, DIB 32/24/8/V5, truncation, PNG>DIB>files priority, text/empty, broken data, NewFromImage, InsertImagePage + undo. Clipboard restored.\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL clipboard after "<<assertions<<" assertions: "<<e.what()<<"\n";return 1;}
}
