// Excel / PowerPoint → PDF：扩展名分类、worker 参数分发、真实 Excel / PowerPoint 转换（多工作表、公式重算、宽屏幻灯片）、
// 源文件不被修改、不残留 Office 进程、PowerPoint 已在运行时拒绝借用（且不退出它）、LumenPDF.exe 作为 worker、合并。
// 用户自己正在使用 PowerPoint 时跳过 PowerPoint 部分（不干扰），并在输出中注明。
#include "core/conversion.h"
#include "core/platform.h"
#include <windows.h>
#include <oleauto.h>
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <set>
#include <thread>
using namespace lpdf;
namespace {
int assertions=0;std::vector<std::string> skipped;
void Require(bool okay,const std::string& why){++assertions;if(!okay)throw std::runtime_error(why);}
std::set<DWORD> Running(const wchar_t* exe){
    std::set<DWORD> r;HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);PROCESSENTRY32W e{sizeof(e)};
    if(Process32FirstW(s,&e))do{if(_wcsicmp(e.szExeFile,exe)==0)r.insert(e.th32ProcessID);}while(Process32NextW(s,&e));
    CloseHandle(s);return r;
}
// 转换结束后，本测试启动的 Office 进程应在数秒内退出。
void RequireNoLeftover(const wchar_t* exe,const std::set<DWORD>& before,const std::string& what){
    for(int i=0;i<60;++i){
        bool extra=false;for(DWORD id:Running(exe))if(!before.contains(id))extra=true;
        if(!extra){++assertions;return;}
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    throw std::runtime_error(what+": Office process left running");
}
std::wstring AllText(Document& d){std::wstring t;for(size_t i=0;i<d.Info().pages.size();++i)t+=d.Text(static_cast<int>(i))+L"\n";return t;}
bool Contains(const std::wstring& text,const wchar_t* part){return text.find(part)!=std::wstring::npos;}
std::string Message(const std::function<void()>& f){try{f();}catch(const std::exception& e){return e.what();}return {};}
uint64_t Stamp(const fs::path& f){return static_cast<uint64_t>(fs::last_write_time(f).time_since_epoch().count())^fs::file_size(f);}
DISPID Id(IDispatch* o,const wchar_t* name){DISPID id{};auto* n=const_cast<LPOLESTR>(name);if(FAILED(o->GetIDsOfNames(IID_NULL,&n,1,LOCALE_USER_DEFAULT,&id)))throw std::runtime_error("COM name");return id;}
VARIANT Get(IDispatch* o,const wchar_t* name){DISPPARAMS p{};VARIANT v;VariantInit(&v);if(FAILED(o->Invoke(Id(o,name),IID_NULL,LOCALE_USER_DEFAULT,DISPATCH_PROPERTYGET,&p,&v,nullptr,nullptr)))throw std::runtime_error("COM get");return v;}
void Call(IDispatch* o,const wchar_t* name){DISPPARAMS p{};VARIANT v;VariantInit(&v);o->Invoke(Id(o,name),IID_NULL,LOCALE_USER_DEFAULT,DISPATCH_METHOD,&p,&v,nullptr,nullptr);VariantClear(&v);}
int RunWorker(const fs::path& exe,const wchar_t* flag,const fs::path& source,const fs::path& output){
    std::wstring command=L"\""+exe.wstring()+L"\" "+flag+L" \""+source.wstring()+L"\" \""+output.wstring()+L"\"";
    STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))return -2;
    DWORD code=3;if(WaitForSingleObject(pi.hProcess,150000)==WAIT_OBJECT_0)GetExitCodeProcess(pi.hProcess,&code);else TerminateProcess(pi.hProcess,3);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return static_cast<int>(code);
}
}
int wmain(int argc,wchar_t** argv){
    if(argc==4){const int worker=OfficeWorkerMain(argv[1],argv[2],argv[3]);if(worker>=0)return worker;}
    try{
        if(argc<2)throw std::runtime_error("usage: office_tests <output> [LumenPDF.exe]");
        const fs::path out=fs::absolute(argv[1]);const fs::path exe=argc>2?fs::absolute(argv[2]):fs::path();
        const fs::path fixtures=fs::path(__FILE__).parent_path()/L"fixtures";
        std::error_code error;fs::remove_all(out,error);fs::create_directories(out);

        // ---- 分类与分发 ----
        for(const wchar_t* e:{L"a.xls",L"a.XLSX",L"a.xlsm",L"a.xlsb",L"a.ods"})Require(FileKind(e)==InputFileKind::Excel,"Excel extension "+Utf8(e));
        for(const wchar_t* e:{L"a.ppt",L"a.PPTX",L"a.pptm",L"a.pps",L"a.ppsx",L"a.odp"})Require(FileKind(e)==InputFileKind::PowerPoint,"PowerPoint extension "+Utf8(e));
        for(const wchar_t* e:{L"a.doc",L"a.DOCX",L"a.docm",L"a.rtf",L"a.odt"})Require(FileKind(e)==InputFileKind::Word,"Word extension "+Utf8(e));
        Require(FileKind(L"a.csv")==InputFileKind::Other&&FileKind(L"a.pdf")==InputFileKind::Pdf,"csv not claimed, pdf unchanged");
        Require(IsOfficeKind(InputFileKind::Excel)&&IsOfficeKind(InputFileKind::PowerPoint)&&IsOfficeKind(InputFileKind::Word)&&!IsOfficeKind(InputFileKind::Pdf),"office kinds");
        Require(std::wstring_view(OfficeWorkerFlag(InputFileKind::Excel))==L"--excel-worker"&&std::wstring_view(OfficeWorkerFlag(InputFileKind::PowerPoint))==L"--powerpoint-worker"&&std::wstring_view(OfficeWorkerFlag(InputFileKind::Word))==L"--word-worker","worker flags");
        Require(OfficeWorkerMain(L"--other",L"a",L"b")==-1,"unknown flag is not a worker");
        Converter converter;
        Require(Message([&]{converter.Convert(out/L"x.csv",{},{});}).find("Supported files")!=std::string::npos,"unsupported file message");
        if(Converter::FindLibreOffice().empty()){
            ConversionOptions lo;lo.office=OfficeBackend::LibreOffice;
            Require(Message([&]{converter.Convert(fixtures/L"excel-sample.xlsx",lo,{});}).find("Install Microsoft Office or LibreOffice")!=std::string::npos,"missing LibreOffice message");
        }else skipped.push_back("missing-LibreOffice message (LibreOffice is installed)");

        std::vector<MergeInput> merge;
        // ---- 真实 Excel ----
        if(Converter::OfficeAvailable(InputFileKind::Excel)){
            const auto source=fixtures/L"excel-sample.xlsx";const auto stamp=Stamp(source);const auto before=Running(L"EXCEL.EXE");
            std::vector<OperationProgress> events;
            auto r=converter.Convert(source,{},{},[&](const OperationProgress& e){events.push_back(e);});
            Require(r.backend==L"Microsoft Excel","Excel backend "+Utf8(r.backend));
            auto office=std::find_if(events.begin(),events.end(),[](const auto& e){return e.stage==ProgressStage::OfficeConversion;});
            Require(office!=events.end()&&office->backend==L"Microsoft Excel"&&events.back().stage==ProgressStage::ConversionReady,"Excel progress stages");
            Document d;d.Open(r.pdf);const auto text=AllText(d);
            Require(d.Info().pages.size()>=2,"both worksheets exported ("+std::to_string(d.Info().pages.size())+" pages)");
            Require(Contains(text,L"sheet one")&&Contains(text,L"Second sheet"),"worksheet text");
            Require(Contains(text,L"5500")||Contains(text,L"5,500"),"formulas recalculated (SUM = 5500)");
            Require(Stamp(source)==stamp,"Excel source untouched");
            RequireNoLeftover(L"EXCEL.EXE",before,"Excel");
            fs::copy_file(r.pdf,out/L"excel.pdf",fs::copy_options::overwrite_existing);merge.push_back({out/L"excel.pdf",L"",L""});
            if(!exe.empty()){
                const auto viaApp=out/L"excel-via-app.pdf";
                Require(RunWorker(exe,L"--excel-worker",source,viaApp)==0&&fs::exists(viaApp),"LumenPDF.exe --excel-worker");
                Document v;v.Open(viaApp);Require(v.Info().pages.size()==d.Info().pages.size(),"app worker output matches");
                RequireNoLeftover(L"EXCEL.EXE",before,"Excel via app");
            }
        }else skipped.push_back("Excel (not installed)");

        // ---- 真实 PowerPoint ----
        const auto userPowerPoint=Running(L"POWERPNT.EXE");
        if(!Converter::OfficeAvailable(InputFileKind::PowerPoint))skipped.push_back("PowerPoint (not installed)");
        else if(!userPowerPoint.empty())skipped.push_back("PowerPoint (the user has PowerPoint open; not disturbed)");
        else{
            const auto source=fixtures/L"ppt-sample.pptx";const auto stamp=Stamp(source);
            auto r=converter.Convert(source,{},{});
            Require(r.backend==L"Microsoft PowerPoint","PowerPoint backend "+Utf8(r.backend));
            Document d;d.Open(r.pdf);const auto info=d.Info();const auto text=AllText(d);
            Require(info.pages.size()==3,"three slides -> three pages ("+std::to_string(info.pages.size())+")");
            Require(std::abs(info.pages[0].width/info.pages[0].height-16.0f/9.0f)<0.02f,"16:9 slide size kept");
            Require(Contains(text,L"Slide two")&&Contains(text,L"conversion body 3"),"slide text");
            Require(Stamp(source)==stamp,"PowerPoint source untouched");
            RequireNoLeftover(L"POWERPNT.EXE",{},"PowerPoint");
            fs::copy_file(r.pdf,out/L"slides.pdf",fs::copy_options::overwrite_existing);merge.push_back({out/L"slides.pdf",L"",L""});
            if(!exe.empty()){
                const auto viaApp=out/L"slides-via-app.pdf";
                Require(RunWorker(exe,L"--powerpoint-worker",source,viaApp)==0&&fs::exists(viaApp),"LumenPDF.exe --powerpoint-worker");
                RequireNoLeftover(L"POWERPNT.EXE",{},"PowerPoint via app");
            }
            // 模拟“用户正开着 PowerPoint”：测试自己启动一个实例，转换必须拒绝且不能退出这个实例。
            CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            {
                CLSID clsid{};CLSIDFromProgID(L"PowerPoint.Application",&clsid);IDispatch* app=nullptr;
                Require(SUCCEEDED(CoCreateInstance(clsid,nullptr,CLSCTX_LOCAL_SERVER,IID_IDispatch,reinterpret_cast<void**>(&app))),"start test PowerPoint");
                const auto message=Message([&]{Converter c;c.Convert(source,{},{});});
                Require(message.find("PowerPoint is already running")!=std::string::npos,"refuses a running PowerPoint: "+message);
                VARIANT count=Get(app,L"Presentations");Require(count.vt==VT_DISPATCH,"running PowerPoint still alive (not quit by converter)");VariantClear(&count);
                Require(!Running(L"POWERPNT.EXE").empty(),"running PowerPoint process kept");
                Call(app,L"Quit");app->Release();
            }
            CoUninitialize();
            RequireNoLeftover(L"POWERPNT.EXE",{},"test PowerPoint");
        }

        // ---- 合并 ----
        if(merge.size()>=2){
            int pages=0;for(const auto& m:merge){Document d;d.Open(m.path);pages+=static_cast<int>(d.Info().pages.size());}
            Document::Merge(merge,out/L"merged.pdf",true,{});
            Document m;m.Open(out/L"merged.pdf");const auto info=m.Info();
            Require(static_cast<int>(info.pages.size())==pages,"merged page count");
            Require(info.outline.size()>=2,"merged outline has one entry per file");
        }else skipped.push_back("merge of Excel + PowerPoint output");
        std::cout<<"PASS office: "<<assertions<<" assertions; extensions, worker flags, messages, Excel (2 sheets, SUM recalculated), PowerPoint (3 slides 16:9), running-PowerPoint refusal, app worker, merge, no leftover processes.";
        for(const auto& s:skipped)std::cout<<" SKIP: "<<s<<";";
        std::cout<<"\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL office after "<<assertions<<" assertions: "<<e.what()<<"\n";return 1;}
}
