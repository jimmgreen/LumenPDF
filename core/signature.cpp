#include "signature.h"
#include <windows.h>
#include <objidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

namespace lpdf {
namespace {
namespace gp = Gdiplus;
void StartGdiplus(){
    static std::once_flag once;
    std::call_once(once,[]{
        gp::GdiplusStartupInput input;ULONG_PTR token{};
        if(gp::GdiplusStartup(&token,&input,nullptr)!=gp::Ok)throw std::runtime_error("GDI+ is unavailable");
        // 进程结束时由系统回收；不调用 Shutdown，避免与静态析构顺序冲突。
    });
}
gp::Color Argb(uint32_t rgb,BYTE alpha=255){return gp::Color(alpha,static_cast<BYTE>(rgb>>16),static_cast<BYTE>(rgb>>8),static_cast<BYTE>(rgb));}
// GDI+ 位图 → 非预乘 BGRA。
Bitmap FromGdiplus(gp::Bitmap& source){
    Bitmap out;out.width=static_cast<int>(source.GetWidth());out.height=static_cast<int>(source.GetHeight());
    if(out.width<=0||out.height<=0)throw std::runtime_error("Empty image");
    out.stride=out.width*4;out.bgra.resize(static_cast<size_t>(out.stride)*out.height);
    gp::Rect all(0,0,out.width,out.height);gp::BitmapData data{};
    data.Width=out.width;data.Height=out.height;data.Stride=out.stride;data.PixelFormat=PixelFormat32bppARGB;data.Scan0=out.bgra.data();
    if(source.LockBits(&all,gp::ImageLockModeRead|gp::ImageLockModeUserInputBuf,PixelFormat32bppARGB,&data)!=gp::Ok)
        throw std::runtime_error("Unable to read image pixels");
    source.UnlockBits(&data);
    return out;
}
std::unique_ptr<gp::Bitmap> ToGdiplus(const Bitmap& b){
    auto bitmap=std::make_unique<gp::Bitmap>(b.width,b.height,PixelFormat32bppARGB);
    gp::Rect all(0,0,b.width,b.height);gp::BitmapData data{};
    data.Width=b.width;data.Height=b.height;data.Stride=b.stride;data.PixelFormat=PixelFormat32bppARGB;
    data.Scan0=const_cast<unsigned char*>(b.bgra.data());
    if(bitmap->GetLastStatus()!=gp::Ok||bitmap->LockBits(&all,gp::ImageLockModeWrite|gp::ImageLockModeUserInputBuf,PixelFormat32bppARGB,&data)!=gp::Ok)
        throw std::runtime_error("Unable to create image");
    bitmap->UnlockBits(&data);
    return bitmap;
}
std::unique_ptr<gp::Bitmap> Canvas(int w,int h){
    if(w<=0||h<=0||w>8192||h>8192)throw std::runtime_error("Signature is too large");
    auto bitmap=std::make_unique<gp::Bitmap>(w,h,PixelFormat32bppARGB);
    if(bitmap->GetLastStatus()!=gp::Ok)throw std::runtime_error("Out of memory");
    return bitmap;
}
void Quality(gp::Graphics& g){
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);g.SetPixelOffsetMode(gp::PixelOffsetModeHighQuality);
    g.SetCompositingQuality(gp::CompositingQualityHighQuality);g.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
    g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);g.Clear(gp::Color(0,0,0,0));
}
const CLSID PngEncoder{0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
const wchar_t* KindName(SignatureKind k){return k==SignatureKind::Drawn?L"drawn":k==SignatureKind::Typed?L"typed":L"image";}
}

Bitmap CropTransparent(const Bitmap& s,int margin,unsigned char threshold){
    int x0=s.width,y0=s.height,x1=-1,y1=-1;
    for(int y=0;y<s.height;++y){
        const auto* row=s.bgra.data()+static_cast<size_t>(y)*s.stride;
        for(int x=0;x<s.width;++x)if(row[x*4+3]>threshold){x0=std::min(x0,x);x1=std::max(x1,x);y0=std::min(y0,y);y1=std::max(y1,y);}
    }
    if(x1<0)return {};
    x0=std::max(0,x0-margin);y0=std::max(0,y0-margin);x1=std::min(s.width-1,x1+margin);y1=std::min(s.height-1,y1+margin);
    Bitmap out;out.width=x1-x0+1;out.height=y1-y0+1;out.stride=out.width*4;out.bgra.resize(static_cast<size_t>(out.stride)*out.height);
    for(int y=0;y<out.height;++y)std::copy_n(s.bgra.data()+static_cast<size_t>(y+y0)*s.stride+x0*4,out.stride,out.bgra.data()+static_cast<size_t>(y)*out.stride);
    return out;
}

