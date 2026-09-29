// 填写与签名：签名图片生成 / 去背景 / PNG 往返 / 签名库，以及放置到 PDF（透明图片批注、日期文字、✓ / ✗ 手绘）。
#include "core/document.h"
#include "core/signature.h"
#include <windows.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
using namespace lpdf;
namespace {
int assertions=0;
void Require(bool okay,const char* why){++assertions;if(!okay)throw std::runtime_error(why);}
template<class F> bool Throws(F f){try{f();}catch(...){return true;}return false;}
unsigned char Alpha(const Bitmap& b,int x,int y){return b.bgra[static_cast<size_t>(y)*b.stride+x*4+3];}
size_t Opaque(const Bitmap& b,unsigned char threshold=128){size_t n=0;for(size_t i=3;i<b.bgra.size();i+=4)if(b.bgra[i]>threshold)++n;return n;}
// 32 位不透明 BMP（GDI+ 按 RGB 读取），模拟扫描 / 拍照的签名。
void WriteBmp(const fs::path& path,int w,int h,uint32_t paper,uint32_t ink){
    std::vector<unsigned char> px(static_cast<size_t>(w)*h*4);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){
        const bool stroke=(y>=90&&y<100&&x>=60&&x<340)||(x>=190&&x<200&&y>=50&&y<150);   // 十字笔画
        const uint32_t c=stroke?ink:paper;auto* p=&px[(static_cast<size_t>(y)*w+x)*4];
        p[0]=c&255;p[1]=(c>>8)&255;p[2]=(c>>16)&255;p[3]=255;
    }
    BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);file.bfSize=file.bfOffBits+static_cast<DWORD>(px.size());
    BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=w;info.biHeight=-h;info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(&file),sizeof(file));out.write(reinterpret_cast<const char*>(&info),sizeof(info));
    out.write(reinterpret_cast<const char*>(px.data()),static_cast<std::streamsize>(px.size()));
}
}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc==5&&std::wstring_view(argv[1])==L"--ui"){
            // 启动真实的 LumenPDF（--smoke），由程序内冒烟走完整放置链路并另存，再核对结果。
            const fs::path exe=argv[2],fixture=argv[3],out=argv[4];
            std::error_code error;fs::remove_all(out,error);fs::create_directories(out/L"library");
            const auto result=out/L"signed-ui.pdf";
            SetEnvironmentVariableW(L"LPDF_SIGNATURE_SMOKE",result.c_str());
            SetEnvironmentVariableW(L"LPDF_SIGNATURE_DIR",(out/L"library").c_str());
            SetEnvironmentVariableW(L"LPDF_SMOKE_TIMEOUT",L"40");
            std::wstring command=L"\""+exe.wstring()+L"\" --smoke \""+fixture.wstring()+L"\"";
            STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
            Require(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,out.c_str(),&si,&pi)!=0,"cannot start LumenPDF");
            const DWORD wait=WaitForSingleObject(pi.hProcess,55000);
            if(wait!=WAIT_OBJECT_0)TerminateProcess(pi.hProcess,3);
            CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
            Require(wait==WAIT_OBJECT_0,"LumenPDF signature smoke timed out");
            Require(fs::exists(result),"signature smoke did not save (a step failed; see the LumenPDF log)");
            Require(ListSignatures(out/L"library").size()==1,"UI signature was not stored in the library");
            Document saved;saved.Open(result);const auto all=saved.Annotations(0);
            Require(std::count_if(all.begin(),all.end(),[](const auto& a){return a.type==Tool::Image;})==1,"UI signature placement");
            Require(std::any_of(all.begin(),all.end(),[](const auto& a){return a.type==Tool::Text&&a.text==L"2026-09-28";}),"UI date placement");
            Require(std::count_if(all.begin(),all.end(),[](const auto& a){return a.type==Tool::Ink;})==2,"UI check / cross placement (and Esc cancel)");
            const auto sig=*std::find_if(all.begin(),all.end(),[](const auto& a){return a.type==Tool::Image;});
            Require(sig.bounds.w<=150.5f&&sig.bounds.h<=60.5f&&sig.bounds.w>100,"click placement uses the default signature size");
            std::cout<<"PASS signature UI: "<<assertions<<" assertions; library, click / drag placement, date, check / cross, Esc cancel, dialog paint, save.\n";
            return 0;
        }
        const fs::path root=argc>1?fs::path(argv[1]):fs::temp_directory_path()/L"lpdf-signature-tests";
        std::error_code error;fs::remove_all(root,error);fs::create_directories(root);

        // 1. 手写签名：透明背景、按颜色着色、裁掉四周空白。
        const std::vector<std::vector<Point>> strokes{{{10,60},{40,20},{70,70},{100,25},{130,65}},{{150,40}}};
        const auto drawn=RenderDrawnSignature(strokes,2.6f,0x1f4fbf);
        Require(drawn.width>300&&drawn.height>100&&drawn.stride==drawn.width*4,"drawn signature size");
        Require(Alpha(drawn,0,0)==0&&Alpha(drawn,drawn.width-1,drawn.height-1)==0,"drawn signature corners must be transparent");
        Require(Opaque(drawn)>500,"drawn signature has too few ink pixels");
        bool blue=false;for(size_t i=0;i+3<drawn.bgra.size();i+=4)if(drawn.bgra[i+3]==255&&drawn.bgra[i]==0xbf&&drawn.bgra[i+1]==0x4f&&drawn.bgra[i+2]==0x1f){blue=true;break;}
        Require(blue,"drawn signature must keep the chosen ink colour");
        Require(Throws([]{RenderDrawnSignature({},2.6f,0);}),"empty drawing must be rejected");
        Require(Throws([]{RenderDrawnSignature({{{0,0},{std::numeric_limits<float>::quiet_NaN(),1}}},2.6f,0);}),"NaN stroke must be rejected");
        // 单击成点。
        Require(Opaque(RenderDrawnSignature({{{5,5}}},3,0))>20,"a tap must render a dot");

        // 2. 文字签名。
        const auto typed=RenderTypedSignature(L"  张三 Lumen ",L"KaiTi",0x1f1f1f,120);
        Require(typed.width>typed.height&&typed.height>60&&Opaque(typed)>800,"typed signature rendering");
        Require(Alpha(typed,0,0)==0,"typed signature background must be transparent");
        Require(Throws([]{RenderTypedSignature(L"   ",L"KaiTi",0);}),"blank typed signature must be rejected");
        Require(Throws([]{RenderTypedSignature(std::wstring(41,L'签'),L"KaiTi",0);}),"overlong typed signature must be rejected");
        Require(!RenderTypedSignature(L"Fallback",L"No Such Font 123",0).bgra.empty(),"missing font must fall back");
        Require(SignatureFontInstalled(L"Microsoft YaHei")&&!SignatureFontInstalled(L"No Such Font 123"),"font availability check");

        // 3. 图片导入：白纸 / 灰纸去背景，裁剪到笔迹附近；不去背景时保持不透明。
        for(uint32_t paper:{0xffffffu,0xd8d6d0u}){
            const auto bmp=root/(L"scan-"+std::to_wstring(paper)+L".bmp");WriteBmp(bmp,400,200,paper,0x202020);
            const auto cleaned=ImportSignatureImage(bmp,true);
            Require(cleaned.width<320&&cleaned.width>=280&&cleaned.height<120&&cleaned.height>=100,"background removal must crop to the strokes");
            Require(Alpha(cleaned,2,2)==0,"paper must become transparent");
            // 十字中心附近（裁剪后的中点 ±8 像素）必有笔迹：取其中最不透明的像素。
            int cx=cleaned.width/2,cy=cleaned.height/2;
            for(int y=cleaned.height/2-8;y<=cleaned.height/2+8;++y)for(int x=cleaned.width/2-8;x<=cleaned.width/2+8;++x)if(Alpha(cleaned,x,y)>Alpha(cleaned,cx,cy)){cx=x;cy=y;}
            Require(Alpha(cleaned,cx,cy)>200,"ink must stay opaque");
            const auto* p=&cleaned.bgra[static_cast<size_t>(cy)*cleaned.stride+cx*4];
            Require(p[0]<80&&p[1]<80&&p[2]<80,"ink colour must stay dark (no white fringe)");
            const auto raw=ImportSignatureImage(bmp,false);
            Require(raw.width==400&&raw.height==200&&Alpha(raw,0,0)==255,"import without background removal keeps the image");
        }
        const auto blank=root/L"blank.bmp";WriteBmp(blank,50,50,0xffffff,0xffffff);
        Require(Throws([&]{ImportSignatureImage(blank,true);}),"an image without strokes must be rejected");
        Require(Throws([&]{ImportSignatureImage(root/L"missing.png",true);}),"missing image must be rejected");

        // 4. PNG 往返 + 原子写入。
        const auto png=root/L"drawn.png";SaveSignaturePng(drawn,png);
        const auto loaded=LoadSignaturePng(png);
        Require(loaded.width==drawn.width&&loaded.height==drawn.height&&loaded.bgra==drawn.bgra,"PNG round trip must be lossless");
        Require(fs::remove(png),"PNG must not stay locked after loading");
        SaveSignaturePng(drawn,png);
        const auto premul=Premultiplied(drawn);
        for(size_t i=0;i+3<premul.bgra.size();i+=4)if(premul.bgra[i]>premul.bgra[i+3]){Require(false,"premultiplied colour exceeds alpha");}
        const auto small=ScaledToFit(drawn,80,40);Require(small.width<=80&&small.height<=40&&small.width>0,"preview scaling");

        // 5. 签名库：新→旧、上限、删除。
        const auto library=root/L"library";
        Require(ListSignatures(library).empty(),"missing library lists nothing");
        const auto first=AddSignature(library,SignatureKind::Drawn,drawn);
        Sleep(5);
        const auto second=AddSignature(library,SignatureKind::Typed,typed);
        {std::ofstream(library/L"notes.txt")<<"ignored";std::ofstream(library/L"sig-bad-x.png")<<"ignored";}
        auto list=ListSignatures(library);
        Require(list.size()==2&&list[0].file==second.file&&list[0].kind==SignatureKind::Typed&&list[1].kind==SignatureKind::Drawn,"library order / filtering");
        while(ListSignatures(library).size()<MaxSignatures)AddSignature(library,SignatureKind::Image,drawn);
        Require(Throws([&]{AddSignature(library,SignatureKind::Drawn,drawn);})&&ListSignatures(library).size()==MaxSignatures,"library limit");
        RemoveSignature(first);
        list=ListSignatures(library);
        Require(list.size()==MaxSignatures-1&&std::none_of(list.begin(),list.end(),[&](const auto& e){return e.file==first.file;}),"remove signature");
        for(const auto& f:fs::directory_iterator(library))Require(f.path().extension()!=L".tmp","no temporary files may remain");

        // 6. 放置到 PDF：签名是透明图片批注（盖在红色矩形上，空白处仍透出红色），可保存重开。
        const auto source=root/L"form.pdf";Document::TextToPdf(L"Signature placement\nName: ______  Date: ______",source);
        const auto sourceBytes=ReadBytes(source);
        Document doc;doc.Open(source);
        AnnotationStyle red;red.color=0xd00000;red.fillColor=0xff0000;red.filled=true;red.lineWidth=0;
        doc.AddAnnotation(0,Tool::Rectangle,{100,300,200,100},L"",{},{},12,1,red);
        doc.AddAnnotation(0,Tool::Image,{110,310,180,80},L"",png);
        auto all=doc.Annotations(0);const auto sig=all.back();
        Require(sig.type==Tool::Image,"signature must be an image annotation");
        const float aspect=static_cast<float>(drawn.width)/drawn.height;
        Require(std::abs(sig.bounds.w/sig.bounds.h-aspect)<.05f&&sig.bounds.w<=180.5f&&sig.bounds.h<=80.5f,"signature keeps its aspect ratio inside the box");
        {
            const auto page=doc.Render(0,1);
            // 签名框左上角附近（空白处）应透出红色，而不是白底或黑底。
            const int x=static_cast<int>(sig.bounds.x+2),y=static_cast<int>(sig.bounds.y+2);
            const auto* p=&page.bgra[static_cast<size_t>(y)*page.stride+x*4];
            Require(p[2]>200&&p[1]<60&&p[0]<60,"signature background must be transparent over other content");
            size_t ink=0;
            for(int yy=static_cast<int>(sig.bounds.y);yy<static_cast<int>(sig.bounds.y+sig.bounds.h);++yy)for(int xx=static_cast<int>(sig.bounds.x);xx<static_cast<int>(sig.bounds.x+sig.bounds.w);++xx){
                const auto* q=&page.bgra[static_cast<size_t>(yy)*page.stride+xx*4];if(q[0]>q[2])++ink;   // 蓝色笔迹
            }
            Require(ink>50,"signature strokes must be visible on the page");
        }
        // 日期：普通文字批注。
        Annotation date;date.id=-1;date.type=Tool::Text;date.text=L"2026-09-28";date.bounds={330,300,90,18};date.fontSize=12;date.fixedTextBox=true;
        doc.AddTextAnnotation(0,date);
        Require(doc.Annotations(0).back().type==Tool::Text&&doc.Annotations(0).back().text==L"2026-09-28","date text annotation");
        // ✓ 一笔、✗ 两笔，各为一次可撤销操作。
        AnnotationStyle ink;ink.color=0x1f1f1f;ink.lineWidth=1.6f;
        const auto before=doc.Annotations(0).size();
        doc.AddInkStrokes(0,CheckMarkStrokes({330,340,14,14}),ink);
        doc.AddInkStrokes(0,CrossMarkStrokes({360,340,14,14}),ink);
        all=doc.Annotations(0);
        Require(all.size()==before+2,"check and cross added");
        Require(all[before].type==Tool::Ink&&all[before].strokes.size()==1&&all[before].points.size()==3,"check mark is one 3-point stroke");
        Require(all[before+1].type==Tool::Ink&&all[before+1].strokes.size()==2&&all[before+1].points.size()==4,"cross mark is two strokes");
        {const auto b=all[before].bounds;
         // MuPDF 会给手绘批注的外观框留出边距（14pt 标记约 26pt 框）：只要求中心准确、尺寸有界。
         Require(std::abs(b.x+b.w/2-337)<3&&std::abs(b.y+b.h/2-347)<4&&b.w<32&&b.h<32,"check mark stays in its box");}
        doc.Undo();Require(doc.Annotations(0).size()==before+1,"undo removes the cross only");
        doc.Redo();Require(doc.Annotations(0).size()==before+2,"redo restores the cross");
        Require(Throws([&]{doc.AddInkStrokes(0,{},ink);})&&Throws([&]{doc.AddInkStrokes(0,{{{1,1},{std::numeric_limits<float>::quiet_NaN(),2}}},ink);}),"invalid marks rejected");
        Require(doc.Annotations(0).size()==before+2,"rejected marks must not mutate the document");
        const auto output=root/L"signed.pdf";doc.Save(output);
        Require(ReadBytes(source)==sourceBytes,"placing signatures must not overwrite the source");
        Document reopened;reopened.Open(output);all=reopened.Annotations(0);
        Require(std::count_if(all.begin(),all.end(),[](const auto& a){return a.type==Tool::Image;})==1,"saved signature");
        Require(std::count_if(all.begin(),all.end(),[](const auto& a){return a.type==Tool::Ink;})==2,"saved marks");
        Require(std::any_of(all.begin(),all.end(),[](const auto& a){return a.type==Tool::Text&&a.text==L"2026-09-28";}),"saved date");
        std::cout<<"PASS signature fill: "<<assertions<<" assertions; drawn/typed/imported signatures, transparency, library, placement, date, check/cross, undo and save.\n";
    }catch(const std::exception& e){std::cerr<<"FAIL signature fill after "<<assertions<<" assertions: "<<e.what()<<'\n';return 1;}
    return 0;
}
