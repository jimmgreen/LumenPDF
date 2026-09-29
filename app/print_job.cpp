#include "print_job.h"
#include <ole2.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <winspool.h>
#pragma comment(lib,"winspool.lib")
namespace lpdf {
std::pair<int,int> PrintGrid(int count){
    switch(count){case 1:return {1,1};case 2:return {2,1};case 4:return {2,2};case 6:return {2,3};case 9:return {3,3};case 16:return {4,4};default:throw std::runtime_error("Invalid pages per sheet");}
}
std::vector<PrintSheet> PlanPrint(const PrintSettings& settings){
    (void)PrintGrid(settings.pagesPerSheet);
    std::vector<int> pages;for(int p:settings.pages){if(p<0)throw std::runtime_error("Invalid print page");if(settings.parity==1&&p%2==1)continue;if(settings.parity==2&&p%2==0)continue;pages.push_back(p);}
    if(settings.reverse)std::reverse(pages.begin(),pages.end());
    std::vector<PrintSheet> sheets;
    if(settings.booklet){
        while(pages.size()%4)pages.push_back(-1);
        for(size_t i=0;i<pages.size()/4;++i){
            if(settings.bookletSide!=2)sheets.push_back({pages[pages.size()-1-2*i],pages[2*i]});
            if(settings.bookletSide!=1)sheets.push_back({pages[2*i+1],pages[pages.size()-2-2*i]});
        }
    }else for(size_t i=0;i<pages.size();i+=settings.pagesPerSheet){PrintSheet sheet;for(int j=0;j<settings.pagesPerSheet&&i+j<pages.size();++j)sheet.push_back(pages[i+j]);sheets.push_back(std::move(sheet));}
    return sheets;
}
PrintPlacement PlacePage(float pageWidth,float pageHeight,int areaWidth,int areaHeight,int dpiX,int dpiY,bool fit,bool autoRotate){
    PrintPlacement result;
    if(pageWidth<=0||pageHeight<=0||areaWidth<=0||areaHeight<=0||dpiX<=0||dpiY<=0)return result;
    const bool pageLandscape=pageWidth>pageHeight*1.02f,areaLandscape=areaWidth>areaHeight;
    result.rotate=autoRotate&&pageLandscape!=areaLandscape&&std::abs(pageWidth-pageHeight)>1;
    // 旋转后页面的宽高（点）互换。
    const float w=result.rotate?pageHeight:pageWidth,h=result.rotate?pageWidth:pageHeight;
    const float naturalW=w*dpiX/72.0f,naturalH=h*dpiY/72.0f;
    float scale=std::min(areaWidth/naturalW,areaHeight/naturalH);
    if(!fit)scale=std::min(scale,1.0f);
    result.scale=scale;
    result.width=std::max(1,static_cast<int>(std::lround(naturalW*scale)));
    result.height=std::max(1,static_cast<int>(std::lround(naturalH*scale)));
    result.x=(areaWidth-result.width)/2;result.y=(areaHeight-result.height)/2;
    return result;
}
std::vector<SlotPlacement> LayoutSheet(const PrintSettings& settings,const PrintSheet& sheet,const std::vector<PageInfo>& pages,
                                       int areaW,int areaH,int dpiX,int dpiY){
    std::vector<SlotPlacement> result;
    const auto [columns,rows]=PrintGrid(settings.booklet?2:settings.pagesPerSheet);
    for(size_t slot=0;slot<sheet.size();++slot){
        const int page=sheet[slot];if(page<0)continue;
        if(page>=static_cast<int>(pages.size()))throw std::runtime_error("Page index out of range");
        const auto& size=pages[static_cast<size_t>(page)];
        const int col=static_cast<int>(slot)%columns,row=static_cast<int>(slot)/columns;
        const int x0=areaW*col/columns,y0=areaH*row/rows;
        auto place=PlacePage(size.width,size.height,areaW*(col+1)/columns-x0,areaH*(row+1)/rows-y0,dpiX,dpiY,settings.fitToPaper,settings.autoRotate);
        place.x+=x0;place.y+=y0;
        result.push_back({static_cast<int>(slot),page,place});
    }
    return result;
}
std::wstring DefaultPrinterName(){
    DWORD size=0;GetDefaultPrinterW(nullptr,&size);if(!size)return {};
    std::wstring name(size,L'\0');if(!GetDefaultPrinterW(name.data(),&size))return {};
    name.resize(wcslen(name.c_str()));return name;
}
namespace {
// 打印机默认 DEVMODE，并设置纸张方向。返回可移动全局内存（调用方 GlobalFree），失败返回 nullptr。
HGLOBAL MakeDevMode(const std::wstring& printer,bool landscape){
    if(printer.empty())return nullptr;
    std::wstring name=printer;HANDLE handle{};
    if(!OpenPrinterW(name.data(),&handle,nullptr))return nullptr;
    HGLOBAL memory=nullptr;
    const LONG size=DocumentPropertiesW(nullptr,handle,name.data(),nullptr,nullptr,0);
    if(size>0&&(memory=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,static_cast<SIZE_T>(size)))){
        auto* mode=static_cast<DEVMODEW*>(GlobalLock(memory));bool ok=false;
        if(mode&&DocumentPropertiesW(nullptr,handle,name.data(),mode,nullptr,DM_OUT_BUFFER)==IDOK){
            mode->dmFields|=DM_ORIENTATION;mode->dmOrientation=static_cast<short>(landscape?DMORIENT_LANDSCAPE:DMORIENT_PORTRAIT);
            ok=DocumentPropertiesW(nullptr,handle,name.data(),mode,mode,DM_IN_BUFFER|DM_OUT_BUFFER)==IDOK;
        }
        if(mode)GlobalUnlock(memory);
        if(!ok){GlobalFree(memory);memory=nullptr;}
    }
    ClosePrinter(handle);return memory;
}
HGLOBAL MakeDevNames(const std::wstring& printer){
    if(printer.empty())return nullptr;
    const std::wstring driver=L"winspool";
    const size_t header=sizeof(DEVNAMES)/sizeof(wchar_t);
    const size_t chars=header+driver.size()+1+printer.size()+1+1;
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,chars*sizeof(wchar_t));if(!memory)return nullptr;
    auto* names=static_cast<DEVNAMES*>(GlobalLock(memory));if(!names){GlobalFree(memory);return nullptr;}
    auto* text=reinterpret_cast<wchar_t*>(names);
    names->wDriverOffset=static_cast<WORD>(header);names->wDeviceOffset=static_cast<WORD>(header+driver.size()+1);
    names->wOutputOffset=static_cast<WORD>(header+driver.size()+1+printer.size()+1);names->wDefault=DN_DEFAULTPRN;
    memcpy(text+names->wDriverOffset,driver.c_str(),driver.size()*sizeof(wchar_t));
    memcpy(text+names->wDeviceOffset,printer.c_str(),printer.size()*sizeof(wchar_t));
    GlobalUnlock(memory);return memory;
}
}
HDC CreatePrinterDC(const std::wstring& printer,bool landscape){
    HGLOBAL memory=MakeDevMode(printer,landscape);
    const DEVMODEW* mode=memory?static_cast<const DEVMODEW*>(GlobalLock(memory)):nullptr;
    HDC dc=CreateDCW(L"WINSPOOL",printer.c_str(),nullptr,mode);
    if(memory){GlobalUnlock(memory);GlobalFree(memory);}
    return dc;
}
PaperInfo QueryPaper(const std::wstring& requested){
    PaperInfo paper;
    const std::wstring printer=requested.empty()?DefaultPrinterName():requested;
    if(printer.empty())return paper;
    paper.printer=printer;
    HGLOBAL memory=MakeDevMode(printer,false);
    const DEVMODEW* mode=memory?static_cast<const DEVMODEW*>(GlobalLock(memory)):nullptr;
    if(HDC ic=CreateICW(L"WINSPOOL",printer.c_str(),nullptr,mode)){
        const int dpiX=GetDeviceCaps(ic,LOGPIXELSX),dpiY=GetDeviceCaps(ic,LOGPIXELSY);
        const int w=GetDeviceCaps(ic,PHYSICALWIDTH),h=GetDeviceCaps(ic,PHYSICALHEIGHT);
        const int ox=GetDeviceCaps(ic,PHYSICALOFFSETX),oy=GetDeviceCaps(ic,PHYSICALOFFSETY);
        const int aw=GetDeviceCaps(ic,HORZRES),ah=GetDeviceCaps(ic,VERTRES);
        if(dpiX>0&&dpiY>0&&w>0&&h>0&&aw>0&&ah>0){
            const float kx=72.0f/dpiX,ky=72.0f/dpiY;
            paper.width=w*kx;paper.height=h*ky;paper.left=ox*kx;paper.top=oy*ky;
            paper.right=std::max(0.0f,(w-ox-aw)*kx);paper.bottom=std::max(0.0f,(h-oy-ah)*ky);
            if(paper.width>paper.height){  // 驱动报告横向默认纸张时换回纵向
                std::swap(paper.width,paper.height);
                const float l=paper.left,t=paper.top,r=paper.right,b=paper.bottom;paper.left=b;paper.top=l;paper.right=t;paper.bottom=r;
            }
            paper.fromPrinter=true;
        }
        DeleteDC(ic);
    }
    if(memory){GlobalUnlock(memory);GlobalFree(memory);}
    return paper;
}
PaperInfo Oriented(const PaperInfo& portrait,bool landscape){
    if(!landscape)return portrait;
    PaperInfo p=portrait;std::swap(p.width,p.height);
    // 横向：纸张逆时针转 90°，纵向的右边成为顶部。
    p.left=portrait.top;p.top=portrait.right;p.right=portrait.bottom;p.bottom=portrait.left;
    return p;
}
std::wstring PaperName(const PaperInfo& paper){
    const float w=std::min(paper.width,paper.height),h=std::max(paper.width,paper.height);
    auto is=[&](float a,float b){return std::abs(w-a)<3&&std::abs(h-b)<3;};
    if(is(595.28f,841.89f))return L"A4";if(is(841.89f,1190.55f))return L"A3";if(is(419.53f,595.28f))return L"A5";
    if(is(612,792))return L"Letter";if(is(612,1008))return L"Legal";if(is(515.91f,728.5f))return L"B5";
    return std::to_wstring(std::lround(paper.width*25.4f/72))+L" × "+std::to_wstring(std::lround(paper.height*25.4f/72))+L" 毫米";
}
bool AutoLandscape(const PrintSettings& settings,const std::vector<PageInfo>& pages){
    int first=-1;
    for(int p:settings.pages){if(p<0||p>=static_cast<int>(pages.size()))continue;if(settings.parity==1&&p%2==1)continue;if(settings.parity==2&&p%2==0)continue;first=p;break;}
    if(first<0&&!pages.empty()&&settings.pages.empty())first=0;
    if(first<0)return false;
    const auto& page=pages[static_cast<size_t>(first)];
    const auto [columns,rows]=PrintGrid(settings.booklet?2:settings.pagesPerSheet);
    return columns*page.width>rows*page.height*1.02f;
}
bool ResolveLandscape(const PrintSettings& settings,const std::vector<PageInfo>& pages){
    return settings.orientation==2?true:settings.orientation==1?false:AutoLandscape(settings,pages);
}
namespace {
void Grayscale(Bitmap& bitmap){
    for(int y=0;y<bitmap.height;++y)for(int x=0;x<bitmap.width;++x){auto* c=bitmap.bgra.data()+static_cast<size_t>(y)*bitmap.stride+x*4;const auto gray=static_cast<unsigned char>((29*c[0]+150*c[1]+77*c[2]+128)>>8);c[0]=c[1]=c[2]=gray;}
}
// 顺时针旋转 90°：源 (x,y) → 目标 (h-1-y, x)。
Bitmap RotateClockwise(const Bitmap& b){
    Bitmap r{b.height,b.width,b.height*4,std::vector<unsigned char>(static_cast<size_t>(b.width)*b.height*4)};
    for(int y=0;y<b.height;++y){const auto* src=b.bgra.data()+static_cast<size_t>(y)*b.stride;
        for(int x=0;x<b.width;++x)memcpy(r.bgra.data()+(static_cast<size_t>(x)*r.width+(b.height-1-y))*4,src+x*4,4);}
    return r;
}
void Put(Bitmap& out,int x,int y,uint32_t rgb){
    if(x<0||y<0||x>=out.width||y>=out.height)return;
    auto* c=out.bgra.data()+static_cast<size_t>(y)*out.stride+x*4;c[0]=rgb&0xff;c[1]=(rgb>>8)&0xff;c[2]=(rgb>>16)&0xff;c[3]=255;
}
void Frame(Bitmap& out,int x,int y,int w,int h,uint32_t rgb,int dash=0){
    for(int i=0;i<w;++i)if(!dash||(i/dash)%2==0){Put(out,x+i,y,rgb);Put(out,x+i,y+h-1,rgb);}
    for(int i=0;i<h;++i)if(!dash||(i/dash)%2==0){Put(out,x,y+i,rgb);Put(out,x+w-1,y+i,rgb);}
}
}
Bitmap RenderSheetPreview(Document& document,const PrintSettings& settings,const PrintSheet& sheet,const PaperInfo& paper,int longSide){
    if(paper.width<=0||paper.height<=0||longSide<16)throw std::runtime_error("Invalid preview paper");
    const float s=longSide/std::max(paper.width,paper.height);   // 预览像素 / 点
    Bitmap out;out.width=std::max(1,static_cast<int>(std::lround(paper.width*s)));out.height=std::max(1,static_cast<int>(std::lround(paper.height*s)));
    out.stride=out.width*4;out.bgra.assign(static_cast<size_t>(out.stride)*out.height,255);
    const int ax=static_cast<int>(std::lround(paper.left*s)),ay=static_cast<int>(std::lround(paper.top*s));
    const int aw=std::max(1,out.width-ax-static_cast<int>(std::lround(paper.right*s))),ah=std::max(1,out.height-ay-static_cast<int>(std::lround(paper.bottom*s)));
    // 以 1/10 预览像素为设备单位布局，减少整数 DPI 的舍入误差。
    constexpr int k=10;const int dpi=std::max(1,static_cast<int>(std::lround(72.0f*k*s)));
    const auto info=document.Info();
    for(const auto& slot:LayoutSheet(settings,sheet,info.pages,aw*k,ah*k,dpi,dpi)){
        const auto& size=info.pages[static_cast<size_t>(slot.page)];
        const int tx=ax+slot.place.x/k,ty=ay+slot.place.y/k,tw=std::max(1,slot.place.width/k),th=std::max(1,slot.place.height/k);
        const float renderScale=std::clamp(slot.place.scale*dpi/72.0f/k,.02f,8.0f);
        auto bitmap=document.Render(slot.page,renderScale,Rect{size.originX,size.originY,size.width,size.height},-1,false,!settings.annotations);
        if(bitmap.width<=0||bitmap.height<=0)continue;
        if(settings.grayscale)Grayscale(bitmap);
        if(slot.place.rotate)bitmap=RotateClockwise(bitmap);
        for(int y=0;y<th;++y){
            const int dy=ty+y;if(dy<0||dy>=out.height)continue;
            const int sy=std::min(bitmap.height-1,y*bitmap.height/th);
            for(int x=0;x<tw;++x){
                const int dx=tx+x;if(dx<0||dx>=out.width)continue;
                const int sx=std::min(bitmap.width-1,x*bitmap.width/tw);
                const auto* c=bitmap.bgra.data()+static_cast<size_t>(sy)*bitmap.stride+sx*4;auto* d=out.bgra.data()+static_cast<size_t>(dy)*out.stride+dx*4;
                d[0]=c[0];d[1]=c[1];d[2]=c[2];d[3]=255;
            }
        }
        Frame(out,tx,ty,tw,th,0xb8b8b8);
    }
    if(paper.left+paper.top+paper.right+paper.bottom>1)Frame(out,ax,ay,aw,ah,0xcdcdcd,4);
    return out;
}
std::vector<std::pair<int,int>> PageRuns(const std::vector<int>& pages){
    std::vector<std::pair<int,int>> runs;
    for(int p:pages){const int n=p+1;if(!runs.empty()&&runs.back().second+1==n)runs.back().second=n;else runs.push_back({n,n});}
    return runs;
}
HDC ChoosePrinter(HWND owner,int pageCount,int currentPage,PrintSettings& settings,std::wstring& error,bool landscape){
    error.clear();
    if(pageCount<=0)return nullptr;
    PRINTPAGERANGE ranges[16]{};
    PRINTDLGEXW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=owner;
    dialog.Flags=PD_RETURNDC|PD_USEDEVMODECOPIESANDCOLLATE|PD_NOSELECTION;
    dialog.nMinPage=1;dialog.nMaxPage=static_cast<DWORD>(pageCount);
    dialog.nMaxPageRanges=static_cast<DWORD>(std::size(ranges));dialog.lpPageRanges=ranges;
    ranges[0]={1,static_cast<DWORD>(pageCount)};dialog.nPageRanges=0;
    // 预置页码：来自预览对话框的选择（全部页面时不预置）。区间超过 16 段时预置为首尾范围。
    std::vector<int> valid;for(int p:settings.pages)if(p>=0&&p<pageCount)valid.push_back(p);
    if(!valid.empty()&&static_cast<int>(valid.size())<pageCount){
        auto runs=PageRuns(valid);
        if(runs.size()>std::size(ranges)){const int lo=*std::min_element(valid.begin(),valid.end())+1,hi=*std::max_element(valid.begin(),valid.end())+1;runs={{lo,hi}};}
        for(size_t i=0;i<runs.size();++i)ranges[i]={static_cast<DWORD>(runs[i].first),static_cast<DWORD>(runs[i].second)};
        dialog.nPageRanges=static_cast<DWORD>(runs.size());dialog.Flags|=PD_PAGENUMS;
    }
    // 预置纸张方向：默认打印机的 DEVMODE。
    const std::wstring printer=DefaultPrinterName();
    dialog.hDevMode=MakeDevMode(printer,landscape);if(dialog.hDevMode)dialog.hDevNames=MakeDevNames(printer);
    dialog.nCopies=1;dialog.nStartPage=START_PAGE_GENERAL;
    const HRESULT hr=PrintDlgExW(&dialog);
    auto release=[&]{if(dialog.hDevMode)GlobalFree(dialog.hDevMode);if(dialog.hDevNames)GlobalFree(dialog.hDevNames);};
    if(FAILED(hr)){release();error=L"无法打开打印对话框（错误 0x"+std::to_wstring(static_cast<unsigned long>(hr))+L"）。请确认已安装打印机。";return nullptr;}
    if(dialog.dwResultAction!=PD_RESULT_PRINT){release();if(dialog.hDC)DeleteDC(dialog.hDC);return nullptr;}
    settings.pages.clear();
    if(dialog.Flags&PD_CURRENTPAGE)settings.pages.push_back(std::clamp(currentPage,0,pageCount-1));
    else if(dialog.Flags&PD_PAGENUMS){
        for(DWORD i=0;i<dialog.nPageRanges;++i){
            const int from=static_cast<int>(std::min(ranges[i].nFromPage,ranges[i].nToPage)),to=static_cast<int>(std::max(ranges[i].nFromPage,ranges[i].nToPage));
            for(int p=std::max(1,from);p<=std::min(pageCount,to);++p)settings.pages.push_back(p-1);
        }
    }else for(int p=0;p<pageCount;++p)settings.pages.push_back(p);
    release();
    if(!dialog.hDC){error=L"打印机没有返回可用的设备。";return nullptr;}
    if(settings.pages.empty()){DeleteDC(dialog.hDC);error=L"所选页码范围内没有页面。";return nullptr;}
    return dialog.hDC;
}
bool BeginPrintJob(HDC dc,std::wstring_view title,const fs::path& output){
    const std::wstring name(title.empty()?L"LumenPDF":title);
    const std::wstring file=output.wstring();
    DOCINFOW info{};info.cbSize=sizeof(info);info.lpszDocName=name.c_str();
    if(!file.empty())info.lpszOutput=file.c_str();
    return StartDocW(dc,&info)>0;
}
void PrintPages(Document& document,HDC dc,const PrintSettings& settings,const Cancel& cancel,const std::function<void(int,int)>& progress){
    bool pageOpen=false;
    try{
        const auto info=document.Info();
        const int areaW=GetDeviceCaps(dc,HORZRES),areaH=GetDeviceCaps(dc,VERTRES);
        const int dpiX=GetDeviceCaps(dc,LOGPIXELSX),dpiY=GetDeviceCaps(dc,LOGPIXELSY);
        if(areaW<=0||areaH<=0||dpiX<=0||dpiY<=0)throw std::runtime_error("Printer reported an invalid page size");
        SetStretchBltMode(dc,HALFTONE);SetBrushOrgEx(dc,0,0,nullptr);
        const auto sheets=PlanPrint(settings);const int total=static_cast<int>(sheets.size());int done=0;
        for(const auto& sheet:sheets){
            CheckCancel(cancel);if(StartPage(dc)<=0)throw std::runtime_error("Printer rejected a new sheet");pageOpen=true;
            for(const auto& slotPlace:LayoutSheet(settings,sheet,info.pages,areaW,areaH,dpiX,dpiY)){
            const int page=slotPlace.page;const auto& place=slotPlace.place;
            CheckCancel(cancel);
            const auto& size=info.pages[static_cast<size_t>(page)];
            // 渲染分辨率：目标像素密度与上限取小。
            const float deviceDpi=static_cast<float>(std::max(dpiX,dpiY))*place.scale;
            const float renderScale=std::clamp(std::min(deviceDpi,static_cast<float>(settings.maxDpi))/72.0f,.25f,8.0f);

            // 分条渲染，控制单次位图大小；条带按像素行累计映射，避免接缝。
            const int fullW=std::max(1,static_cast<int>(std::lround(size.width*renderScale)));
            const int fullH=std::max(1,static_cast<int>(std::lround(size.height*renderScale)));
            const int bandRows=std::max(64,std::min(fullH,static_cast<int>(12000000/std::max(1,fullW))));
            for(int row=0;row<fullH;row+=bandRows){
                CheckCancel(cancel);
                const int rows=std::min(bandRows,fullH-row);
                const Rect clip{size.originX,size.originY+row/renderScale,size.width,rows/renderScale};
                auto bitmap=document.Render(page,renderScale,clip,-1,false,!settings.annotations);
                if(settings.grayscale)Grayscale(bitmap);
                if(bitmap.width<=0||bitmap.height<=0)continue;
                std::vector<unsigned char> pixels;const unsigned char* bits=bitmap.bgra.data();
                int bw=bitmap.width,bh=bitmap.height,stride=bitmap.stride;
                if(place.rotate){
                    // 顺时针旋转 90°：源 (x,y) → 目标 (bh-1-y, x)。
                    pixels.resize(static_cast<size_t>(bw)*bh*4);
                    for(int y=0;y<bh;++y){const auto* src=bitmap.bgra.data()+static_cast<size_t>(y)*stride;
                        for(int x=0;x<bw;++x){auto* dst=pixels.data()+(static_cast<size_t>(x)*bh+(bh-1-y))*4;memcpy(dst,src+x*4,4);}}
                    std::swap(bw,bh);stride=bw*4;bits=pixels.data();
                }
                BITMAPINFO bmi{};bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bmi.bmiHeader.biWidth=stride/4;bmi.bmiHeader.biHeight=-bh;
                bmi.bmiHeader.biPlanes=1;bmi.bmiHeader.biBitCount=32;bmi.bmiHeader.biCompression=BI_RGB;
                int dx,dy,dw,dh;
                if(!place.rotate){
                    const int top=static_cast<int>(std::lround(static_cast<double>(row)*place.height/fullH));
                    const int bottom=static_cast<int>(std::lround(static_cast<double>(row+rows)*place.height/fullH));
                    dx=place.x;dy=place.y+top;dw=place.width;dh=std::max(1,bottom-top);
                }else{
                    // 旋转后，页面顶部的条带位于纸张右侧。
                    const int right=static_cast<int>(std::lround(static_cast<double>(row)*place.width/fullH));
                    const int left=static_cast<int>(std::lround(static_cast<double>(row+rows)*place.width/fullH));
                    dx=place.x+place.width-left;dy=place.y;dw=std::max(1,left-right);dh=place.height;
                }
                if(StretchDIBits(dc,dx,dy,dw,dh,0,0,bw,bh,bits,&bmi,DIB_RGB_COLORS,SRCCOPY)==0)throw std::runtime_error("Printer rejected page image");
            }
            } // slots
            if(EndPage(dc)<=0)throw std::runtime_error("Printer failed to finish a page");
            pageOpen=false;
            if(progress)progress(++done,total);
        }
        if(EndDoc(dc)<=0)throw std::runtime_error("Printer failed to finish the document");
    }catch(...){
        (void)pageOpen;AbortDoc(dc);throw;
    }
}
}