Bitmap RenderDrawnSignature(const std::vector<std::vector<Point>>& strokes,float penWidth,uint32_t rgb,float scale){
    StartGdiplus();
    if(!std::isfinite(penWidth)||penWidth<=0||penWidth>40||!std::isfinite(scale)||scale<=0||scale>16)throw std::runtime_error("Invalid pen");
    float x0=INFINITY,y0=INFINITY,x1=-INFINITY,y1=-INFINITY;size_t count=0;
    for(const auto& s:strokes)for(auto p:s){
        if(!std::isfinite(p.x)||!std::isfinite(p.y))throw std::runtime_error("Invalid stroke point");
        x0=std::min(x0,p.x);y0=std::min(y0,p.y);x1=std::max(x1,p.x);y1=std::max(y1,p.y);++count;
    }
    if(!count)throw std::runtime_error("Draw your signature first");
    const float pad=penWidth*2;
    // 限制输出尺寸：过大的笔迹按比例降低 scale。
    const float span=std::max(x1-x0,y1-y0)+pad*2;if(span*scale>3000)scale=3000/span;
    const int w=std::max(1,static_cast<int>(std::ceil((x1-x0+pad*2)*scale))),h=std::max(1,static_cast<int>(std::ceil((y1-y0+pad*2)*scale)));
    auto bitmap=Canvas(w,h);
    {
        gp::Graphics g(bitmap.get());Quality(g);
        gp::Pen pen(Argb(rgb),penWidth*scale);pen.SetLineCap(gp::LineCapRound,gp::LineCapRound,gp::DashCapRound);pen.SetLineJoin(gp::LineJoinRound);
        gp::SolidBrush brush(Argb(rgb));
        auto map=[&](Point p){return gp::PointF((p.x-x0+pad)*scale,(p.y-y0+pad)*scale);};
        for(const auto& s:strokes){
            if(s.empty())continue;
            if(s.size()==1||std::all_of(s.begin(),s.end(),[&](Point p){return std::hypot(p.x-s[0].x,p.y-s[0].y)<.01f;})){
                const auto c=map(s[0]);const float r=penWidth*scale/2;g.FillEllipse(&brush,c.X-r,c.Y-r,r*2,r*2);continue;
            }
            std::vector<gp::PointF> pts;pts.reserve(s.size());for(auto p:s)pts.push_back(map(p));
            // 轻微张力的样条让鼠标采样的折线更顺滑。
            if(pts.size()>=3)g.DrawCurve(&pen,pts.data(),static_cast<INT>(pts.size()),.35f);else g.DrawLines(&pen,pts.data(),static_cast<INT>(pts.size()));
        }
    }
    auto out=CropTransparent(FromGdiplus(*bitmap),static_cast<int>(std::ceil(penWidth*scale)));
    if(out.bgra.empty())throw std::runtime_error("Draw your signature first");
    return out;
}

Bitmap RenderTypedSignature(std::wstring_view text,std::wstring_view family,uint32_t rgb,float pixelHeight){
    StartGdiplus();
    std::wstring value(text);
    while(!value.empty()&&iswspace(value.back()))value.pop_back();
    while(!value.empty()&&iswspace(value.front()))value.erase(value.begin());
    if(value.empty())throw std::runtime_error("Enter the signature text");
    if(value.size()>40)throw std::runtime_error("Signature text is too long");
    if(!std::isfinite(pixelHeight)||pixelHeight<8||pixelHeight>512)throw std::runtime_error("Invalid size");
    std::unique_ptr<gp::FontFamily> font;
    for(std::wstring name:{std::wstring(family),std::wstring(L"KaiTi"),std::wstring(L"Microsoft YaHei"),std::wstring(L"Segoe UI")}){
        if(name.empty())continue;
        font=std::make_unique<gp::FontFamily>(name.c_str());
        if(font->GetLastStatus()==gp::Ok&&font->IsStyleAvailable(gp::FontStyleRegular))break;
        font.reset();
    }
    if(!font)throw std::runtime_error("No usable font");
    gp::GraphicsPath path;gp::StringFormat format(gp::StringFormat::GenericTypographic());
    path.AddString(value.c_str(),static_cast<INT>(value.size()),font.get(),gp::FontStyleRegular,pixelHeight,gp::PointF(0,0),&format);
    gp::RectF bounds;path.GetBounds(&bounds);
    if(bounds.Width<=0||bounds.Height<=0)throw std::runtime_error("The font cannot display this text");
    const float pad=pixelHeight*.08f;
    auto bitmap=Canvas(static_cast<int>(std::ceil(bounds.Width+pad*2)),static_cast<int>(std::ceil(bounds.Height+pad*2)));
    {
        gp::Graphics g(bitmap.get());Quality(g);
        gp::Matrix shift;shift.Translate(pad-bounds.X,pad-bounds.Y);path.Transform(&shift);
        gp::SolidBrush brush(Argb(rgb));g.FillPath(&brush,&path);
    }
    auto out=CropTransparent(FromGdiplus(*bitmap),static_cast<int>(pad));
    if(out.bgra.empty())throw std::runtime_error("The font cannot display this text");
    return out;
}

