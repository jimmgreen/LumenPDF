// 真正的涂黑：文字 / 批注 / 弹出注释 / 表单值 / 元数据 / 图片像素被永久删除，未标记内容保留；
// 逐个对象（含解压后的流）扫描输出文件，确认原文不在文件任何位置；原文件与当前文档不变。
#include "core/document.h"
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
using namespace lpdf;
namespace {
int assertions=0;
void Require(bool okay,const char* why){++assertions;if(!okay)throw std::runtime_error(why);}
template<class F> bool Throws(F f){try{f();}catch(...){return true;}return false;}
const char kSecret[]="SECRET-4711";

// fz_try 基于 setjmp：本函数内带析构的对象都在 fz_try 之外声明，不会被 longjmp 跳过析构。
#pragma warning(disable:4611)
// 文件中任一对象（字典 / 字符串序列化后）或任一解压后的流包含 needle 时返回 true。
bool FileContains(const fs::path& path,const char* needle){
    fz_context* ctx=fz_new_context(nullptr,nullptr,FZ_STORE_DEFAULT);
    if(!ctx)throw std::runtime_error("mupdf context");
    bool found=false;const size_t n=strlen(needle);
    auto has=[&](const unsigned char* data,size_t len){return len>=n&&std::search(data,data+len,needle,needle+n)!=data+len;};
    pdf_document* doc=nullptr;const std::string file=path.string();const char* name=file.c_str();
    fz_try(ctx){
        fz_register_document_handlers(ctx);
        doc=pdf_open_document(ctx,name);
        const int len=pdf_xref_len(ctx,doc);
        for(int i=1;i<len&&!found;++i){
            pdf_obj* obj=nullptr;fz_buffer* buf=nullptr;char* text=nullptr;
            fz_try(ctx){
                obj=pdf_load_object(ctx,doc,i);
                size_t tl=0;text=pdf_sprint_obj(ctx,nullptr,0,&tl,obj,1,0);
                if(has(reinterpret_cast<unsigned char*>(text),tl))found=true;
                if(!found&&pdf_is_stream(ctx,obj)){
                    buf=pdf_load_stream_number(ctx,doc,i);
                    unsigned char* data=nullptr;const size_t bl=fz_buffer_storage(ctx,buf,&data);
                    if(has(data,bl))found=true;
                }
            }
            fz_always(ctx){fz_free(ctx,text);fz_drop_buffer(ctx,buf);pdf_drop_obj(ctx,obj);}
            fz_catch(ctx){/* 空闲 / 损坏的对象槽跳过 */}
        }
    }
    fz_always(ctx){pdf_drop_document(ctx,doc);}
    fz_catch(ctx){fz_drop_context(ctx);throw std::runtime_error("cannot scan PDF objects");}
    fz_drop_context(ctx);
    return found;
}
FormField Field(Document& d,int page,std::wstring_view name){
    for(auto& f:d.FormFields(page))if(f.name==name)return f;
    throw std::runtime_error("field not found");
}
// 页面坐标点处的像素（BGRA）。
const unsigned char* Pixel(const Bitmap& b,const PageInfo& info,float scale,Point p){
    const int x=static_cast<int>((p.x-info.originX)*scale),y=static_cast<int>((p.y-info.originY)*scale);
    return &b.bgra[static_cast<size_t>(std::clamp(y,0,b.height-1))*b.stride+std::clamp(x,0,b.width-1)*4];
}
void WriteRedBmp(const fs::path& path,int w,int h){
    const int row=(w*3+3)&~3;std::vector<unsigned char> px(static_cast<size_t>(row)*h,0);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){auto* p=&px[static_cast<size_t>(y)*row+x*3];p[0]=30;p[1]=30;p[2]=220;}
    BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};
    file.bfType=0x4D42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+static_cast<DWORD>(px.size());
    info.biSize=sizeof(info);info.biWidth=w;info.biHeight=h;info.biPlanes=1;info.biBitCount=24;info.biCompression=BI_RGB;
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(&file),sizeof(file));out.write(reinterpret_cast<const char*>(&info),sizeof(info));
    out.write(reinterpret_cast<const char*>(px.data()),static_cast<std::streamsize>(px.size()));
}
bool Reddish(const unsigned char* p){return p[2]>150&&p[1]<90&&p[0]<90;}
bool Dark(const unsigned char* p){return p[0]<60&&p[1]<60&&p[2]<60;}
bool Light(const unsigned char* p){return p[0]>200&&p[1]>200&&p[2]>200;}
}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc==4&&std::wstring_view(argv[1])==L"--ui"){
            // 启动真实 LumenPDF（--smoke）：搜索 → 标记全部结果（去重）→ 框选回调增删 → 应用对话框 → 应用并另存。
            const fs::path exe=fs::absolute(argv[2]),out=fs::absolute(argv[3]);
            std::error_code error;fs::remove_all(out,error);fs::create_directories(out);
            const auto fixture=out/L"ui-source.pdf";
            Document::TextToPdf(L"Intro text\nSECRET-1 first line\nMiddle public text\nSECRET-2 second line\nThe end",fixture);
            const auto before=ReadBytes(fixture);const auto result=out/L"redacted-ui.pdf";
            SetEnvironmentVariableW(L"LPDF_REDACT_SMOKE",result.c_str());
            SetEnvironmentVariableW(L"LPDF_SMOKE_TIMEOUT",L"40");
            SetEnvironmentVariableW(L"LPDF_NO_DEFAULT_PROMPT",L"1");
            std::wstring command=L"\""+exe.wstring()+L"\" --smoke \""+fixture.wstring()+L"\"";
            STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
            Require(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,out.c_str(),&si,&pi)!=0,"cannot start LumenPDF");
            const DWORD wait=WaitForSingleObject(pi.hProcess,55000);
            if(wait!=WAIT_OBJECT_0)TerminateProcess(pi.hProcess,3);
            CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
            Require(wait==WAIT_OBJECT_0,"LumenPDF redaction smoke timed out");
            if(!fs::exists(result)){
                std::string why="redaction smoke did not save";
                const auto failFile=fs::path(result.wstring()+L".fail.txt");
                if(fs::exists(failFile)){const auto bytes=ReadBytes(failFile);why+=": "+std::string(bytes.begin(),bytes.end());}
                throw std::runtime_error(why);
            }
            Require(ReadBytes(fixture)==before,"source PDF must stay untouched");
            Document r;r.Open(result);
            Require(r.Search(L"SECRET").empty(),"UI redaction removed every search hit");
            const auto text=r.Text(0);
            Require(text.find(L"Middle public text")!=std::wstring::npos&&text.find(L"Intro text")!=std::wstring::npos,"unmarked text kept");
            Require(!FileContains(result,"SECRET"),"secret absent from every object of the UI output");
            std::cout<<"PASS redaction UI: "<<assertions<<" assertions; search hits, dedupe, canvas callbacks, dialog, export, verification.\n";
            return 0;
        }
        if(argc<3)throw std::runtime_error("usage: redaction_tests <fixtures> <output>");
        const fs::path fixtures=argv[1],out=argv[2];
        std::error_code error;fs::remove_all(out,error);fs::create_directories(out);

        // ---- 文字 + 批注 + 元数据 ----
        const auto source=out/L"text.pdf";
        Document::TextToPdf(L"Public line: hello world\nSECRET-4711 account number\nAnother public line",source);
        const auto sourceBytes=ReadBytes(source);
        Document d;d.Open(source);
        auto hits=d.SearchPage(0,L"SECRET-4711");
        Require(hits.size()==1,"fixture secret found once");
        const Rect secret=hits[0].bounds;
        // 与涂黑区重叠的便笺（其正文复制了秘密）和不重叠的文本框。
        d.AddAnnotation(0,Tool::Note,{secret.x+2,secret.y,20,20},L"copied: SECRET-4711");
        d.AddAnnotation(0,Tool::Text,{secret.x,secret.y+160,160,24},L"keep-me");
        auto meta=d.Metadata();meta.title=L"SECRET-4711 title";meta.author=L"SECRET-4711 author";d.SetMetadata(meta);
        const auto plainCopy=out/L"unredacted-copy.pdf";d.Save(plainCopy);
        const bool detectable=FileContains(plainCopy,kSecret);
        Require(detectable,"scanner must see the secret in an unredacted copy (otherwise the scan proves nothing)");

        // 参数错误：不产生输出文件。
        const auto bad=out/L"bad.pdf";
        Require(Throws([&]{d.ExportRedacted({},{},bad);})&&!fs::exists(bad),"no marks is rejected");
        Require(Throws([&]{d.ExportRedacted({{5,secret}},{},bad);})&&!fs::exists(bad),"page out of range is rejected");
        Require(Throws([&]{d.ExportRedacted({{0,{10,10,0,0}}},{},bad);})&&!fs::exists(bad),"empty area is rejected");

        const auto redacted=out/L"text-redacted.pdf";
        Rect mark=secret;mark.x-=1;mark.y-=1;mark.w+=2;mark.h+=2;
        const auto result=d.ExportRedacted({{0,mark}},{},redacted);
        Require(result.marks==1&&result.pages==1,"result counts");
        Require(result.annotationsRemoved>=1,"overlapping note removed");
        Require(ReadBytes(source)==sourceBytes,"source file untouched");
        Require(d.SearchPage(0,L"SECRET-4711").size()==1,"current document untouched");

        Document r;r.Open(redacted);
        Require(r.Search(L"SECRET").empty()&&r.Search(L"4711").empty(),"secret text removed");
        const auto text=r.Text(0);
        Require(text.find(L"Public line")!=std::wstring::npos&&text.find(L"Another public line")!=std::wstring::npos,"unmarked text kept");
        const auto annots=r.Annotations(0);
        Require(std::none_of(annots.begin(),annots.end(),[](const Annotation& a){return a.type==Tool::Note;}),"note annotation gone");
        Require(std::any_of(annots.begin(),annots.end(),[](const Annotation& a){return a.type==Tool::Text&&a.text==L"keep-me";}),"non-overlapping annotation kept");
        Require(r.Metadata().title.empty()&&r.Metadata().author.empty(),"metadata cleared");
        Require(!FileContains(redacted,kSecret),"secret must not appear in any object or stream of the output");
        {
            const float scale=2;const auto page=r.Render(0,scale);const auto info=r.Info().pages[0];
            Require(Dark(Pixel(page,info,scale,{secret.x+secret.w/2,secret.y+secret.h/2})),"black box drawn over the redaction");
        }
        // clearMetadata=false 时保留元数据。
        {
            const auto keep=out/L"keep-meta.pdf";RedactionOptions o;o.clearMetadata=false;
            d.ExportRedacted({{0,mark}},o,keep);Document k;k.Open(keep);
            Require(k.Metadata().title==L"SECRET-4711 title","metadata kept on request");
        }

        // ---- 表单字段 ----
        {
            Document f;f.Open(fixtures/L"form.pdf");
            const auto name=Field(f,0,L"name");
            f.SetFieldValue(0,name.id,L"SECRET-4711");
            const auto out2=out/L"form-redacted.pdf";
            const auto res=f.ExportRedacted({{0,name.bounds}},{},out2);
            Require(res.fieldsCleared==1,"overlapping field cleared");
            Document g;g.Open(out2);
            const auto after=Field(g,0,L"name");
            Require(after.value.empty()&&after.readOnly,"field value cleared and locked");
            Require(Field(g,0,L"color").value==L"Green","other fields untouched");
            Require(!FileContains(out2,kSecret),"field value must not remain anywhere in the file");
        }

        // ---- 图片：只涂黑被覆盖的像素 / 整张删除 ----
        {
            const auto bmp=out/L"red.bmp";WriteRedBmp(bmp,200,200);
            const auto img=out/L"image.pdf";Document::ImageToPdf(bmp,img,false);
            Document i;i.Open(img);const auto info=i.Info().pages[0];
            const Rect half{info.originX,info.originY,info.width/2,info.height};
            const Point covered{info.originX+info.width*.25f,info.originY+info.height*.5f};
            const Point open{info.originX+info.width*.75f,info.originY+info.height*.5f};
            {
                const float scale=1;const auto page=i.Render(0,scale);
                Require(Reddish(Pixel(page,info,scale,covered))&&Reddish(Pixel(page,info,scale,open)),"fixture image renders red");
            }
            const auto px=out/L"image-pixels.pdf";i.ExportRedacted({{0,half}},{},px);
            {
                Document p;p.Open(px);const float scale=1;const auto page=p.Render(0,scale);
                Require(Dark(Pixel(page,p.Info().pages[0],scale,covered)),"covered pixels blacked out");
                Require(Reddish(Pixel(page,p.Info().pages[0],scale,open)),"uncovered part of the image kept");
            }
            RedactionOptions whole;whole.removeWholeImages=true;
            const auto rm=out/L"image-removed.pdf";i.ExportRedacted({{0,half}},whole,rm);
            {
                Document p;p.Open(rm);const float scale=1;const auto page=p.Render(0,scale);
                Require(Light(Pixel(page,p.Info().pages[0],scale,open)),"whole image removed on request");
            }
        }
        std::cout<<"PASS redaction: "<<assertions<<" assertions; text, notes, metadata, form values, image pixels / removal, object scan, source untouched.\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL redaction after "<<assertions<<" assertions: "<<e.what()<<'\n';return 1;}
}
