// AcroForm 表单填写：字段读取、文本 / 选项 / 复选框 / 单选、只读与长度限制、撤销、重置、下一项、中文外观、保存往返。
#include "core/document.h"
#include <windows.h>
#include <algorithm>
#include <iostream>
using namespace lpdf;
namespace {
int assertions=0;
void Require(bool okay,const char* why){++assertions;if(!okay)throw std::runtime_error(why);}
template<class F> bool Throws(F f){try{f();}catch(...){return true;}return false;}
FormField Field(Document& d,int page,std::wstring_view name,int nth=0){
    for(auto& f:d.FormFields(page))if(f.name==name&&nth--==0)return f;
    throw std::runtime_error("field not found: "+Utf8(name));
}
// 字段框内的“墨迹”像素（明显比白色暗）数量。
size_t Ink(Document& d,const FormField& f){
    const float scale=2;const auto page=d.Render(f.page,scale);const auto info=d.Info().pages[f.page];
    size_t n=0;
    const int x0=static_cast<int>((f.bounds.x-info.originX)*scale),y0=static_cast<int>((f.bounds.y-info.originY)*scale);
    const int x1=static_cast<int>((f.bounds.x+f.bounds.w-info.originX)*scale),y1=static_cast<int>((f.bounds.y+f.bounds.h-info.originY)*scale);
    for(int y=std::max(0,y0+2);y<std::min(page.height,y1-2);++y)for(int x=std::max(0,x0+2);x<std::min(page.width,x1-2);++x){
        const auto* p=&page.bgra[static_cast<size_t>(y)*page.stride+x*4];if(p[0]+p[1]+p[2]<300)++n;
    }
    return n;
}
}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc==5&&std::wstring_view(argv[1])==L"--ui"){
            // 启动真实的 LumenPDF（--smoke），程序内冒烟模拟单击字段：复选 / 单选 / 下拉 / 文本对话框 / 保存并下一项 /
            // 超长拒绝后重开 / 只读不可编辑，最后另存；这里核对保存结果且原文件未被修改。
            const fs::path exe=fs::absolute(argv[2]),fixture=fs::absolute(argv[3]),out=fs::absolute(argv[4]);
            std::error_code error;fs::remove_all(out,error);fs::create_directories(out);
            const auto before=ReadBytes(fixture);const auto result=out/L"filled-ui.pdf";
            SetEnvironmentVariableW(L"LPDF_FORM_SMOKE",result.c_str());
            SetEnvironmentVariableW(L"LPDF_SMOKE_TIMEOUT",L"40");
            SetEnvironmentVariableW(L"LPDF_NO_DEFAULT_PROMPT",L"1");
            std::wstring command=L"\""+exe.wstring()+L"\" --smoke \""+fixture.wstring()+L"\"";
            STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
            Require(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,out.c_str(),&si,&pi)!=0,"cannot start LumenPDF");
            const DWORD wait=WaitForSingleObject(pi.hProcess,55000);
            if(wait!=WAIT_OBJECT_0)TerminateProcess(pi.hProcess,3);
            CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
            Require(wait==WAIT_OBJECT_0,"LumenPDF form smoke timed out");
            if(!fs::exists(result)){
                std::string why="form smoke did not save";
                const auto failFile=fs::path(result.wstring()+L".fail.txt");
                if(fs::exists(failFile)){const auto bytes=ReadBytes(failFile);why+=": "+std::string(bytes.begin(),bytes.end());}
                throw std::runtime_error(why);
            }
            Require(ReadBytes(fixture)==before,"source PDF must stay untouched");
            Document saved;saved.Open(result);
            Require(Field(saved,0,L"name").value==L"张三","UI text field (and over-long value rejected)");
            Require(Field(saved,0,L"notes").value.empty(),"cancelled next-field dialog must not write");
            Require(Field(saved,0,L"account").value==L"A-001","read-only field unchanged");
            Require(Field(saved,0,L"agree").checked,"UI checkbox");
            Require(Field(saved,0,L"plan",1).checked&&!Field(saved,0,L"plan",0).checked,"UI radio");
            Require(Field(saved,0,L"city").value==L"sh","UI combo box");
            Require(Field(saved,0,L"color").value==L"Green","untouched list box");
            std::cout<<"PASS form UI: "<<assertions<<" assertions; checkbox, radio, combo, text dialog, save & next, max length, read-only, save.\n";
            return 0;
        }
        if(argc<3)throw std::runtime_error("usage: form_tests <fixtures> <output>");
        const fs::path fixtures=argv[1],out=argv[2];
        std::error_code error;fs::remove_all(out,error);fs::create_directories(out);
        const auto source=fixtures/L"form.pdf";const auto sourceBytes=ReadBytes(source);
        {Document closed;Require(Throws([&]{closed.Info();}),"Info() on a closed document must throw normally (not crash on the PDF trailer)");}
        Document d;d.Open(source);
        Require(d.Info().hasForm,"form must be detected");
        {const auto plain=out/L"plain.pdf";Document::TextToPdf(L"no form",plain);Document p;p.Open(plain);Require(!p.Info().hasForm&&p.FormFields(0).empty(),"plain PDF has no form");}

        // 1. 字段读取
        auto fields=d.FormFields(0);
        Require(fields.size()==9,"page 1 widget count");
        const auto name=Field(d,0,L"name");
        Require(name.type==FieldType::Text&&name.label==L"Full name"&&name.maxLength==20&&!name.multiline&&name.Fillable(),"text field");
        Require(name.bounds.w>199&&name.bounds.w<201&&name.bounds.h>19&&name.bounds.h<21,"text field bounds");
        Require(Field(d,0,L"notes").multiline,"multiline flag");
        const auto account=Field(d,0,L"account");
        Require(account.readOnly&&!account.Fillable()&&account.value==L"A-001","read-only field");
        const auto agree=Field(d,0,L"agree");
        Require(agree.type==FieldType::CheckBox&&!agree.checked&&agree.onState==L"Yes","checkbox");
        const auto basic=Field(d,0,L"plan",0),pro=Field(d,0,L"plan",1);
        Require(basic.type==FieldType::Radio&&pro.type==FieldType::Radio&&basic.onState==L"basic"&&pro.onState==L"pro"&&!basic.checked,"radio group");
        const auto city=Field(d,0,L"city");
        Require(city.type==FieldType::ComboBox&&city.options==std::vector<std::wstring>{L"Taipei",L"Tokyo",L"Shanghai"}&&city.exports[2]==L"sh"&&city.value==L"Taipei"&&!city.editable,"combo box");
        const auto color=Field(d,0,L"color");
        Require(color.type==FieldType::ListBox&&color.options.size()==3&&color.value==L"Green","list box");
        const auto sig=Field(d,0,L"signature");
        Require(sig.type==FieldType::Signature&&!sig.signedField&&!sig.Fillable(),"signature field");
        Require(d.Annotations(0).empty(),"widgets must not appear as annotations");

        // 2. 文本填写 + 外观
        const auto blank=Ink(d,name);
        d.SetFieldValue(0,name.id,L"Lumen Tester");
        Require(Field(d,0,L"name").value==L"Lumen Tester","text value");
        Require(Ink(d,Field(d,0,L"name"))>blank+40,"text appearance must show the value");
        Require(d.Info().dirty,"filling marks the document dirty");
        d.Undo();Require(Field(d,0,L"name").value.empty(),"undo restores the text");
        d.Redo();Require(Field(d,0,L"name").value==L"Lumen Tester","redo");
        // 中文：Helvetica 没有汉字，外观必须仍然可见。
        d.SetFieldValue(0,name.id,L"张三 Zhang");
        Require(Field(d,0,L"name").value==L"张三 Zhang","CJK value stored");
        const auto cjkInk=Ink(d,Field(d,0,L"name"));
        std::cout<<"CJK ink pixels: "<<cjkInk<<" (blank "<<blank<<")\n";
        {   // 供人工核对字形（不是豆腐块）：导出字段附近区域为 BMP。
            const auto page=d.Render(0,3);const auto f=Field(d,0,L"name");const auto info=d.Info().pages[0];
            const int x0=static_cast<int>((f.bounds.x-info.originX-10)*3),y0=static_cast<int>((f.bounds.y-info.originY-10)*3),w=static_cast<int>((f.bounds.w+20)*3),h=static_cast<int>((f.bounds.h+20)*3);
            std::vector<unsigned char> px(static_cast<size_t>(w)*h*4);
            for(int y=0;y<h;++y)std::copy_n(&page.bgra[static_cast<size_t>(y0+y)*page.stride+x0*4],w*4,&px[static_cast<size_t>(y)*w*4]);
            BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);file.bfSize=file.bfOffBits+static_cast<DWORD>(px.size());
            BITMAPINFOHEADER bi{};bi.biSize=sizeof(bi);bi.biWidth=w;bi.biHeight=-h;bi.biPlanes=1;bi.biBitCount=32;bi.biCompression=BI_RGB;
            FILE* fp=nullptr;if(_wfopen_s(&fp,(out/L"cjk-field.bmp").c_str(),L"wb")==0&&fp){fwrite(&file,sizeof(file),1,fp);fwrite(&bi,sizeof(bi),1,fp);fwrite(px.data(),1,px.size(),fp);fclose(fp);}
        }
        Require(cjkInk>blank+80,"CJK text must be visible in the field appearance");
        // 单行去换行、多行保留
        d.SetFieldValue(0,name.id,L"a\r\nb");Require(Field(d,0,L"name").value==L"ab","single-line field drops line breaks");
        d.SetFieldValue(0,Field(d,0,L"notes").id,L"line 1\nline 2");Require(Field(d,0,L"notes").value.find(L'\n')!=std::wstring::npos,"multiline keeps line breaks");
        // 拒绝：超长、只读、非选项、错误类型
        const auto before=Field(d,0,L"name").value;
        Require(Throws([&]{d.SetFieldValue(0,name.id,std::wstring(21,L'x'));})&&Field(d,0,L"name").value==before,"max length enforced without change");
        Require(Throws([&]{d.SetFieldValue(0,account.id,L"hack");})&&Field(d,0,L"account").value==L"A-001","read-only enforced");
        Require(Throws([&]{d.SetFieldValue(0,city.id,L"Paris");})&&Field(d,0,L"city").value==L"Taipei","combo rejects unknown values");
        Require(Throws([&]{d.SetFieldValue(0,agree.id,L"Yes");}),"checkbox does not take text");
        Require(Throws([&]{d.ToggleField(0,name.id);}),"text field cannot be toggled");
        Require(Throws([&]{d.SetFieldValue(0,999999,L"x");}),"missing field rejected");

        // 3. 选项
        d.SetFieldValue(0,city.id,L"Shanghai");Require(Field(d,0,L"city").value==L"sh","display text maps to export value");
        d.SetFieldValue(0,city.id,L"Tokyo");Require(Field(d,0,L"city").value==L"Tokyo","combo value");
        d.SetFieldValue(0,color.id,L"Blue");Require(Field(d,0,L"color").value==L"Blue","list box value");

        // 4. 复选框 / 单选
        d.ToggleField(0,agree.id);Require(Field(d,0,L"agree").checked&&Field(d,0,L"agree").value==L"Yes","checkbox on");
        d.ToggleField(0,agree.id);Require(!Field(d,0,L"agree").checked,"checkbox off");
        d.ToggleField(0,agree.id);
        d.ToggleField(0,basic.id);Require(Field(d,0,L"plan",0).checked&&!Field(d,0,L"plan",1).checked,"radio basic");
        d.ToggleField(0,pro.id);Require(!Field(d,0,L"plan",0).checked&&Field(d,0,L"plan",1).checked&&Field(d,0,L"plan",1).value==L"pro","radio switches within the group");
        d.Undo();Require(Field(d,0,L"plan",0).checked,"undo radio switch");
        d.Redo();

        // 5. 下一项
        const auto notes=Field(d,0,L"notes");const auto page2=Field(d,1,L"page2");
        auto next=d.NextFillableField(0,name.id);Require(next&&next->id==notes.id,"next after name is notes");
        next=d.NextFillableField(0,notes.id);Require(next&&next->id==city.id,"next skips read-only / buttons");
        next=d.NextFillableField(0,notes.id,true);Require(next&&next->id==page2.id,"text-only next crosses pages");
        next=d.NextFillableField(1,page2.id);Require(next&&next->id==name.id,"next wraps to the start");
        next=d.NextFillableField(0,-1);Require(next&&next->id==name.id,"first fillable field");

        // 6. 保存往返（源文件不变）
        const auto saved=out/L"filled.pdf";d.Save(saved);
        Require(ReadBytes(source)==sourceBytes,"filling must not overwrite the source");
        {
            Document r;r.Open(saved);
            Require(Field(r,0,L"name").value==L"ab"&&Field(r,0,L"city").value==L"Tokyo"&&Field(r,0,L"color").value==L"Blue","saved values");
            Require(Field(r,0,L"agree").checked&&Field(r,0,L"plan",1).checked&&!Field(r,0,L"plan",0).checked,"saved check states");
            Require(Field(r,0,L"notes").value.find(L"line 2")!=std::wstring::npos,"saved multiline");
            Require(Ink(r,Field(r,0,L"name"))>blank+10,"saved appearance visible");
            // 7. 重置
            r.ResetForm();
            Require(Field(r,0,L"name").value.empty()&&!Field(r,0,L"agree").checked&&!Field(r,0,L"plan",1).checked,"reset clears values");
            Require(Field(r,0,L"account").value==L"A-001"||Field(r,0,L"account").value.empty(),"reset keeps the document readable");
            r.Undo();Require(Field(r,0,L"name").value==L"ab"&&Field(r,0,L"agree").checked,"undo reset");
        }
        std::cout<<"PASS form filling: "<<assertions<<" assertions; fields, text/choice/check/radio, limits, undo, next field, CJK appearance, reset and save.\n";
    }catch(const std::exception& e){std::cerr<<"FAIL form filling after "<<assertions<<" assertions: "<<e.what()<<'\n';return 1;}
    return 0;
}