bool SignatureFontInstalled(std::wstring_view family){
    StartGdiplus();
    const std::wstring name(family);gp::FontFamily font(name.c_str());
    return font.GetLastStatus()==gp::Ok&&font.IsStyleAvailable(gp::FontStyleRegular);
}

Bitmap ImportSignatureImage(const fs::path& image,bool removeBackground,int maxSide){
    StartGdiplus();
    gp::Bitmap source(image.c_str());
    if(source.GetLastStatus()!=gp::Ok)throw std::runtime_error("Unable to read the image");
    int w=static_cast<int>(source.GetWidth()),h=static_cast<int>(source.GetHeight());
    if(w<=0||h<=0)throw std::runtime_error("Unable to read the image");
    const float factor=std::min(1.0f,static_cast<float>(maxSide)/std::max(w,h));
    w=std::max(1,static_cast<int>(std::lround(w*factor)));h=std::max(1,static_cast<int>(std::lround(h*factor)));
    auto scaled=Canvas(w,h);
    {gp::Graphics g(scaled.get());Quality(g);g.DrawImage(&source,gp::Rect(0,0,w,h),0,0,static_cast<INT>(source.GetWidth()),static_cast<INT>(source.GetHeight()),gp::UnitPixel);}
    auto b=FromGdiplus(*scaled);
    if(removeBackground){
        // 纸张底色：取亮度直方图的 90 分位（照片里的纸往往偏灰），墨迹阈值取其下方一段过渡。
        std::vector<int> hist(256);size_t opaque=0;
        auto lum=[](const unsigned char* p){return (p[2]*299+p[1]*587+p[0]*114)/1000;};
        for(size_t i=0;i+3<b.bgra.size();i+=4)if(b.bgra[i+3]>128){++hist[lum(&b.bgra[i])];++opaque;}
        if(opaque){
            int paper=255;size_t seen=0;
            for(int v=0;v<256;++v){seen+=hist[v];if(seen>=opaque*9/10){paper=v;break;}}
            paper=std::max(paper,120);
            const float ink=std::max(0.0f,paper-110.0f);
            // 背景色（用于去除半透明边缘的白边）：接近纸色的像素平均。
            double br=0,bg=0,bb=0;size_t n=0;
            for(size_t i=0;i+3<b.bgra.size();i+=4)if(b.bgra[i+3]>128&&lum(&b.bgra[i])>=paper-12){bb+=b.bgra[i];bg+=b.bgra[i+1];br+=b.bgra[i+2];++n;}
            const float back[3]{n?static_cast<float>(bb/n):255.f,n?static_cast<float>(bg/n):255.f,n?static_cast<float>(br/n):255.f};
            for(size_t i=0;i+3<b.bgra.size();i+=4){
                auto* p=&b.bgra[i];
                float a=std::clamp((paper-6-static_cast<float>(lum(p)))/std::max(1.0f,paper-6-ink),0.0f,1.0f);
                a=a*a*(3-2*a);   // smoothstep：让笔画边缘柔和
                for(int c=0;c<3;++c){const float v=a>0?(p[c]-back[c]*(1-a))/a:0;p[c]=static_cast<unsigned char>(std::clamp(v,0.0f,255.0f));}
                p[3]=static_cast<unsigned char>(std::lround(p[3]*a));
            }
        }
    }
    auto out=CropTransparent(b,std::max(2,std::max(b.width,b.height)/100));
    if(out.bgra.empty())throw std::runtime_error("No signature strokes were found in the image");
    return out;
}

void SaveSignaturePng(const Bitmap& bitmap,const fs::path& destination){
    StartGdiplus();
    if(bitmap.width<=0||bitmap.height<=0||bitmap.bgra.size()<static_cast<size_t>(bitmap.stride)*bitmap.height)throw std::runtime_error("Empty signature");
    std::error_code error;fs::create_directories(destination.parent_path(),error);
    const auto temporary=UniquePath(destination.parent_path(),L".tmp");
    {
        auto image=ToGdiplus(bitmap);
        if(image->Save(temporary.c_str(),&PngEncoder,nullptr)!=gp::Ok){fs::remove(temporary,error);throw std::runtime_error("Unable to save the signature image");}
    }
    try{AtomicReplace(temporary,destination);}catch(...){fs::remove(temporary,error);throw;}
}

