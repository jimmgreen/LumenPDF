#include "core/document.h"
#include "core/annotation_geometry.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <limits>
using namespace lpdf;
namespace {
int assertions=0;
void Require(bool okay,const char* why){++assertions;if(!okay)throw std::runtime_error(why);}
bool Near(float a,float b,float tolerance=.03f){return std::abs(a-b)<tolerance;}
void BitmapFile(const fs::path& path,const Bitmap& image){
    BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);file.bfSize=file.bfOffBits+image.stride*image.height;
    BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=image.width;info.biHeight=-image.height;info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
    std::ofstream output(path,std::ios::binary);output.write(reinterpret_cast<const char*>(&file),sizeof(file));output.write(reinterpret_cast<const char*>(&info),sizeof(info));output.write(reinterpret_cast<const char*>(image.bgra.data()),image.bgra.size());
}
}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc==3&&std::wstring_view(argv[1])==L"--verify-features"){
            // tests/feature_smoke.ps1 保存的结果：椭圆、直线、印章、书签与页脚。
            Document saved;saved.Open(argv[2]);const auto all=saved.Annotations(0);
            for(Tool tool:{Tool::Ellipse,Tool::Line,Tool::Stamp})
                Require(std::any_of(all.begin(),all.end(),[tool](const auto& a){return a.type==tool;}),"a new UI annotation tool is missing");
            Require(std::any_of(all.begin(),all.end(),[](const auto& a){return a.type==Tool::Stamp&&a.text==L"机密";}),"UI stamp text");
            const auto outline=saved.Info().outline;
            Require(std::any_of(outline.begin(),outline.end(),[](const auto& o){return o.title==L"冒烟书签";}),"UI bookmark was not saved");
            Require(saved.Text(0).find(L"冒烟页脚 1")!=std::wstring::npos,"UI footer was not written");
            std::cout<<"PASS: UI ellipse, line, stamp, bookmark and footer saved.\n";return 0;
        }
        if(argc==3&&std::wstring_view(argv[1])==L"--verify-ui"){
            Document saved;saved.Open(argv[2]);const auto all=saved.Annotations(0);
            Require(all.size()==6,"UI cancelled operations added annotations or a tool failed to save");
            for(Tool tool:{Tool::Note,Tool::Highlight,Tool::Rectangle,Tool::Arrow,Tool::Ink,Tool::Image}){
                auto it=std::find_if(all.begin(),all.end(),[tool](const auto& a){return a.type==tool;});
                Require(it!=all.end(),"a UI annotation tool is missing");
                if(tool==Tool::Note)Require(it->text.find(L"第二行")!=std::wstring::npos,"UI multiline note did not commit");
                if(tool==Tool::Highlight)Require(it->quads.size()>=2&&!it->areaHighlight,"UI text highlight is not multi-line");
                if(tool==Tool::Rectangle)Require(it->style.filled&&Near(it->style.lineWidth,2.25f)&&Near(it->opacity,.82f),"UI property editing did not commit");
                if(tool==Tool::Arrow)Require(it->points.size()==2&&Near(it->points[1].x,286,3)&&Near(it->points[1].y,312,3),"UI arrow endpoint did not move");
                if(tool==Tool::Ink)Require(it->points.size()>20,"UI hand-drawn path was not smoothed");
                if(tool==Tool::Image)Require(it->rotation==90,"UI image rotation was not saved");
            }
            BitmapFile(fs::path(argv[2]).parent_path()/L"annotation-ui-saved.bmp",saved.Render(0,1.5f));
            std::cout<<"PASS saved native UI annotations: "<<assertions<<" assertions\n";return 0;
        }
        fs::path out=argc>1?argv[1]:L"annotation-output";fs::create_directories(out);
        auto textPdf=out/L"annotation-source.pdf";
        Document::TextToPdf(L"Precision annotation review\nThe second line stays readable beneath a highlight.\nSix existing tools, fully editable PDF annotations.",textPdf);
        const auto sourceBytes=ReadBytes(textPdf);
        Document doc;doc.Open(textPdf);
        auto first=doc.Search(L"Precision").front().bounds,second=doc.Search(L"second").front().bounds;
        const Point start{first.x,first.y+first.h/2},end{second.x+second.w,second.y+second.h/2};
        auto quads=doc.HighlightQuads(0,start,end);
        Require(quads.size()>=2,"multi-line highlight must contain separate text quads");
        Require(doc.HighlightQuads(0,end,start).size()==quads.size(),"reverse text selection differs");
        Require(doc.HighlightQuads(0,{450,600},{510,650}).empty(),"empty-margin drag snapped to unrelated text");
        Require(!doc.Info().dirty&&!doc.Info().canUndo,"highlight preview changed the document");
        auto highlight=DefaultAnnotationStyle(Tool::Highlight);
        doc.AddAnnotation(0,Tool::Highlight,{40,40,450,80},L"",{},{start,end},12,.45f,highlight,true);
        Require(!doc.Annotations(0).back().areaHighlight,"text highlight marked as area");
        Require(doc.Annotations(0).back().quads.size()==quads.size(),"saved highlight differs from its preview geometry");
        auto rectangle=DefaultAnnotationStyle(Tool::Rectangle);rectangle.lineWidth=2;rectangle.filled=true;rectangle.fillColor=0xffecec;
        doc.AddAnnotation(0,Tool::Rectangle,{65,155,195,82},L"",{},{},12,.9f,rectangle);
        auto box=doc.Annotations(0).back();
        Require(box.style.filled&&box.style.color==rectangle.color&&Near(box.style.lineWidth,2),"rectangle style did not round-trip");
        Require(HitAnnotation(box,{120,190},3),"filled rectangle is not selectable in its interior");
        auto hollow=box;hollow.style.filled=false;
        Require(!HitAnnotation(hollow,{120,190},3)&&HitAnnotation(hollow,{66,190},3),"empty rectangle must hit its outline, not empty space");
        auto arrow=DefaultAnnotationStyle(Tool::Arrow);arrow.color=0x2864dc;arrow.lineWidth=2;arrow.endEnding=5;
        doc.AddAnnotation(0,Tool::Arrow,{65,290,195,80},L"",{},{{65,370},{260,290}},12,1,arrow);
        auto line=doc.Annotations(0).back();
        Require(line.points.size()==2&&Near(line.points[1].x,260)&&line.style.endEnding==5,"arrow direction or head lost");
        Require(!HitAnnotation(line,{75,295},3)&&HitAnnotation(line,{160,331},5),"arrow hit test selects empty bounding-box regions");
        line.geometryEdited=true;line.points[1]={270,335};line.bounds=PointsRect(line.points[0],line.points[1]);doc.UpdateAnnotation(0,line);
        Require(Near(doc.Annotations(0).back().points[1].y,335),"arrow endpoint was not updated independently");
        doc.Undo();Require(Near(doc.Annotations(0).back().points[1].y,290),"endpoint undo failed");
        doc.Redo();Require(Near(doc.Annotations(0).back().points[1].y,335),"endpoint redo failed");
        const std::vector<Point> raw{{320,338},{330,295},{350,273},{370,300},{348,337},{368,359},{400,310},{425,285},{441,315},{470,325}};
        auto smooth=SmoothInk(raw);Require(smooth.size()>raw.size(),"freehand curve was not smoothed");
        Require(PointDistance(smooth.front(),raw.front())<.001f&&PointDistance(smooth.back(),raw.back())<.001f,"smoothing moved endpoints");
        for(auto p:smooth)Require(p.x>=320&&p.x<=470&&p.y>=273&&p.y<=359,"smoothing overshot its input hull");
        doc.AddAnnotation(0,Tool::Ink,{320,270,155,92},L"",{},raw,12,.8f,DefaultAnnotationStyle(Tool::Ink));
        auto ink=doc.Annotations(0).back();Require(ink.points.size()==smooth.size(),"preview and PDF use different ink paths");
        auto moved=TransformAnnotation(ink,{ink.bounds.x+8,ink.bounds.y+15,ink.bounds.w*1.2f,ink.bounds.h*.8f});doc.UpdateAnnotation(0,moved);
        Require(Near(doc.Annotations(0).back().points.front().x,moved.points.front().x),"ink transform did not preserve path geometry");
        doc.AddAnnotation(0,Tool::Note,{472,42,24,24},L"请确认高亮段落。\n多行中文便签。",{},{},12,1,DefaultAnnotationStyle(Tool::Note));
        auto note=doc.Annotations(0).back();Require(note.text.find(L"\n")!=std::wstring::npos,"multiline note lost line breaks");
        note.bounds.x-=15;note.style.color=0x36a56d;doc.UpdateAnnotation(0,note);
        Require(doc.Annotations(0).back().style.color==note.style.color,"note color edit failed");
        auto image=fs::path(__FILE__).parent_path()/L"fixtures/transparent.png";
        doc.AddAnnotation(0,Tool::Image,{330,153,156,90},L"",image);
        auto stamp=doc.Annotations(0).back();const float ratio=stamp.bounds.w/stamp.bounds.h;
        const auto opaque=doc.Render(0,1);stamp.opacity=.4f;doc.UpdateAnnotation(0,stamp);
        Require(doc.Render(0,1).bgra!=opaque.bgra,"image opacity metadata did not change visible pixels");
        stamp=doc.Annotations(0).back();stamp.bounds.w*=.9f;stamp.bounds.h*=.9f;doc.UpdateAnnotation(0,stamp);
        Require(Near(doc.Annotations(0).back().bounds.w/doc.Annotations(0).back().bounds.h,ratio),"image resizing changed aspect ratio");
        stamp=doc.Annotations(0).back();stamp.opacity=1;doc.UpdateAnnotation(0,stamp);
        const auto beforeSave=doc.Render(0,1);
        Require(doc.Annotations(0).size()==6,"six tools were not created");
        doc.Save(out/L"annotation-showcase.pdf");BitmapFile(out/L"annotation-showcase.bmp",beforeSave);
        Document reopen;reopen.Open(out/L"annotation-showcase.pdf");
        auto annotations=reopen.Annotations(0);Require(annotations.size()==6,"annotations flattened or lost after reopening");
        Require(annotations[1].style==box.style,"rectangle appearance metadata changed after save");
        Require(annotations[2].style.endEnding==5&&annotations[3].points.size()==smooth.size(),"arrow/ink geometry lost after save");
        Require(annotations[4].text.find(L"多行中文")!=std::wstring::npos,"note contents lost after save");
        const auto afterSave=reopen.Render(0,1);Require(beforeSave.bgra==afterSave.bgra,"visible appearance changed after save/reopen");
        for(auto a:annotations){
            a.bounds.x+=12;a.bounds.y+=8;a.opacity=.6f;reopen.UpdateAnnotation(0,a);
            Require(reopen.Annotations(0).size()==6,"editing removed annotations");
            reopen.Undo();Require(reopen.Annotations(0).size()==6,"undo changed annotation count");reopen.Redo();
        }
        const auto oldSize=reopen.Annotations(0).size();reopen.DeleteAnnotation(0,reopen.Annotations(0).front().id);
        Require(reopen.Annotations(0).size()==oldSize-1,"delete failed");reopen.Undo();Require(reopen.Annotations(0).size()==oldSize,"delete undo failed");
        doc.AddAnnotation(0,Tool::Highlight,{64,405,430,28},L"",{},{},12,.3f,highlight);
        Require(doc.Annotations(0).back().areaHighlight,"explicit area highlight not marked");
        doc.AddAnnotation(0,Tool::Ink,{80,470,2,2},L"",{},{{80,470}},12,1);
        Require(doc.Annotations(0).back().points.size()==2,"a pen tap must create a round dot");
        const auto dot=doc.RenderAnnotation(0,doc.Annotations(0).back().id,3);size_t painted=0;
        for(size_t i=3;i<dot.pixels.bgra.size();i+=4)if(dot.pixels.bgra[i]>0)++painted;
        Require(painted>0,"a pen tap rendered an invisible dot");
        doc.AddAnnotation(0,Tool::Ink,{100,470,150,2},L"",{},{{100,470},{250,470}});
        Require(!doc.RenderAnnotation(0,doc.Annotations(0).back().id,2).pixels.bgra.empty(),"horizontal ink failed to render");
        const auto count=doc.Annotations(0).size();bool invalid=false;
        try{auto bad=rectangle;bad.lineWidth=std::numeric_limits<float>::quiet_NaN();doc.AddAnnotation(0,Tool::Rectangle,{40,40,100,100},L"",{},{},12,1,bad);}catch(...){invalid=true;}
        Require(invalid&&doc.Annotations(0).size()==count,"invalid appearance partially mutated the document");
        Require(ReadBytes(textPdf)==sourceBytes,"annotation operations overwrote their source");
        auto square=ConstrainEndpoint({10,10},{40,20},Tool::Rectangle,true);Require(Near(square.x-10,square.y-10),"Shift rectangle constraint failed");
        auto diagonal=ConstrainEndpoint({0,0},{100,84},Tool::Arrow,true);Require(Near(diagonal.x,diagonal.y),"Shift arrow angle constraint failed");
        const auto edge=ConstrainToPage({570,810},{640,850},Tool::Rectangle,true,{595,842,0,0});
        Require(Near(edge.x-570,edge.y-810)&&edge.x<=595&&edge.y<=842,"Shift square lost its ratio at the page edge");
        std::cout<<"PASS annotation tools: "<<assertions<<" assertions; style, geometry, multiline highlights, ink, image opacity, undo and editable save.\n";
    }catch(const std::exception& e){std::cerr<<"FAIL annotation tools after "<<assertions<<" assertions: "<<e.what()<<'\n';return 1;}
}