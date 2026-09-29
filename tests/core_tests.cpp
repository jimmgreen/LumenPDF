#include "core/conversion.h"
#include <cmath>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <windows.h>
#include <thread>
using namespace lpdf;
static void Require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
int wmain(int argc,wchar_t** argv){
    if(argc==2&&std::wstring_view(argv[1])==L"--wait-worker"){Sleep(120000);return 0;}
    if(argc==4&&std::wstring_view(argv[1])==L"--word-worker")return WordWorker(argv[2],argv[3]);
    try{
        const fs::path out=argc>1?fs::path(argv[1]):fs::path(L"test-output");fs::create_directories(out);
        if(argc<3||std::wstring_view(argv[2])!=L"--features"){
        Require(ParsePageRange(L"1-3,5",5)==std::vector<int>({0,1,2,4}),"range expansion");
        bool rejected=false;try{(void)ParsePageRange(L"2-1",5);}catch(...){rejected=true;}Require(rejected,"invalid range accepted");
        Require(QuoteArg(L"a\\\"b\\")==L"\"a\\\\\\\"b\\\\\"","Windows argument quoting");
        const std::wstring sample=L"LumenPDF test\n中文测试：栖间阅读空间。\n文字、图像与页面合并。\n";
        const auto encoded=Utf8(sample);std::vector<unsigned char> bytes(encoded.begin(),encoded.end());
        Require(Converter::DecodeText(bytes,TextEncoding::Auto)==sample,"UTF-8 decoding");
        Require(Converter::DecodeText({0xff,0xfe,0x2d,0x4e},TextEncoding::Auto)==L"中","UTF-16 decoding");
        Require(Converter::DecodeText({0xd6,0xd0,0xce,0xc4},TextEncoding::GB18030)==L"中文","GB18030 decoding");
        const auto textPdf=out/L"中文文本.pdf";
        Document::TextToPdf(sample,textPdf);
        Document doc;doc.Open(textPdf);
        Require(doc.Info().pages.size()==1,"text pagination");
        Require(doc.Text(0).find(L"中文测试")!=std::wstring::npos,"Chinese text is not searchable");
        {   // 中文双击：结果落在“栖间阅读空间”之内，不越过标点（系统 ICU 按词典分词，缺失时取整段连续汉字）。
            const auto found=doc.SearchPage(0,L"阅读");Require(!found.empty(),"Chinese word search");
            const auto snap=doc.SnapSelection(0,{found[0].bounds.x+found[0].bounds.w*.25f,found[0].bounds.y+found[0].bounds.h*.5f},SnapUnit::Word);
            const auto word=snap?doc.SelectionText(0,snap->first,snap->second):std::wstring{};
            Require(!word.empty()&&word.find(L"阅")!=std::wstring::npos&&std::wstring(L"栖间阅读空间").find(word)!=std::wstring::npos,"Chinese word snap");
        }
        auto raster=doc.Render(0,.6f);Require(raster.width>300&&!raster.bgra.empty(),"page render");
        doc.AddAnnotation(0,Tool::Text,{50,160,260,60},L"新增中文批注：你好世界");
        doc.AddAnnotation(0,Tool::Note,{330,100,24,24},L"请确认材料。");
        doc.AddAnnotation(0,Tool::Rectangle,{50,250,200,50});
        doc.AddAnnotation(0,Tool::Arrow,{50,330,100,40});
        doc.AddAnnotation(0,Tool::Ink,{50,410,80,30},L"",{},{{50,410},{75,435},{120,415}});
        const auto image=fs::path(__FILE__).parent_path()/L"fixtures/transparent.png";
        doc.AddAnnotation(0,Tool::Image,{350,240,90,90},L"",image);
        Require(doc.Annotations(0).size()==6,"annotation insertion");
        {
            // Display-list cache: tiles must match a direct full render, and every edit/undo/redo
            // must invalidate cached lists and page geometry.
            const auto full=doc.Render(0,.5f);
            const auto again=doc.Render(0,.5f);Require(full.bgra==again.bgra&&full.width==again.width,"cached render differs from first render");
            const auto size=doc.Info().pages[0];
            const auto tile=doc.Render(0,.5f,Rect{size.originX,size.originY,size.width*.5f,size.height*.5f});
            Require(tile.width>0&&tile.width<=full.width/2+2&&tile.height<=full.height/2+2,"cached tile size");
            // Tiles and full pages share one display list; only antialiasing at the tile edge may differ.
            uint64_t diff=0;int worst=0;
            for(int y=0;y<tile.height;++y)for(int x=0;x<tile.width*4;++x){
                const int d=std::abs(int(tile.bgra[static_cast<size_t>(y)*tile.stride+x])-int(full.bgra[static_cast<size_t>(y)*full.stride+x]));diff+=static_cast<uint64_t>(d);worst=std::max(worst,d);}
            const double mean=double(diff)/(double(tile.width)*4*tile.height);
            std::cout<<"tile vs full: "<<tile.width<<"x"<<tile.height<<" of "<<full.width<<"x"<<full.height<<" mean="<<mean<<" max="<<worst<<"\n";
            Require(mean<.05,"cached tile differs from full page render");
            {
                // Informational timing: first tile after an edit records the display list, later tiles replay it.
                auto time=[&](auto&& fn){LARGE_INTEGER f,a,b;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&a);fn();QueryPerformanceCounter(&b);return double(b.QuadPart-a.QuadPart)*1000.0/double(f.QuadPart);};
                doc.AddAnnotation(0,Tool::Rectangle,{10,10,5,5});
                const double cold=time([&]{(void)doc.Render(0,2.f,Rect{size.originX,size.originY,size.width*.5f,size.height*.5f});});
                const double warm=time([&]{for(int i=0;i<3;++i)(void)doc.Render(0,2.f,Rect{size.originX+size.width*.5f*(i&1),size.originY+size.height*.5f*(i>>1?1:0)+(i==2?0:0),size.width*.5f,size.height*.5f});})/3;
                std::cout<<"tile render: first "<<cold<<" ms, cached "<<warm<<" ms\n";doc.Undo();
            }
            const int hide=doc.Annotations(0)[2].id;
            Require(doc.Render(0,.5f,{},hide).bgra!=full.bgra,"hidden-annotation render reused the full cache entry");
            Require(doc.Render(0,.5f).bgra==full.bgra,"hidden variant polluted the full cache entry");
            doc.AddAnnotation(0,Tool::Rectangle,{300,500,120,80});
            const auto edited=doc.Render(0,.5f);Require(edited.bgra!=full.bgra,"edit did not invalidate the render cache");
            doc.Undo();Require(doc.Render(0,.5f).bgra==full.bgra,"undo did not invalidate the render cache");
            doc.Redo();Require(doc.Render(0,.5f).bgra==edited.bgra,"redo did not invalidate the render cache");
            doc.Undo();
            doc.RotatePage(0);const auto rotated=doc.Info().pages[0];
            Require(std::abs(rotated.width-size.height)<.5f&&std::abs(rotated.height-size.width)<.5f,"rotation did not invalidate cached page geometry");
            Require(std::abs(doc.Render(0,.5f).width-full.height)<=1,"rotation did not invalidate cached display list");
            doc.Undo();Require(std::abs(doc.Info().pages[0].width-size.width)<.5f,"undo did not restore cached page geometry");
            Require(doc.Annotations(0).size()==6,"render cache test changed annotations");
        }
        doc.Undo();Require(doc.Annotations(0).size()==5,"undo");
        doc.Redo();Require(doc.Annotations(0).size()==6,"redo");
        {   // 拖动预览：单个批注外观为透明背景位图，覆盖范围包住批注框，且不改动文档。
            const auto textAnnotation=doc.Annotations(0).front();
            const auto sprite=doc.RenderAnnotation(0,textAnnotation.id,1.0f);
            Require(sprite.pixels.width>0&&sprite.pixels.height>0,"annotation sprite size");
            Require(sprite.bounds.x<=textAnnotation.bounds.x+.5f&&sprite.bounds.y<=textAnnotation.bounds.y+.5f,"annotation sprite origin");
            Require(sprite.bounds.x+sprite.bounds.w>=textAnnotation.bounds.x+textAnnotation.bounds.w-.5f,"annotation sprite extent");
            size_t opaque=0,clear=0;
            for(int y=0;y<sprite.pixels.height;++y)for(int x=0;x<sprite.pixels.width;++x){
                const unsigned char a=sprite.pixels.bgra[static_cast<size_t>(y)*sprite.pixels.stride+static_cast<size_t>(x)*4+3];
                if(a==0)++clear;else ++opaque;
            }
            Require(opaque>0&&clear>0,"annotation sprite must be transparent outside glyphs");
            const bool canUndoBefore=doc.Info().canUndo;doc.Undo();doc.Redo();
            Require(canUndoBefore&&doc.Annotations(0).size()==6,"sprite rendering must not add undo steps or annotations");
        }
        auto stamp=doc.Annotations(0).back();const auto unrotated=doc.Render(0,.5f);stamp.rotation=90;doc.UpdateAnnotation(0,stamp);
        Require(doc.Annotations(0).back().rotation==90,"image rotation metadata");Require(doc.Render(0,.5f).bgra!=unrotated.bgra,"image rotation must change visible appearance");
        doc.Undo();Require(doc.Render(0,.5f).bgra==unrotated.bgra,"image rotation undo");
        const auto annotated=out/L"中文批注.pdf";doc.Save(annotated);
        Document reopen;reopen.Open(annotated);Require(reopen.Annotations(0).size()==6,"annotations lost after save");
        Require(reopen.Text(0).find(L"你好世界")!=std::wstring::npos,"Chinese annotation not searchable");
        const auto rendered=reopen.Render(0,.5f);Require(rendered.width>200,"annotated render");
        auto annotation=reopen.Annotations(0).front();annotation.text=L"修改后的中文";annotation.bounds.x+=10;
        reopen.UpdateAnnotation(0,annotation);reopen.Save(out/L"修改.pdf");
        reopen.InsertBlank(0);Require(reopen.Info().pages.size()==2,"blank insertion");
        reopen.RotatePage(0);Require(reopen.Info().pages[0].width>800,"page rotation");
        reopen.Reorder({1,0});reopen.Extract({1},out/L"提取.pdf");reopen.DeletePages({0});
        Require(reopen.Info().pages.size()==1,"page deletion");
        const auto imagePdf=out/L"图片.pdf";Document::ImageToPdf(image,imagePdf,true);
        const auto merged=out/L"合并.pdf";
        Document::Merge({{annotated,L"1",L""},{textPdf,L"",L""},{imagePdf,L"",L""}},merged,true,{});
        Document merge;merge.Open(merged);Require(merge.Info().pages.size()==3,"merge count");
        Require(merge.Info().outline.size()==3,"merge bookmarks");
        Require(merge.Annotations(0).empty(),"merge should bake annotations");
        Require(merge.Text(0).find(L"你好世界")!=std::wstring::npos,"baked Chinese annotation lost");
        const auto baked=merge.Render(0,.5f); Require(baked.width==rendered.width&&baked.height==rendered.height,"merge changed page size"); uint64_t error=0; for(size_t i=0;i<baked.bgra.size();++i)error+=static_cast<uint64_t>(std::abs(int(baked.bgra[i])-int(rendered.bgra[i]))); Require(double(error)/baked.bgra.size()<0.20,"merge changed appearance beyond subpixel antialiasing tolerance");
        const auto sentinel=ReadBytes(annotated);
        auto cancelled=std::make_shared<std::atomic_bool>(true);bool stopped=false;
        try{Document::Merge({{textPdf,L"",L""}},annotated,true,cancelled);}catch(const Cancelled&){stopped=true;}
        Require(stopped&&ReadBytes(annotated)==sentinel,"cancelled merge changed source");
        bool failed=false;try{doc.Save(out/L"missing-directory"/L"file.pdf");}catch(...){failed=true;}
        Require(failed&&ReadBytes(annotated)==sentinel,"failed save changed source");
        const auto fixtures=fs::path(__FILE__).parent_path()/L"fixtures";
        Document locked;bool passwordNeeded=false;
        try{locked.Open(fixtures/L"password.pdf");}catch(const PasswordRequired&){passwordNeeded=true;}
        Require(passwordNeeded,"encrypted PDF did not require password");locked.Open(fixtures/L"password.pdf",L"reader");
        locked.AddAnnotation(0,Tool::Text,{40,60,200,40},L"密码文档中文");
        locked.Save(out/L"密码保存.pdf");Document lockedAgain;lockedAgain.Open(out/L"密码保存.pdf",L"reader");
        Require(lockedAgain.Text(0).find(L"密码文档中文")!=std::wstring::npos,"encrypted document save");
        locked.Extract({0},out/L"密码提取.pdf");
        Document forms;forms.Open(fixtures/L"forms-mixed.pdf");
        const auto beforeForm=forms.Render(0,.75f);
        Require(forms.Info().pages[1].width==360&&forms.Info().pages[1].height==630,"rotated crop box");
        Document::Merge({{fixtures/L"forms-mixed.pdf",L"",L""}},out/L"表单合并.pdf",true,{});
        Document bakedForms;bakedForms.Open(out/L"表单合并.pdf");
        Require(bakedForms.Text(0).find(L"FORM VALUE 42")!=std::wstring::npos,"form appearance lost in merge");
        Require(bakedForms.Render(0,.75f).bgra==beforeForm.bgra,"form merge appearance changed"); Document::Merge({{fixtures/L"no-view.pdf",L"",L""}},out/L"不可见表单.pdf",false,{});Document hidden;hidden.Open(out/L"不可见表单.pdf");Require(hidden.Text(0).find(L"FORM VALUE 42")==std::wstring::npos,"merge exposed NoView field");
        Require(!doc.Search(L"你好世界").empty(),"search must include Chinese annotations");
        Require(doc.Info().canUndo,"save destroyed undo history");doc.Undo();doc.Redo();
        bool timeout=false;try{RunProcess(ExecutablePath(),{L"--wait-worker"},{},1);}catch(const std::exception&){timeout=true;}
        Require(timeout,"converter timeout did not terminate owned job");
        auto stop=std::make_shared<std::atomic_bool>(false);
        std::jthread cancelThread([stop]{Sleep(200);stop->store(true);});
        bool cancelledProcess=false;try{RunProcess(ExecutablePath(),{L"--wait-worker"},stop,10);}catch(const Cancelled&){cancelledProcess=true;}
        Require(cancelledProcess,"active converter cancellation failed");
        doc.MarkDirty();doc.Snapshot(out/L"恢复快照.pdf");Require(doc.Info().dirty&&doc.Info().canUndo,"backup changed edit state");
        Document recovered;recovered.Open(out/L"恢复快照.pdf");Require(recovered.Text(0).find(L"你好世界")!=std::wstring::npos,"backup lost annotations");
        Document highlight;highlight.Open(textPdf);highlight.AddAnnotation(0,Tool::Highlight,{40,40,400,65});
        highlight.Save(out/L"高亮.pdf");Document highlighted;highlighted.Open(out/L"高亮.pdf");Require(highlighted.Annotations(0).size()==1,"highlight persistence");
        Document longDocument;
        Document::Merge(std::vector<MergeInput>(100,{textPdf,L"",L""}),out/L"长文档100页.pdf",false,{});longDocument.Open(out/L"长文档100页.pdf");
        Require(longDocument.Info().pages.size()==100,"long document fixture");
        Document fitted;fitted.New();
        fitted.AddAnnotation(0,Tool::Text,{40,40,400,60},L"你好 PDF");
        auto fit=fitted.Annotations(0).front();
        Require(fit.bounds.w<130&&fit.bounds.h<30,"new text box does not fit text");
        fit.text=L"第一行 Hello\n第二行 World";fit.fontSize=24;fitted.UpdateAnnotation(0,fit);
        fit=fitted.Annotations(0).front();
        Require(fit.bounds.h>40&&fit.bounds.h<85,"multiline font resize does not fit");
        Require(fitted.Text(0).find(L"World")!=std::wstring::npos,"fitted text clipped");
        fit.text=L"短";fit.fontSize=12;fitted.UpdateAnnotation(0,fit);
        fit=fitted.Annotations(0).front();
        Require(fit.bounds.w<35&&fit.bounds.h<30,"shorter edit did not shrink");
        fitted.Save(out/L"贴合文字.pdf");Document fitReopen;fitReopen.Open(out/L"贴合文字.pdf");
        Require(std::abs(fitReopen.Annotations(0).front().bounds.w-fit.bounds.w)<.1f,"fit changed after save");
        for(Tool tool:{Tool::Arrow,Tool::Ink,Tool::Highlight}){
            fitted.AddAnnotation(0,tool,{70,180,100,40},L"",{},{{170,220},{70,180}});
            auto shape=fitted.Annotations(0).back();const float oldX=shape.bounds.x;
            shape.bounds.x+=35;shape.bounds.y+=20;shape.opacity=.5f;fitted.UpdateAnnotation(0,shape);
            auto moved=fitted.Annotations(0).back();
            Require(std::abs(moved.bounds.x-oldX-35)<2,"tool geometry did not move");
            Require(std::abs(moved.opacity-.5f)<.01f,"tool opacity not updated");
            fitted.Undo();Require(std::abs(fitted.Annotations(0).back().bounds.x-oldX)<.1f,"tool move undo");
        }
        fitted.Save(out/L"工具编辑.pdf");
        Document editorDoc;editorDoc.New();
        const auto emptyPage=editorDoc.Render(0,1);
        editorDoc.AddAnnotation(0,Tool::Text,{40,40,160,50},L"编辑测试 Editor",{},{},18,.65f);
        auto edited=editorDoc.Annotations(0).front();
        Require(std::abs(edited.fontSize-18)<.1f&&std::abs(edited.opacity-.65f)<.01f,"new editor font and opacity");
        Require(editorDoc.Render(0,1,{},edited.id).bgra==emptyPage.bgra,"draft background still shows original annotation");
        Require(editorDoc.Annotations(0).size()==1&&editorDoc.Info().canUndo,"draft rendering mutated document");
        edited.text=L"第一行中文文字自动换行测试\nSecond line";edited.bounds.w=90;edited.fixedTextWidth=true;
        editorDoc.UpdateAnnotation(0,edited);
        Require(editorDoc.Annotations(0).front().bounds.h>40,"explicit editor width did not wrap");
        editorDoc.Undo();Require(editorDoc.Annotations(0).front().text==L"编辑测试 Editor","editor commit is not one undo operation");
        editorDoc.Redo();Require(editorDoc.Annotations(0).front().text==edited.text,"editor redo lost content");

        if(argc>2&&std::wstring_view(argv[2])==L"--office"){
            Converter converter;const auto source=fs::path(__FILE__).parent_path()/L"fixtures/word-sample.docx";
            auto converted=converter.Convert(source,{},{});Document word;word.Open(converted.pdf);
            Require(word.Text(0).find(L"Word conversion")!=std::wstring::npos,"Word conversion text");
            Require(word.Text(0).find(L"中文转换测试")!=std::wstring::npos,"Word Chinese conversion text");word.Save(out/L"Word转换.pdf");
        }
        {   // 链接与阅读顺序文字选择。
            Document linked;linked.Open(fs::path(__FILE__).parent_path()/L"fixtures/links.pdf");
            const auto links=linked.Links(0);
            Require(links.size()==3,"page links not loaded");
            const Link* internal=nullptr;const Link* web=nullptr;const Link* file=nullptr;
            for(const auto& l:links){if(!l.External())internal=&l;else if(l.uri.find(L"https://")==0)web=&l;else file=&l;}
            Require(internal&&internal->page==2,"internal link must resolve to page 3");
            // PDF 纵坐标 420（自下而上）在 792 高的页面上对应 MuPDF 页面坐标 372。
            Require(std::isfinite(internal->targetY)&&std::abs(internal->targetY-372)<2,"internal link target position");
            Require(web&&web->uri==L"https://example.com/lumen","external link uri");
            Require(file!=nullptr,"non-web links are still reported so the UI can refuse them");
            Require(internal->bounds.y>100&&internal->bounds.y<140&&internal->bounds.w>150,"link bounds must use top-left page coordinates");
            Require(linked.Links(1).empty(),"page without links");
            const auto outline=linked.Info().outline;
            Require(outline.size()==3&&outline[1].depth==1&&outline[1].page==1&&outline[2].depth==0&&outline[2].page==2,"nested outline");
            // 导出：加密副本需要口令、去除加密后无需口令；PNG 与文本导出；失败时不覆盖已有目标。
            Require(!linked.Info().encrypted,"plain fixture reported as encrypted");
            const auto securedPath=out/L"加密副本.pdf";
            Security sec;sec.userPassword=L"开门123";sec.allowCopy=false;
            linked.ExportSecured(securedPath,sec);
            bool needs=false;try{Document probe;probe.Open(securedPath);}catch(const PasswordRequired&){needs=true;}
            Require(needs,"encrypted copy must require the open password");
            Document reopened;reopened.Open(securedPath,L"开门123");
            Require(reopened.Info().encrypted&&reopened.Info().pages.size()==3,"encrypted copy reopen");
            Require(!linked.Info().dirty,"export must not change the dirty flag");
            const auto unlocked=out/L"去除加密.pdf";
            reopened.ExportSecured(unlocked,{});
            Document plainCopy;plainCopy.Open(unlocked);
            Require(!plainCopy.Info().encrypted&&plainCopy.Info().pages.size()==3,"decrypted copy opens without password");
            Security ownerOnly;ownerOnly.allowPrint=false;
            const auto restricted=out/L"限制打印.pdf";linked.ExportSecured(restricted,ownerOnly);
            Document r;r.Open(restricted);Require(r.Info().encrypted,"permission-only copy opens without password but stays encrypted");
            const auto images=linked.ExportImages({0,2},out/L"png",L"页",96);
            Require(images.size()==2&&images[1].filename()==L"页-3.png"&&fs::file_size(images[0])>1000,"png export");
            const auto txt=out/L"全文.txt";linked.ExportText(txt);
            const auto textBytes=ReadBytes(txt);const std::string body(textBytes.begin(),textBytes.end());
            Require(body.rfind("\xEF\xBB\xBF",0)==0&&body.find("Target on page three")!=std::string::npos&&body.find('\f')!=std::string::npos,"text export");
            WriteBytes(securedPath,{'k','e','e','p'});
            bool refused=false;try{Security bad;bad.userPassword=std::wstring(200,L'x');linked.ExportSecured(securedPath,bad);}catch(const std::exception&){refused=true;}
            Require(refused&&fs::file_size(securedPath)==4,"failed export must keep the existing destination");
            // 关闭：释放文档，之后的访问报错；可以重新打开。
            Document closing;closing.Open(fs::path(__FILE__).parent_path()/L"fixtures/links.pdf");Require(closing.IsOpen(),"open state");
            closing.Close();Require(!closing.IsOpen(),"closed state");
            bool closedThrows=false;try{(void)closing.Info();}catch(const std::exception&){closedThrows=true;}
            Require(closedThrows,"closed document must reject access");
            closing.Open(fs::path(__FILE__).parent_path()/L"fixtures/links.pdf");Require(closing.Info().pages.size()==3,"reopen after close");
            const auto text=linked.SelectionText(0,{72,58},{400,95});
            Require(text.find(L"Links fixture first line")!=std::wstring::npos&&text.find(L"Second line")!=std::wstring::npos,"selection text in reading order");
            Require(linked.SelectionText(0,{500,780},{560,790}).empty(),"selection in empty margin must be empty");
            Require(!linked.HighlightQuads(0,{72,58},{400,95}).empty(),"selection quads");
            // 双击选词 / 三击选段：吸附到命中的单词或整个文本块，空白处不吸附。
            const auto hits=linked.SearchPage(0,L"fixture");Require(!hits.empty(),"fixture word search");
            const Point inWord{hits[0].bounds.x+hits[0].bounds.w*.5f,hits[0].bounds.y+hits[0].bounds.h*.5f};
            const auto word=linked.SnapSelection(0,inWord,SnapUnit::Word);
            Require(word&&linked.SelectionText(0,word->first,word->second)==L"fixture","double-click word snap");
            const auto block=linked.SnapSelection(0,inWord,SnapUnit::Paragraph);
            Require(block&&linked.SelectionText(0,block->first,block->second).find(L"Links fixture first line")!=std::wstring::npos,"triple-click paragraph snap");
            Require(!linked.SnapSelection(0,{530,785},SnapUnit::Word),"word snap in empty margin");
            // 跨页选择：中间页整页、末页从页首取。
            Require(linked.SelectionText(2,{},{},true,true).find(L"Target on page three")!=std::wstring::npos,"whole-page selection text");
            Require(!linked.HighlightQuads(2,{},{},true,true).empty(),"whole-page selection quads");
            Point from{},to{hits[0].bounds.x+1,hits[0].bounds.y+hits[0].bounds.h*.5f};
            Require(linked.ResolveSelection(0,from,to,true,false)&&linked.SelectionText(0,from,to).find(L"Links")!=std::wstring::npos,"page-start endpoint");
        }
        {   // 新批注类型：下划线、删除线、椭圆、直线、印章往返并可保存重开。
            Document d;d.Open(fs::path(__FILE__).parent_path()/L"fixtures/links.pdf");
            d.AddAnnotation(0,Tool::Underline,{72,58,328,37},{},{},{{72,58},{400,95}},12,1,{},true);
            d.AddAnnotation(0,Tool::StrikeOut,{72,58,330,20});
            d.AddAnnotation(0,Tool::Ellipse,{100,300,120,80});
            d.AddAnnotation(0,Tool::Line,{100,420,150,40});
            d.AddAnnotation(0,Tool::Stamp,{300,500,2,2},L"已批准");
            auto has=[](const std::vector<Annotation>& list,Tool t){return std::any_of(list.begin(),list.end(),[t](const Annotation& a){return a.type==t;});};
            const auto saved=out/L"新批注类型.pdf";d.Save(saved);
            Document back;back.Open(saved);const auto list=back.Annotations(0);
            for(Tool t:{Tool::Underline,Tool::StrikeOut,Tool::Ellipse,Tool::Line,Tool::Stamp})Require(has(list,t),"new annotation type round-trip");
            Require(!has(list,Tool::Arrow)&&!has(list,Tool::Image),"line/stamp must not be detected as arrow/image");
            auto st=*std::find_if(list.begin(),list.end(),[](const Annotation& a){return a.type==Tool::Stamp;});
            Require(st.text==L"已批准"&&st.bounds.w>40&&std::abs(st.bounds.h-42)<1.5f,"stamp text and computed size");
            const float cx=st.bounds.x+st.bounds.w/2;
            st.text=L"已审核通过 APPROVED";back.UpdateAnnotation(0,st);
            const auto after=back.Annotations(0);
            const auto s2=*std::find_if(after.begin(),after.end(),[](const Annotation& a){return a.type==Tool::Stamp;});
            Require(s2.text==L"已审核通过 APPROVED"&&s2.bounds.w>st.bounds.w&&std::abs(s2.bounds.x+s2.bounds.w/2-cx)<1.5f,"stamp text change keeps center");
            // 压缩：结果是有效 PDF，页数不变，当前文档不变脏。
            back.Undo();
            const auto small=out/L"压缩.pdf";const auto sizes=back.Compress(small,CompressLevel::Balanced);
            Require(sizes.after==fs::file_size(small)&&sizes.before>0,"compress sizes");
            Document c;c.Open(small);Require(c.Info().pages.size()==3&&!c.Annotations(0).empty(),"compressed copy keeps pages and annotations");
            // 拆分：每 2 页 / 顶层书签 / 大小。
            const auto folder=out/L"拆分";fs::remove_all(folder);fs::create_directories(folder);
            const auto every=d.Split(SplitMode::EveryPages,2,folder,L"分");
            Require(every.size()==2,"split every 2 pages");
            {Document a;a.Open(every[0]);Document b;b.Open(every[1]);Require(a.Info().pages.size()==2&&b.Info().pages.size()==1,"split page counts");}
            const auto chapters=d.Split(SplitMode::TopBookmarks,0,folder,L"章");
            Require(chapters.size()==2,"split by top-level bookmarks");
            {Document a;a.Open(chapters[0]);Require(a.Info().pages.size()==2,"first chapter spans two pages");}
            bool fits=false;try{(void)d.Split(SplitMode::MaxSize,1,folder,L"大小");}catch(const std::exception&){fits=true;}
            Require(fits,"a document already under the size limit is not split");
            bool badSplit=false;try{(void)d.Split(SplitMode::EveryPages,0,folder,L"x");}catch(const std::exception&){badSplit=true;}
            Require(badSplit,"invalid split amount rejected");
            // 页眉页脚、页码、水印：写入页面文字，可撤销。
            Document deco;deco.Open(fs::path(__FILE__).parent_path()/L"fixtures/links.pdf");
            PageDecoration pd;pd.header=L"内部资料";pd.footer=L"第 {page} 页，共 {total} 页";pd.watermark=L"机密";pd.startNumber=5;
            deco.Decorate(pd);
            Require(deco.Info().dirty&&deco.Info().canUndo,"decorate is an undoable edit");
            const auto t3=deco.Text(2);
            Require(t3.find(L"第 7 页，共 7 页")!=std::wstring::npos&&t3.find(L"内部资料")!=std::wstring::npos&&t3.find(L"机密")!=std::wstring::npos,"decoration text on page");
            Require(deco.Text(0).find(L"Links fixture first line")!=std::wstring::npos,"original content kept");
            const auto decorated=out/L"页眉页脚.pdf";deco.Save(decorated);
            {Document r;r.Open(decorated);Require(r.Text(1).find(L"第 6 页")!=std::wstring::npos,"decoration persisted");}
            deco.Undo();Require(deco.Text(2).find(L"内部资料")==std::wstring::npos,"decoration undo");
            PageDecoration some;some.footer=L"QQ{page}QQ";some.pages={1};deco.Decorate(some);
            Require(deco.Text(0).find(L"QQ")==std::wstring::npos&&deco.Text(1).find(L"QQ2QQ")!=std::wstring::npos,"page subset decoration");
            // 书签编辑：整体替换、层级与位置往返，可撤销。
            Document bm;bm.Open(fs::path(__FILE__).parent_path()/L"fixtures/links.pdf");
            std::vector<OutlineItem> items{{L"封面",0,0},{L"第一章 概述",1,0,200},{L"1.1 背景",1,1},{L"附录",2,0}};
            bm.SetOutline(items);
            auto o=bm.Info().outline;
            Require(o.size()==4&&o[1].title==L"第一章 概述"&&o[2].depth==1&&o[3].page==2&&std::isfinite(o[1].y)&&std::abs(o[1].y-200)<2,"outline edit");
            const auto outlined=out/L"书签编辑.pdf";bm.Save(outlined);
            {Document r;r.Open(outlined);const auto ro=r.Info().outline;Require(ro.size()==4&&ro[2].title==L"1.1 背景"&&ro[2].depth==1,"outline persisted");}
            bm.Undo();Require(bm.Info().outline.size()==3,"outline undo");
            bm.SetOutline({});Require(bm.Info().outline.empty(),"remove all bookmarks");
        }
        } // baseline tests
        {
            Document d;d.Open(fs::path(__FILE__).parent_path()/L"fixtures/links.pdf");
            const auto initial=d.Info();
            std::cout<<"feature: rotation"<<std::endl;
            d.RotatePages({0,1},-90);Require(std::abs(d.Info().pages[0].width-initial.pages[0].height)<1,"rotate left swaps geometry");
            d.Undo();Require(std::abs(d.Info().pages[1].width-initial.pages[1].width)<1,"batch rotate single undo");
            d.RotatePage(0,180);Require(std::abs(d.Info().pages[0].width-initial.pages[0].width)<1,"rotate 180 geometry");d.Undo();
            std::cout<<"feature: crop"<<std::endl;
            d.CropPages({0},10,20,30,40);Require(std::abs(d.Info().pages[0].width-(initial.pages[0].width-40))<1,"crop width");
            Require(std::abs(d.Info().pages[0].height-(initial.pages[0].height-60))<1,"crop height");d.Undo();
            bool bad=false;try{d.CropPages({0,1},100000,0,0,0);}catch(...){bad=true;}Require(bad&&std::abs(d.Info().pages[0].width-initial.pages[0].width)<1,"invalid crop atomic");
            std::cout<<"feature: metadata"<<std::endl;
            auto metadata=d.Metadata();metadata.title=L"测试标题";metadata.author=L"Reviewer";d.SetMetadata(metadata);Require(d.Metadata().title==metadata.title,"metadata write");d.Undo();Require(d.Metadata().title!=metadata.title,"metadata undo");
            std::cout<<"feature: labels"<<std::endl;
            d.SetPageLabels(0,'r',L"序-",1);Require(d.Info().labels[0]==L"序-i"&&d.Info().labels[1]==L"序-ii","page labels");d.Undo();
            std::cout<<"feature: duplicate"<<std::endl;
            const auto original=d.Text(0);d.DuplicatePages({0});Require(d.Info().pages.size()==initial.pages.size()+1&&d.Text(1)==original,"duplicate content");
            d.RotatePage(1);Require(std::abs(d.Info().pages[0].width-initial.pages[0].width)<1,"duplicate independent rotation");d.Undo();d.Undo();
            std::cout<<"feature: replace"<<std::endl;
            const auto replacement=out/L"replacement.pdf";Document::TextToPdf(L"Replacement sentinel",replacement);
            d.ReplacePages({0},replacement);Require(d.Text(0).find(L"Replacement sentinel")!=std::wstring::npos&&d.Info().pages.size()==initial.pages.size(),"replace pages");d.Undo();Require(d.Text(0)==original,"replace undo");
            {
                const auto file=out/L"dense-search.pdf";std::wstring text;
                for(int i=0;i<600;++i)text+=L"Needle Needle Needle Needle Needle Needle Needle Needle Needle Needle\n";
                Document::TextToPdf(text,file);Document many;many.Open(file);Require(many.Search(L"Needle").size()==6000,"search no 256-per-page or 5000-document truncation");
            }
            auto hits=d.SearchPage(0,L"links");Require(!hits.empty()&&!hits[0].context.empty(),"search context and ignore case");
            SearchOptions opt;opt.matchCase=true;Require(d.SearchPage(0,L"links",opt).empty(),"case-sensitive search");
            opt={};opt.wholeWord=true;Require(d.SearchPage(0,L"Link",opt).empty()&&!d.SearchPage(0,L"Links",opt).empty(),"whole-word boundaries");
            d.AddAnnotation(0,Tool::Note,{80,80,24,24},L"UniqueReviewNeedle");
            Require(d.SearchPage(0,L"UniqueReviewNeedle").empty(),"body excludes comment text");opt={};opt.annotations=true;
            Require(d.SearchPage(0,L"UniqueReviewNeedle",opt).size()==1,"comment search");
            d.SetOutline({{L"UniqueBookmarkNeedle",0,0}});opt={};opt.bookmarks=true;Require(d.SearchPage(0,L"UniqueBookmarkNeedle",opt).size()==1,"bookmark search");
            auto visible=d.Render(0,.5f),withoutComments=d.Render(0,.5f,{},-1,false,true);Require(visible.bgra!=withoutComments.bgra,"annotation rendering switch");Require(d.Render(0,.5f).bgra==visible.bgra,"render cache separates annotation visibility");
            auto cancel=std::make_shared<std::atomic_bool>(true);bool searchCancelled=false;try{d.SearchPage(0,L"Links",{},cancel);}catch(const Cancelled&){searchCancelled=true;}Require(searchCancelled,"search cancellation");
        }
        std::cout<<"PASS: Unicode, ranges, rendering, editable annotations, undo/redo, pages, lossless visible merge, bookmarks, safe save and cancellation.\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}