Bitmap LoadSignaturePng(const fs::path& file){
    StartGdiplus();
    // 先读入内存，避免 GDI+ 持有文件句柄导致之后无法删除。
    const auto bytes=ReadBytes(file);
    IStream* stream=SHCreateMemStream(bytes.data(),static_cast<UINT>(bytes.size()));
    if(!stream)throw std::runtime_error("Out of memory");
    Bitmap out;
    {
        gp::Bitmap image(stream);
        const bool ok=image.GetLastStatus()==gp::Ok;
        if(ok)out=FromGdiplus(image);
        stream->Release();
        if(!ok)throw std::runtime_error("Unable to read the signature image");
    }
    return out;
}

Bitmap Premultiplied(Bitmap b){
    for(size_t i=0;i+3<b.bgra.size();i+=4){const unsigned a=b.bgra[i+3];for(int c=0;c<3;++c)b.bgra[i+c]=static_cast<unsigned char>((b.bgra[i+c]*a+127)/255);}
    return b;
}

Bitmap ScaledToFit(const Bitmap& s,int maxW,int maxH){
    if(s.width<=0||s.height<=0||maxW<=0||maxH<=0)return {};
    const float f=std::min({1.0f,static_cast<float>(maxW)/s.width,static_cast<float>(maxH)/s.height});
    if(f>=1)return s;
    StartGdiplus();
    const int w=std::max(1,static_cast<int>(std::lround(s.width*f))),h=std::max(1,static_cast<int>(std::lround(s.height*f)));
    auto source=ToGdiplus(s);auto target=Canvas(w,h);
    {gp::Graphics g(target.get());Quality(g);g.DrawImage(source.get(),gp::Rect(0,0,w,h),0,0,s.width,s.height,gp::UnitPixel);}
    return FromGdiplus(*target);
}

std::vector<SignatureEntry> ListSignatures(const fs::path& folder){
    std::vector<SignatureEntry> out;std::error_code error;
    if(folder.empty()||!fs::is_directory(folder,error))return out;
    for(const auto& item:fs::directory_iterator(folder,error)){
        if(!item.is_regular_file(error)||item.path().extension()!=L".png")continue;
        const auto stem=item.path().stem().wstring();
        SignatureEntry e;e.file=item.path();
        if(stem.rfind(L"sig-drawn-",0)==0)e.kind=SignatureKind::Drawn;
        else if(stem.rfind(L"sig-typed-",0)==0)e.kind=SignatureKind::Typed;
        else if(stem.rfind(L"sig-image-",0)==0)e.kind=SignatureKind::Image;
        else continue;
        try{e.created=std::stoll(stem.substr(10));}catch(...){continue;}
        out.push_back(e);
    }
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return a.created!=b.created?a.created>b.created:a.file<b.file;});
    return out;
}

SignatureEntry AddSignature(const fs::path& folder,SignatureKind kind,const Bitmap& bitmap){
    if(folder.empty())throw std::runtime_error("The signature folder is unavailable");
    if(ListSignatures(folder).size()>=MaxSignatures)throw std::runtime_error("Signature library is full");
    int64_t stamp=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    SignatureEntry e;e.kind=kind;
    std::error_code error;
    for(;;++stamp){e.file=folder/(std::wstring(L"sig-")+KindName(kind)+L"-"+std::to_wstring(stamp)+L".png");if(!fs::exists(e.file,error))break;}
    e.created=stamp;
    SaveSignaturePng(bitmap,e.file);
    return e;
}

void RemoveSignature(const SignatureEntry& entry){
    std::error_code error;fs::remove(entry.file,error);
    if(error)throw std::runtime_error("Unable to delete the signature");
}

std::vector<std::vector<Point>> CheckMarkStrokes(Rect r){
    return {{{r.x+r.w*.12f,r.y+r.h*.55f},{r.x+r.w*.40f,r.y+r.h*.84f},{r.x+r.w*.90f,r.y+r.h*.16f}}};
}
std::vector<std::vector<Point>> CrossMarkStrokes(Rect r){
    return {{{r.x+r.w*.18f,r.y+r.h*.18f},{r.x+r.w*.82f,r.y+r.h*.82f}},{{r.x+r.w*.82f,r.y+r.h*.18f},{r.x+r.w*.18f,r.y+r.h*.82f}}};
}
}
