#include "core/document.h"
#include "core/font_catalog.h"
#include "app/inline_editor.h"
#include "app/text_format_bar.h"
#include "app/text_layout.h"
#include <lumen/Window.h>
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <lumen/App.h>
#include <lumen/ComboBox.h>
#include <lumen/NumberBox.h>
#include <lumen/ToggleButton.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <type_traits>
using namespace lpdf;
namespace {
int assertions=0;
void Require(bool condition,const char* message){++assertions;if(!condition)throw std::runtime_error(message);}
bool Near(float a,float b){return std::abs(a-b)<.12f;}
bool Same(Rect a,Rect b){return Near(a.x,b.x)&&Near(a.y,b.y)&&Near(a.w,b.w)&&Near(a.h,b.h);}
#pragma warning(push)
#pragma warning(disable:4611)
template<class F> auto Call(fz_context* ctx,F&& fn){
    using R=std::invoke_result_t<F>;
    if constexpr(std::is_void_v<R>){fz_try(ctx){fn();}fz_catch(ctx){throw std::runtime_error(fz_caught_message(ctx));}}
    else{R value{};fz_var(value);fz_try(ctx){value=fn();}fz_catch(ctx){throw std::runtime_error(fz_caught_message(ctx));}return value;}
}
#pragma warning(pop)
struct Audit {
    fz_context* ctx=fz_new_context(nullptr,nullptr,FZ_STORE_DEFAULT);pdf_document* doc{};
    explicit Audit(const fs::path& path){const auto name=Utf8(path.wstring());doc=Call(ctx,[&]{return pdf_open_document(ctx,name.c_str());});}
    ~Audit(){pdf_drop_document(ctx,doc);fz_drop_context(ctx);}
    int EmbeddedFonts(){
        int count=0;const int length=Call(ctx,[&]{return pdf_xref_len(ctx,doc);});
        for(int i=1;i<length;++i){
            auto* obj=Call(ctx,[&]{return pdf_load_object(ctx,doc,i);});
            const bool descriptor=Call(ctx,[&]{return pdf_name_eq(ctx,pdf_dict_get(ctx,obj,PDF_NAME(Type)),PDF_NAME(FontDescriptor));})!=0;
            if(descriptor){auto* stream=Call(ctx,[&]{auto* s=pdf_dict_get(ctx,obj,PDF_NAME(FontFile2));return s?s:pdf_dict_get(ctx,obj,PDF_NAME(FontFile3));});
                if(stream&&Call(ctx,[&]{return pdf_is_stream(ctx,stream);}))++count;}
            pdf_drop_obj(ctx,obj);
        }
        return count;
    }
};
void Core(const fs::path& out){
    const auto families=InstalledFontFamilies();Require(!families.empty(),"font enumeration empty");
    Require(std::find(families.begin(),families.end(),L"Arial")!=families.end(),"Windows Arial not enumerated");
    for(const auto* family:{L"Arial",L"Times New Roman",L"Courier New",L"Microsoft YaHei"}){
        const auto source=ResolveSystemFont(family,true,true);Require(fs::is_regular_file(source.path),"font source not a local file");
    }
    bool rejected=false;try{ResolveSystemFont(L"Lumen Font Not Installed 123");}catch(...){rejected=true;}
    Require(rejected,"unknown font silently substituted");
    Document doc;doc.New();Annotation a;a.id=-1;a.type=Tool::Text;a.bounds={60,160,360,132};a.fixedTextBox=true;
    a.fontSize=20;a.text=L"Font proof: MWii 123 & <PDF>\n第二行：中文与字体";a.textFormat.family=L"Arial";
    a.textFormat.color=0x2456a8;a.textFormat.bold=true;a.textFormat.italic=true;a.textFormat.underline=true;a.textFormat.alignment=1;
    doc.AddTextAnnotation(0,a);auto value=doc.Annotations(0).front();const auto id=value.id;
    Require(value.textFormat==a.textFormat,"new font format not readable");Require(value.fixedTextBox,"new fixed box marker missing");
    Require(Same(value.bounds,a.bounds),"creation changed explicit frame dimensions");
    Require(doc.Text(0).find(L"<PDF>")!=std::wstring::npos,"XML escaping changed visible text");
    Require(doc.Text(0).find(L"第二行")!=std::wstring::npos,"font fallback lost Chinese glyphs");
    auto render=doc.Render(0,1);
    auto familyOnly=value;familyOnly.textFormat.family=L"Courier New";doc.UpdateAnnotation(0,familyOnly);
    Require(doc.Render(0,1).bgra!=render.bgra,"changing only the font family did not affect actual PDF glyphs");
    doc.Undo();Require(doc.Annotations(0).front().textFormat==value.textFormat,"font-only undo failed");
    const auto path=out/L"formatted-text.pdf";doc.Save(path);Document reopen;reopen.Open(path);
    auto round=reopen.Annotations(0).front();
    Require(round.text==a.text&&round.textFormat==a.textFormat,"saved format/content changed");
    Require(round.fixedTextBox&&Same(round.bounds,a.bounds),"saved box width/height changed");
    Require(reopen.Render(0,1).bgra==render.bgra,"embedded font render changed after reopening");
    {Audit audit(path);Require(audit.EmbeddedFonts()>=1,"font was only changed in UI: PDF contains no embedded font stream");}
    value.fontSize=26;value.textFormat.family=L"Times New Roman";value.textFormat.alignment=2;
    doc.UpdateAnnotation(0,value);Require(Same(doc.Annotations(0).front().bounds,a.bounds),"font change shrank a fixed text box");
    Require(doc.Render(0,1).bgra!=render.bgra,"font/style change not visible in PDF rendering");
    doc.Undo();Require(doc.Annotations(0).front().textFormat==a.textFormat,"font change not one undo step");
    Require(doc.Render(0,1).bgra==render.bgra,"font undo changed original appearance");
    doc.Redo();Require(doc.Annotations(0).front().textFormat==value.textFormat,"font redo failed");
    value=doc.Annotations(0).front();value.bounds.h=210;value.geometryEdited=true;doc.UpdateAnnotation(0,value);
    Require(Near(doc.Annotations(0).front().bounds.h,210),"vertical resize was auto-fitted away");
    value.bounds.h=48;doc.UpdateAnnotation(0,value);Require(Near(doc.Annotations(0).front().bounds.h,48),"shortened height grew back");
    value.bounds.w=210;value.bounds.x+=25;value.bounds.y+=30;value.fontSize=16;doc.UpdateAnnotation(0,value);
    Require(Same(doc.Annotations(0).front().bounds,value.bounds),"resize/move altered opposite geometry");
    const auto resize=out/L"resized-text.pdf";doc.Save(resize);Document resized;resized.Open(resize);
    Require(Same(resized.Annotations(0).front().bounds,value.bounds),"resized frame not durable across save/reopen");
    const auto safe=doc.Render(0,.8f).bgra;const auto unchanged=doc.Annotations(0).front();
    auto invalid=value;invalid.textFormat.family=L"Lumen Font Not Installed 123";rejected=false;
    try{doc.UpdateAnnotation(0,invalid);}catch(...){rejected=true;}
    Require(rejected&&doc.Annotations(0).size()==1,"invalid font must fail atomically");
    Require(doc.Annotations(0).front().textFormat==unchanged.textFormat&&doc.Render(0,.8f).bgra==safe,"invalid font changed existing annotation");
    Require(doc.Annotations(0).front().id==id,"text editing created an overlapping replacement annotation");
    Document samples;samples.New();int index=0;
    for(const auto* family:{L"Arial",L"Times New Roman",L"Courier New",L"Microsoft YaHei"}){
        Annotation sample;sample.id=-1;sample.type=Tool::Text;sample.bounds={55.0f,80.0f+index*164.0f,470,132};sample.fixedTextBox=true;sample.fontSize=22;
        sample.text=std::wstring(family)+L"\nPage-side editing 012345\n中文字体 · 多行文字";sample.textFormat.family=family;
        sample.textFormat.bold=index==1;sample.textFormat.italic=index==2;sample.textFormat.underline=index==3;
        sample.textFormat.color=index%2?0x2456a8:0x242f40;samples.AddTextAnnotation(0,sample);++index;
    }
    const auto samplePath=out/L"text-font-samples.pdf";samples.Save(samplePath);Document sampleReopen;sampleReopen.Open(samplePath);
    const auto list=sampleReopen.Annotations(0);Require(list.size()==4,"font samples lost after reopen");
    for(size_t i=0;i<list.size();++i){Require(list[i].textFormat==samples.Annotations(0)[i].textFormat,"font family/style roundtrip failed");Require(list[i].fixedTextBox,"sample fixed frame lost");}
    {Audit audit(samplePath);Require(audit.EmbeddedFonts()>=4,"chosen families not embedded as separate fonts");}
    const auto uiSource=ResolveSystemFont(L"Microsoft YaHei UI",false,true);
    Require(uiSource.faceIndex>0,"TTC regression must exercise a nonzero font face");
    Require(uiSource.simulatedItalic,"font without italic face needs synthetic oblique");
    Document ttc;ttc.New();Annotation ui=a;ui.textFormat.family=L"Microsoft YaHei UI";ui.textFormat.bold=false;ui.textFormat.italic=true;
    ttc.AddTextAnnotation(0,ui);const auto ttcRender=ttc.Render(0,1).bgra;const auto ttcPath=out/L"ttc-face-text.pdf";ttc.Save(ttcPath);
    Document ttcAgain;ttcAgain.Open(ttcPath);Require(ttcAgain.Render(0,1).bgra==ttcRender,"TTC face or synthetic style changed after save");
    Document aligned;aligned.New();Annotation automatic=a;automatic.fixedTextBox=false;automatic.text=L"Centered text";automatic.textFormat.alignment=1;
    aligned.AddTextAnnotation(0,automatic);auto centerBox=aligned.Annotations(0).front();
    Require(Near(centerBox.bounds.w,automatic.bounds.w),"auto-fit shifted centered text by tightening its wrap width");
    centerBox.textFormat.alignment=2;aligned.UpdateAnnotation(0,centerBox);
    Require(Near(aligned.Annotations(0).front().bounds.w,automatic.bounds.w),"auto-fit shifted right-aligned text");
    Document fixture;fixture.New();Annotation first;first.id=-1;first.type=Tool::Text;first.bounds={65,250,360,94};first.fixedTextBox=true;
    first.fontSize=18;first.text=L"Page-side editing\n在页面上直接编辑文字";first.textFormat.family=L"Arial";first.textFormat.color=0x2456a8;fixture.AddTextAnnotation(0,first);
    first.bounds={65,470,360,80};first.fontSize=16;first.text=L"Second annotation — independent";first.textFormat.family=L"Times New Roman";first.textFormat.color=0x263043;fixture.AddTextAnnotation(0,first);
    fixture.Save(out/L"text-editor-fixture.pdf");
    Document automaticFixture;automaticFixture.New();
    Annotation label;label.type=Tool::Text;label.id=-1;label.text=L"水电费";label.fontSize=12;
    label.bounds={65,250,360,370};label.fixedTextBox=true;label.textSizing=TextSizing::Auto;
    automaticFixture.AddTextAnnotation(0,label);
    auto read=automaticFixture.Annotations(0).front();Require(read.lumenText&&read.textSizing==TextSizing::Auto,"text layer/sizing metadata missing");
    const auto normal=automaticFixture.Render(0,1).bgra;
    const auto suppressed=automaticFixture.Render(0,1,{},-1,true).bgra;
    Require(normal!=suppressed,"page render did not omit Lumen text for the shared UI layer");
    Require(automaticFixture.Render(0,1).bgra==normal,"UI-only suppression modified the PDF appearance");
    automaticFixture.Save(out/L"text-auto-fixture.pdf");
    Document stacked;stacked.New();stacked.AddTextAnnotation(0,label);
    AnnotationStyle cover;cover.filled=true;cover.fillColor=0x24844b;
    stacked.AddAnnotation(0,Tool::Rectangle,label.bounds,{}, {}, {},12,1,cover);
    Require(!stacked.Annotations(0).front().lumenText,"text overlay would incorrectly rise above a later opaque annotation");
    Require(stacked.Render(0,1,{},-1,true).bgra==stacked.Render(0,1).bgra,"UI layer changed PDF annotation stacking order");
    auto top=label;top.text=L"Top text";stacked.AddTextAnnotation(0,top);
    Require(stacked.Annotations(0).back().lumenText,"topmost Lumen text was not eligible for consistent UI rendering");


}
lumen::Control* Find(lumen::Control& node,std::wstring_view name){
    if(node.AccessibleName()==name)return &node;
    if(auto* panel=node.AsPanel())for(size_t i=0;i<panel->ChildCount();++i)if(auto* found=Find(panel->Child(i),name))return found;
    return nullptr;
}
void Native(){
    lumen::App::Ensure();
    lumen::Window window(lumen::WindowSpec{.title=L"LumaText editing regression",.size={720,520},.titleBar=false});
    auto& host=window.Root().Add<lumen::Panel>();host.Grow();
    const auto owner=static_cast<HWND>(window.NativeHandle());
    InlineEditor editor;editor.Attach(host);
    Annotation a;a.type=Tool::Text;a.fontSize=18;a.text=L"First line\n第二行编辑";
    a.textFormat.family=L"Courier New";a.textFormat.bold=true;a.textFormat.italic=true;
    a.textFormat.underline=true;a.textFormat.color=0xa82c49;a.textFormat.alignment=2;
    bool finished=false;editor.Begin(owner,a,[&](bool apply){finished=apply;});
    window.Show(false);window.LayoutNow();editor.Place({100,200,320,100},{0,0,720,520},4.0f/3.0f,1.5f);window.LayoutNow();
    Require(editor.Text()==a.text,"LUMEN Unicode/newline content changed");
    auto* field=editor.View();Require(field&&field->Parent()==&host,"editor is not attached to the canvas tree");
    const auto style=field->Typography();
    Require(style.family==L"Courier New"&&Near(style.size,24),"18 PDF pt must be 24 DIP at actual-size zoom, independent of DPI");
    Require(style.weight==700&&style.italic&&style.underline,"LumaText editor typography missing B/I/U");
    Require(style.alignment==lumen::Align::Trailing,"LumaText editor alignment missing");
    field->Select(2,8);a.textFormat.family=L"Arial";editor.Style(20,a.textFormat);
    Require(field->SelectionStart()==2&&field->SelectionEnd()==8,"format change discarded selection");
    Require(editor.RequiredHeight()>40,"shared paragraph height measurement is not usable");
    field->Select(a.text.size(),a.text.size());editor.Focus();SendMessageW(owner,WM_CHAR,L'!',0);
    editor.Style(22,a.textFormat);editor.UndoText();Require(editor.Text()==a.text,"formatting consumed text undo");
    editor.RedoText();Require(editor.Text()==a.text+L"!","LUMEN text redo failed");editor.UndoText();
    Annotation small;small.type=Tool::Text;small.fontSize=12;small.text=L"水电费";small.bounds={65,250,360,370};
    lumen::TextLayout measure;const PageInfo page{595.28f,841.89f,0,0};
    const auto snug=FitTextBounds(small,page,measure);
    Require(snug.w<60&&snug.h<30,"short 12pt label retained an oversized drawing frame");
    Require(Near(AnnotationTypography(small,96.0f/72.0f).size,16),"12pt-to-DIP conversion is not 16 DIP at 100 percent");
    small.textSizing=TextSizing::Width;small.bounds.w=85;small.text=L"Automatic word wrapping and 中文换行 needs several lines.";
    const auto wrapped=FitTextBounds(small,page,measure);
    Require(Near(wrapped.w,85)&&wrapped.h>40,"explicit wrap width did not reflow and grow vertically");
    small.text=L"短句";Require(FitTextBounds(small,page,measure).h<wrapped.h,"deleting text did not shrink automatic height");
    TextFormatBar bar;int changes=0;bool cancelled=false;
    bar.Begin(owner,a,[&](TextFormat f,float size){++changes;a.textFormat=f;a.fontSize=size;editor.Style(size,f);},
        [&](bool apply){cancelled=!apply;},[&]{editor.Focus();},[]{});
    bar.Place({100,200,320,100},{0,0,720,520},1);
    Require(IsWindowVisible(bar.Handle())!=FALSE,"page-side format bar not visible");
    auto& family=*dynamic_cast<lumen::ComboBox*>(Find(bar.View()->Root(),L"字体"));
    auto& size=*dynamic_cast<lumen::NumberBox*>(Find(bar.View()->Root(),L"字号"));
    a.fontSize=18.125f;bar.Sync(a);const int unchanged=changes;
    Require(bar.CommitFields()&&a.fontSize==18.125f&&changes==unchanged,"display rounding altered an untouched fractional font size");
    Require(family.Count()>10,"installed font collection not available");
    family.Text(L"Times New Roman");size.Text(L"24.5");
    Require(bar.CommitFields()&&a.textFormat.family==L"Times New Roman"&&Near(a.fontSize,24.5f),"font and size fields not committed");
    size.Text(L"999");const auto count=changes;
    Require(!bar.CommitFields()&&changes==count,"invalid size changed draft");size.Text(L"24.5");
    family.Text(L"Lumen Font Not Installed 123");Require(!bar.CommitFields(),"invalid font was accepted");family.Text(L"Times New Roman");
    const bool bold=a.textFormat.bold;Find(bar.View()->Root(),L"加粗")->AutomationToggle();
    Require(a.textFormat.bold!=bold,"format toggle failed");
    Find(bar.View()->Root(),L"左对齐")->AutomationToggle();Require(a.textFormat.alignment==0,"alignment toggle failed");
    Find(bar.View()->Root(),L"取消文字编辑")->AutomationInvoke();Require(cancelled,"format cancellation callback failed");
    bar.End();editor.Focus();window.DispatchKey(VK_ESCAPE);Require(!finished,"Escape committed the draft");editor.End();
    Require(!editor.Active()&&!field->Visible(),"page editor did not detach input when dismissed");
    window.Close();
}
void VerifyInline(const fs::path& file){
    Document doc;doc.Open(file);const auto list=doc.Annotations(0);
    Require(list.size()==1,"inline editing created a second overlapping annotation");
    const auto& a=list.front();Require(a.text==L"水电费\n第二行", "inline Unicode/newline save failed");
    Require(Near(a.fontSize,12),"12pt was changed to compensate for screen zoom");
    Require(a.bounds.w<100&&a.bounds.h<60&&a.bounds.h>25,"automatic text frame was saved as an oversized rectangle");
    Require(a.textSizing==TextSizing::Auto&&a.lumenText,"automatic sizing mode or renderer marker not durable");
    Require(doc.Text(0).find(L"第二行")!=std::wstring::npos,"saved PDF appearance clipped its second line");
    Audit audit(file);Require(audit.EmbeddedFonts()>=1,"inline text lost real PDF font embedding");
}
void VerifyGui(const fs::path& file){
    Document doc;doc.Open(file);const auto list=doc.Annotations(0);Require(list.size()==2,"direct edit added overlapping annotation");
    const auto& a=list.front();Require(a.text.find(L"Direct editing verified")!=std::wstring::npos,"GUI editor text not saved");
    Require(a.textFormat.family==L"Times New Roman"&&Near(a.fontSize,24),"GUI font/size not saved");
    Require(a.textFormat.bold&&a.textFormat.italic&&a.textFormat.underline,"GUI B/I/U not saved");
    Require(a.textFormat.alignment==1,"GUI alignment not saved");Require(a.textFormat.color==0x7c3aed,"GUI text color not saved");Require(a.fixedTextBox&&a.bounds.h>130,"GUI vertical resize not durable");
    Require(Near(a.bounds.w,360)&&Near(a.bounds.x,65)&&Near(a.bounds.y,250),"GUI vertical resize changed width or top anchor");
    Require(list.back().text==L"Second annotation — independent","edit changed other annotation");
    {Audit audit(file);Require(audit.EmbeddedFonts()>=2,"GUI fonts not embedded");}
}
}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc>2&&std::wstring_view(argv[1])==L"--verify-inline")VerifyInline(argv[2]);
        else if(argc>2&&std::wstring_view(argv[1])==L"--verify-gui")VerifyGui(argv[2]);
        else{const fs::path out=argc>1?argv[1]:L"text-output";fs::create_directories(out);Core(out);Native();}
        std::cout<<"PASS text editing: "<<assertions<<" assertions\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL text editing: "<<e.what()<<" (after "<<assertions<<" assertions)\n";return 1;}
}
