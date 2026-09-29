#include "clipboard_image.h"
#include "core/signature.h"
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>
#pragma comment(lib,"shell32.lib")
namespace lpdf {
namespace {
UINT PngFormat(){static const UINT f=RegisterClipboardFormatW(L"PNG");return f;}
UINT MimePngFormat(){static const UINT f=RegisterClipboardFormatW(L"image/png");return f;}
constexpr uint64_t kMaxPixels=200ull*1000*1000;
// 剪贴板可能被其它程序短暂占用：重试几次。
bool OpenClipboardRetry(HWND owner){
    for(int i=0;i<10;++i){if(OpenClipboard(owner))return true;std::this_thread::sleep_for(std::chrono::milliseconds(20));}
    return false;
}
struct ClipboardLock{bool open;explicit ClipboardLock(HWND owner):open(OpenClipboardRetry(owner)){}~ClipboardLock(){if(open)CloseClipboard();}};
fs::path TempPng(const fs::path& dir){
    std::error_code error;fs::create_directories(dir,error);
    static unsigned counter=0;
    return dir/(L"clip-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(++counter)+L".png");
}
int Shift(uint32_t mask){if(!mask)return 0;int s=0;while(!(mask&1u)){mask>>=1;++s;}return s;}
uint8_t Channel(uint32_t pixel,uint32_t mask){
    if(!mask)return 0;const int s=Shift(mask);const uint32_t m=mask>>s;const uint32_t v=(pixel&mask)>>s;
    return static_cast<uint8_t>(m==255?v:(v*255+m/2)/m);
}
}
bool IsImageFile(const fs::path& file){
    auto ext=file.extension().wstring();for(auto& c:ext)c=static_cast<wchar_t>(towlower(c));
    for(const wchar_t* e:{L".png",L".jpg",L".jpeg",L".bmp",L".gif",L".tif",L".tiff",L".jxr",L".jpx",L".jp2",L".pnm",L".pbm",L".pgm",L".ppm"})if(ext==e)return true;
    return false;
}
PasteIntent ResolvePasteIntent(PasteIntent requested,bool loaded,bool home,int mode){
    if(requested!=PasteIntent::Auto)return requested;
    if(!loaded||home||mode==3)return PasteIntent::NewDocument;
    return mode==2?PasteIntent::NewPage:PasteIntent::Annotation;
}
bool ClipboardHasContent(){
    return IsClipboardFormatAvailable(PngFormat())||IsClipboardFormatAvailable(MimePngFormat())||IsClipboardFormatAvailable(CF_DIBV5)||
           IsClipboardFormatAvailable(CF_DIB)||IsClipboardFormatAvailable(CF_HDROP);
}
Bitmap DibToBitmap(const void* data,size_t size){
    if(!data||size<sizeof(BITMAPINFOHEADER))throw std::runtime_error("Clipboard bitmap is truncated");
    BITMAPINFOHEADER h;memcpy(&h,data,sizeof(h));
    if(h.biSize<sizeof(BITMAPINFOHEADER)||h.biSize>size)throw std::runtime_error("Clipboard bitmap header is invalid");
    const int width=h.biWidth,height=std::abs(h.biHeight);const bool topDown=h.biHeight<0;
    if(width<=0||height<=0||static_cast<uint64_t>(width)*height>kMaxPixels)throw std::runtime_error("Clipboard bitmap size is invalid");
    const auto* bytes=static_cast<const unsigned char*>(data);
    size_t colors=h.biClrUsed;if(!colors&&h.biBitCount<=8)colors=size_t{1}<<h.biBitCount;
    size_t offset=h.biSize+colors*sizeof(RGBQUAD);
    uint32_t masks[4]{0x00ff0000,0x0000ff00,0x000000ff,0xff000000};
    if(h.biCompression==BI_BITFIELDS){
        if(h.biSize>=sizeof(BITMAPV4HEADER))memcpy(masks,bytes+40,16);   // V4 / V5：掩码在头部
        else{if(h.biSize+12>size)throw std::runtime_error("Clipboard bitmap masks are truncated");memcpy(masks,bytes+h.biSize,12);masks[3]=0;offset+=12;}
    }
    Bitmap out;out.width=width;out.height=height;out.stride=width*4;out.bgra.assign(static_cast<size_t>(out.stride)*height,0);
    if(h.biBitCount==32&&(h.biCompression==BI_RGB||h.biCompression==BI_BITFIELDS)){
        const size_t row=static_cast<size_t>(width)*4;
        if(offset+row*height>size)throw std::runtime_error("Clipboard bitmap pixels are truncated");
        if(h.biCompression==BI_RGB&&h.biSize<sizeof(BITMAPV4HEADER))masks[3]=0xff000000;   // 32 位 BI_RGB：第 4 字节可能是 alpha
        bool anyAlpha=false;
        for(int y=0;y<height;++y){
            const auto* src=bytes+offset+row*static_cast<size_t>(topDown?y:height-1-y);auto* dst=out.bgra.data()+static_cast<size_t>(y)*out.stride;
            for(int x=0;x<width;++x){
                uint32_t p;memcpy(&p,src+x*4,4);
                dst[x*4+0]=Channel(p,masks[2]);dst[x*4+1]=Channel(p,masks[1]);dst[x*4+2]=Channel(p,masks[0]);
                dst[x*4+3]=masks[3]?Channel(p,masks[3]):255;anyAlpha|=dst[x*4+3]!=0;
            }
        }
        if(!anyAlpha)for(size_t i=3;i<out.bgra.size();i+=4)out.bgra[i]=255;   // 全 0 alpha：旧程序不写 alpha，按不透明处理
        return out;
    }
    // 其它格式交给 GDI：画到 32 位自上而下的 DIB 节。
    const size_t headerSize=offset;if(headerSize>size)throw std::runtime_error("Clipboard bitmap palette is truncated");
    if(h.biCompression==BI_RGB||h.biCompression==BI_BITFIELDS){
        const size_t row=((static_cast<size_t>(width)*h.biBitCount+31)/32)*4;
        if(!h.biBitCount||headerSize+row*height>size)throw std::runtime_error("Clipboard bitmap pixels are truncated");
    }else if(h.biCompression==BI_RLE8||h.biCompression==BI_RLE4){
        if(headerSize+h.biSizeImage>size)throw std::runtime_error("Clipboard bitmap pixels are truncated");
    }else throw std::runtime_error("Unsupported clipboard bitmap compression");
    std::vector<unsigned char> info(bytes,bytes+headerSize);   // 头 + 掩码 + 调色板（对齐拷贝）
    BITMAPINFO target{};target.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);target.bmiHeader.biWidth=width;target.bmiHeader.biHeight=-height;
    target.bmiHeader.biPlanes=1;target.bmiHeader.biBitCount=32;target.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;HDC screen=GetDC(nullptr);HDC dc=CreateCompatibleDC(screen);ReleaseDC(nullptr,screen);
    HBITMAP section=CreateDIBSection(dc,&target,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!dc||!section||!bits){if(section)DeleteObject(section);if(dc)DeleteDC(dc);throw std::runtime_error("Cannot convert clipboard bitmap");}
    HGDIOBJ old=SelectObject(dc,section);
    const int lines=SetDIBitsToDevice(dc,0,0,width,height,0,0,0,height,bytes+headerSize,reinterpret_cast<const BITMAPINFO*>(info.data()),DIB_RGB_COLORS);
    GdiFlush();
    if(lines>0)memcpy(out.bgra.data(),bits,out.bgra.size());
    SelectObject(dc,old);DeleteObject(section);DeleteDC(dc);
    if(lines<=0)throw std::runtime_error("Cannot convert clipboard bitmap");
    for(size_t i=3;i<out.bgra.size();i+=4)out.bgra[i]=255;
    return out;
}
bool ReadClipboard(HWND owner,const fs::path& tempDir,ClipboardContent& out,std::wstring& error){
    out={};error.clear();
    ClipboardLock lock(owner);
    if(!lock.open){error=L"剪贴板正被其它程序占用，请稍后再试。";return false;}
    try{
        // 1. PNG 原始字节（截图工具、浏览器“复制图片”等），直接写文件，保留透明。
        for(UINT format:{PngFormat(),MimePngFormat()}){
            if(!format||!IsClipboardFormatAvailable(format))continue;
            HGLOBAL handle=static_cast<HGLOBAL>(GetClipboardData(format));if(!handle)continue;
            const SIZE_T size=GlobalSize(handle);const void* p=GlobalLock(handle);if(!p)continue;
            const bool png=size>=8&&memcmp(p,"\x89PNG\r\n\x1a\n",8)==0;
            if(png){
                const auto file=TempPng(tempDir);
                {std::ofstream f(file,std::ios::binary);f.write(static_cast<const char*>(p),static_cast<std::streamsize>(size));if(!f){GlobalUnlock(handle);throw std::runtime_error("Cannot write clipboard image");}}
                GlobalUnlock(handle);
                out.image=file;out.temporary=true;out.source=L"PNG";return true;
            }
            GlobalUnlock(handle);
        }
        // 2. 位图（PrintScreen、Win+Shift+S、Office 等）。
        for(UINT format:{static_cast<UINT>(CF_DIBV5),static_cast<UINT>(CF_DIB)}){
            if(!IsClipboardFormatAvailable(format))continue;
            HGLOBAL handle=static_cast<HGLOBAL>(GetClipboardData(format));if(!handle)continue;
            const SIZE_T size=GlobalSize(handle);const void* p=GlobalLock(handle);if(!p)continue;
            Bitmap bitmap;
            try{bitmap=DibToBitmap(p,size);}catch(...){GlobalUnlock(handle);throw;}
            GlobalUnlock(handle);
            const auto file=TempPng(tempDir);SaveSignaturePng(bitmap,file);
            out.image=file;out.temporary=true;out.width=bitmap.width;out.height=bitmap.height;out.source=L"DIB";return true;
        }
        // 3. 资源管理器中复制的文件。
        if(IsClipboardFormatAvailable(CF_HDROP)){
            if(HDROP drop=static_cast<HDROP>(GetClipboardData(CF_HDROP))){
                const UINT count=DragQueryFileW(drop,0xFFFFFFFF,nullptr,0);
                for(UINT i=0;i<count;++i){
                    const UINT length=DragQueryFileW(drop,i,nullptr,0);std::wstring name(length+1,L'\0');
                    DragQueryFileW(drop,i,name.data(),length+1);name.resize(length);
                    const fs::path file(name);auto ext=file.extension().wstring();for(auto& c:ext)c=static_cast<wchar_t>(towlower(c));
                    if(ext==L".pdf")out.pdfs.push_back(file);
                    else if(out.image.empty()&&IsImageFile(file))out.image=file;
                }
                if(!out.image.empty()||!out.pdfs.empty()){out.source=L"文件";return true;}
            }
        }
    }catch(const std::exception&){
        if(out.temporary&&!out.image.empty()){std::error_code e;fs::remove(out.image,e);}
        out={};error=L"无法读取剪贴板中的图片（格式损坏或不受支持）。";return false;
    }
    return false;
}
}
