#include "conversion.h"
#include <windows.h>
#include <oleauto.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <fstream>
#include <set>
#include <algorithm>
#include <cwctype>
#include <utility>
namespace lpdf {
namespace {
void CheckHr(HRESULT hr,const char* message){if(FAILED(hr))throw std::runtime_error(std::string(message)+" (HRESULT "+std::to_string(static_cast<unsigned>(hr))+")");}
struct ComScope {
    HRESULT hr{CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)};
    ComScope(){CheckHr(hr,"Cannot initialize COM");}
    ~ComScope(){CoUninitialize();}
};
struct Variant {
    VARIANT v;
    Variant(){VariantInit(&v);}
    explicit Variant(const std::wstring& s):Variant(){v.vt=VT_BSTR;v.bstrVal=SysAllocStringLen(s.data(),static_cast<UINT>(s.size()));if(!v.bstrVal)throw std::bad_alloc();}
    explicit Variant(long n):Variant(){v.vt=VT_I4;v.lVal=n;}
    explicit Variant(bool b):Variant(){v.vt=VT_BOOL;v.boolVal=b?VARIANT_TRUE:VARIANT_FALSE;}
    Variant(Variant&& o)noexcept:v(o.v){VariantInit(&o.v);}
    Variant& operator=(Variant&& o)noexcept{if(this!=&o){VariantClear(&v);v=o.v;VariantInit(&o.v);}return *this;}
    Variant(const Variant&)=delete;
    ~Variant(){VariantClear(&v);}
    IDispatch* Dispatch()const{if(v.vt!=VT_DISPATCH||!v.pdispVal)throw std::runtime_error("Office returned no document");return v.pdispVal;}
};
Variant Invoke(IDispatch* object,const wchar_t* method,WORD flags,std::vector<std::pair<std::wstring,Variant>> values={}){
    std::vector<LPOLESTR> names{const_cast<LPOLESTR>(method)};
    for(auto& pair:values)names.push_back(pair.first.data());
    std::vector<DISPID> ids(names.size());
    CheckHr(object->GetIDsOfNames(IID_NULL,names.data(),static_cast<UINT>(names.size()),LOCALE_USER_DEFAULT,ids.data()),("Office method unavailable: "+Utf8(method)).c_str());
    std::vector<VARIANT> args;std::vector<DISPID> named;
    for(size_t i=0;i<values.size();++i){args.push_back(values[i].second.v);named.push_back(ids[i+1]);}
    DISPID put=DISPID_PROPERTYPUT;
    DISPPARAMS params{};
    params.rgvarg=args.data();params.cArgs=static_cast<UINT>(args.size());
    params.rgdispidNamedArgs=(flags&DISPATCH_PROPERTYPUT)?&put:named.data();
    params.cNamedArgs=(flags&DISPATCH_PROPERTYPUT)?1:static_cast<UINT>(named.size());
    Variant result;EXCEPINFO exception{};UINT error=0;
    HRESULT hr=object->Invoke(ids[0],IID_NULL,LOCALE_USER_DEFAULT,flags,&params,&result.v,&exception,&error);
    std::string detail=exception.bstrDescription?Utf8(exception.bstrDescription):"Office operation failed";
    SysFreeString(exception.bstrSource);SysFreeString(exception.bstrDescription);SysFreeString(exception.bstrHelpFile);
    CheckHr(hr,detail.c_str());return result;
}
// 位置参数调用（PowerPoint 不支持具名参数）。args 按声明顺序给出。
Variant InvokeArgs(IDispatch* object,const wchar_t* method,WORD flags,std::vector<Variant> values={}){
    DISPID id;auto* name=const_cast<LPOLESTR>(method);
    CheckHr(object->GetIDsOfNames(IID_NULL,&name,1,LOCALE_USER_DEFAULT,&id),("Office method unavailable: "+Utf8(method)).c_str());
    std::vector<VARIANT> args;for(auto it=values.rbegin();it!=values.rend();++it)args.push_back(it->v);   // IDispatch 要求逆序
    DISPPARAMS params{};params.rgvarg=args.data();params.cArgs=static_cast<UINT>(args.size());
    Variant result;EXCEPINFO exception{};UINT error=0;
    HRESULT hr=object->Invoke(id,IID_NULL,LOCALE_USER_DEFAULT,flags,&params,&result.v,&exception,&error);
    std::string detail=exception.bstrDescription?Utf8(exception.bstrDescription):"Office operation failed";
    SysFreeString(exception.bstrSource);SysFreeString(exception.bstrDescription);SysFreeString(exception.bstrHelpFile);
    CheckHr(hr,detail.c_str());return result;
}
void Put(IDispatch* obj,const wchar_t* property,Variant value){
    DISPID id;auto* name=const_cast<LPOLESTR>(property);CheckHr(obj->GetIDsOfNames(IID_NULL,&name,1,LOCALE_USER_DEFAULT,&id),"Office property unavailable");
    DISPID put=DISPID_PROPERTYPUT;DISPPARAMS params{&value.v,&put,1,1};
    CheckHr(obj->Invoke(id,IID_NULL,LOCALE_USER_DEFAULT,DISPATCH_PROPERTYPUT,&params,nullptr,nullptr,nullptr),"Cannot set Office property");
}
std::set<DWORD> OfficeProcesses(const wchar_t* exe){
    std::set<DWORD> result;HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot inspect existing Office instances");
    PROCESSENTRY32W entry{sizeof(entry)};
    if(Process32FirstW(snapshot,&entry))do{if(_wcsicmp(entry.szExeFile,exe)==0)result.insert(entry.th32ProcessID);}while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);return result;
}
std::set<DWORD> WordProcesses(){return OfficeProcesses(L"WINWORD.EXE");}
void RecordOwnedWord(DWORD pid,const fs::path& file){
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process)throw std::runtime_error("Cannot identify the conversion Word process");
    FILETIME creation{},exit{},kernel{},user{};
    const bool ok=GetProcessTimes(process,&creation,&exit,&kernel,&user)!=0;CloseHandle(process);
    if(!ok)throw std::runtime_error("Cannot identify Word process creation time");
    ULARGE_INTEGER time{};time.LowPart=creation.dwLowDateTime;time.HighPart=creation.dwHighDateTime;
    std::ofstream out(file);out<<pid<<" "<<time.QuadPart;out.close();if(!out)throw std::runtime_error("Cannot write converter ownership record");
}
std::wstring Lower(std::wstring s){std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});return s;}
}
InputFileKind FileKind(const fs::path& path){
    const auto extension=Lower(path.extension().wstring());
    if(extension==L".pdf")return InputFileKind::Pdf;
    for(const wchar_t* e:{L".doc",L".docx",L".docm",L".rtf",L".odt"})if(extension==e)return InputFileKind::Word;
    for(const wchar_t* e:{L".xls",L".xlsx",L".xlsm",L".xlsb",L".ods"})if(extension==e)return InputFileKind::Excel;
    for(const wchar_t* e:{L".ppt",L".pptx",L".pptm",L".pps",L".ppsx",L".odp"})if(extension==e)return InputFileKind::PowerPoint;
    if(extension==L".txt")return InputFileKind::Text;
    if(extension==L".png"||extension==L".jpg"||extension==L".jpeg")return InputFileKind::Image;
    return InputFileKind::Other;
}
int WordWorker(const fs::path& source,const fs::path& output){
    try{
        ComScope com;const auto existing=WordProcesses();CLSID clsid{};
        CheckHr(CLSIDFromProgID(L"Word.Application",&clsid),"Microsoft Word is not installed");
        Variant app;app.v.vt=VT_DISPATCH;
        CheckHr(CoCreateInstance(clsid,nullptr,CLSCTX_LOCAL_SERVER,IID_IDispatch,reinterpret_cast<void**>(&app.v.pdispVal)),"Cannot start Microsoft Word");
        auto initialDocuments=Invoke(app.Dispatch(),L"Documents",DISPATCH_PROPERTYGET); auto blank=Invoke(initialDocuments.Dispatch(),L"Add",DISPATCH_METHOD); auto blankWindow=Invoke(blank.Dispatch(),L"ActiveWindow",DISPATCH_PROPERTYGET); auto hwnd=Invoke(blankWindow.Dispatch(),L"Hwnd",DISPATCH_PROPERTYGET);DWORD pid=0;
        GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<intptr_t>(hwnd.v.lVal)),&pid);
        const bool owned=pid && !existing.contains(pid);
        // Never change or quit an existing interactive Word application.
        if(!owned)throw std::runtime_error("Word returned an existing instance; close Word or choose LibreOffice");
        struct Quit {
            IDispatch* app;
            ~Quit(){try{Invoke(app,L"Quit",DISPATCH_METHOD);}catch(...){}}
        } quit{app.Dispatch()};
        RecordOwnedWord(pid,output.wstring()+L".owner"); {std::vector<std::pair<std::wstring,Variant>> closeBlank;closeBlank.emplace_back(L"SaveChanges",Variant(0L));Invoke(blank.Dispatch(),L"Close",DISPATCH_METHOD,std::move(closeBlank));}
        Put(app.Dispatch(),L"Visible",Variant(false));
        Put(app.Dispatch(),L"DisplayAlerts",Variant(0L));
        Put(app.Dispatch(),L"AutomationSecurity",Variant(3L));
        auto options=Invoke(app.Dispatch(),L"Options",DISPATCH_PROPERTYGET);
        Put(options.Dispatch(),L"UpdateLinksAtOpen",Variant(false));
        auto documents=Invoke(app.Dispatch(),L"Documents",DISPATCH_PROPERTYGET);
        std::vector<std::pair<std::wstring,Variant>> open;
        open.emplace_back(L"FileName",Variant(fs::absolute(source).wstring()));
        open.emplace_back(L"ReadOnly",Variant(true));open.emplace_back(L"AddToRecentFiles",Variant(false));
        open.emplace_back(L"ConfirmConversions",Variant(false));open.emplace_back(L"Visible",Variant(false));
        open.emplace_back(L"NoEncodingDialog",Variant(true));
        auto document=Invoke(documents.Dispatch(),L"Open",DISPATCH_METHOD,std::move(open));
        struct Close {IDispatch* doc;~Close(){try{std::vector<std::pair<std::wstring,Variant>> args;args.emplace_back(L"SaveChanges",Variant(0L));Invoke(doc,L"Close",DISPATCH_METHOD,std::move(args));}catch(...){}}} close{document.Dispatch()};
        std::vector<std::pair<std::wstring,Variant>> save;
        save.emplace_back(L"OutputFileName",Variant(fs::absolute(output).wstring()));
        save.emplace_back(L"ExportFormat",Variant(17L));save.emplace_back(L"OpenAfterExport",Variant(false));
        save.emplace_back(L"OptimizeFor",Variant(0L));save.emplace_back(L"CreateBookmarks",Variant(1L));
        Invoke(document.Dispatch(),L"ExportAsFixedFormat",DISPATCH_METHOD,std::move(save));
        return 0;
    }catch(const std::exception& e){std::ofstream error(output.wstring()+L".error",std::ios::binary);error<<e.what();return 1;}
}
// Excel / PowerPoint：在隐藏的、由本进程新启动的实例中只读打开并导出 PDF，结束后退出该实例。
// 与 Word 相同的原则：绝不接管、修改或退出用户已经打开的 Office（PowerPoint 是单实例程序，正在运行时直接拒绝）。
int OfficeWorker(InputFileKind kind,const fs::path& source,const fs::path& output){
    if(kind==InputFileKind::Word)return WordWorker(source,output);
    const bool excel=kind==InputFileKind::Excel;
    try{
        if(!excel&&kind!=InputFileKind::PowerPoint)throw std::runtime_error("Not an Office document");
        const wchar_t* exe=excel?L"EXCEL.EXE":L"POWERPNT.EXE";
        ComScope com;const auto existing=OfficeProcesses(exe);CLSID clsid{};
        CheckHr(CLSIDFromProgID(excel?L"Excel.Application":L"PowerPoint.Application",&clsid),excel?"Microsoft Excel is not installed":"Microsoft PowerPoint is not installed");
        Variant app;app.v.vt=VT_DISPATCH;
        CheckHr(CoCreateInstance(clsid,nullptr,CLSCTX_LOCAL_SERVER,IID_IDispatch,reinterpret_cast<void**>(&app.v.pdispVal)),excel?"Cannot start Microsoft Excel":"Cannot start Microsoft PowerPoint");
        DWORD pid=0;
        try{auto hwnd=Invoke(app.Dispatch(),L"HWND",DISPATCH_PROPERTYGET);if(hwnd.v.vt==VT_I4)GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<intptr_t>(hwnd.v.lVal)),&pid);}catch(...){}
        if(!pid){   // 取不到窗口时：新出现的唯一进程
            std::vector<DWORD> fresh;for(DWORD id:OfficeProcesses(exe))if(!existing.contains(id))fresh.push_back(id);
            if(fresh.size()==1)pid=fresh[0];
        }
        if(!pid||existing.contains(pid))
            throw std::runtime_error(excel?"Excel returned an existing instance; close Excel or choose LibreOffice":"PowerPoint is already running; close PowerPoint or choose LibreOffice");
        struct Quit {IDispatch* app;~Quit(){try{Invoke(app,L"Quit",DISPATCH_METHOD);}catch(...){}}} quit{app.Dispatch()};
        RecordOwnedWord(pid,output.wstring()+L".owner");
        const auto input=fs::absolute(source).wstring(),target=fs::absolute(output).wstring();
        if(excel){
            Put(app.Dispatch(),L"Visible",Variant(false));
            Put(app.Dispatch(),L"DisplayAlerts",Variant(false));
            Put(app.Dispatch(),L"AutomationSecurity",Variant(3L));   // msoAutomationSecurityForceDisable：不运行宏
            Put(app.Dispatch(),L"AskToUpdateLinks",Variant(false));
            Put(app.Dispatch(),L"EnableEvents",Variant(false));
            auto books=Invoke(app.Dispatch(),L"Workbooks",DISPATCH_PROPERTYGET);
            std::vector<std::pair<std::wstring,Variant>> open;
            open.emplace_back(L"Filename",Variant(input));open.emplace_back(L"UpdateLinks",Variant(0L));
            open.emplace_back(L"ReadOnly",Variant(true));open.emplace_back(L"Password",Variant(std::wstring(L"\x1f")));   // 有打开密码时报错而不是弹出隐藏的密码框
            open.emplace_back(L"IgnoreReadOnlyRecommended",Variant(true));open.emplace_back(L"Notify",Variant(false));
            open.emplace_back(L"AddToMru",Variant(false));
            auto book=Invoke(books.Dispatch(),L"Open",DISPATCH_METHOD,std::move(open));
            struct Close {IDispatch* book;~Close(){try{std::vector<std::pair<std::wstring,Variant>> a;a.emplace_back(L"SaveChanges",Variant(false));Invoke(book,L"Close",DISPATCH_METHOD,std::move(a));}catch(...){}}} close{book.Dispatch()};
            std::vector<std::pair<std::wstring,Variant>> save;   // 整个工作簿（所有工作表），按各表的打印设置分页
            save.emplace_back(L"Type",Variant(0L));save.emplace_back(L"Filename",Variant(target));
            save.emplace_back(L"Quality",Variant(0L));save.emplace_back(L"IncludeDocProperties",Variant(true));
            save.emplace_back(L"IgnorePrintAreas",Variant(false));save.emplace_back(L"OpenAfterPublish",Variant(false));
            Invoke(book.Dispatch(),L"ExportAsFixedFormat",DISPATCH_METHOD,std::move(save));
        }else{
            Put(app.Dispatch(),L"DisplayAlerts",Variant(1L));        // ppAlertsNone
            try{Put(app.Dispatch(),L"AutomationSecurity",Variant(3L));}catch(...){}
            auto presentations=Invoke(app.Dispatch(),L"Presentations",DISPATCH_PROPERTYGET);
            std::vector<Variant> open;open.emplace_back(input);open.emplace_back(-1L);open.emplace_back(0L);open.emplace_back(0L);   // ReadOnly、非无标题、无窗口
            auto deck=InvokeArgs(presentations.Dispatch(),L"Open",DISPATCH_METHOD,std::move(open));
            struct Close {IDispatch* deck;~Close(){try{InvokeArgs(deck,L"Close",DISPATCH_METHOD);}catch(...){}}} close{deck.Dispatch()};
            std::vector<Variant> save;save.emplace_back(target);save.emplace_back(32L);   // ppSaveAsPDF
            InvokeArgs(deck.Dispatch(),L"SaveAs",DISPATCH_METHOD,std::move(save));
        }
        return 0;
    }catch(const std::exception& e){std::ofstream error(output.wstring()+L".error",std::ios::binary);error<<e.what();return 1;}
}
const wchar_t* OfficeWorkerFlag(InputFileKind kind){
    return kind==InputFileKind::Excel?L"--excel-worker":kind==InputFileKind::PowerPoint?L"--powerpoint-worker":L"--word-worker";
}
int OfficeWorkerMain(std::wstring_view flag,const fs::path& source,const fs::path& output){
    for(auto kind:{InputFileKind::Word,InputFileKind::Excel,InputFileKind::PowerPoint})if(flag==OfficeWorkerFlag(kind))return OfficeWorker(kind,source,output);
    return -1;
}
bool Converter::WordAvailable(){
    CLSID clsid{};return SUCCEEDED(CLSIDFromProgID(L"Word.Application",&clsid));
}
bool Converter::OfficeAvailable(InputFileKind kind){
    const wchar_t* id=kind==InputFileKind::Word?L"Word.Application":kind==InputFileKind::Excel?L"Excel.Application":kind==InputFileKind::PowerPoint?L"PowerPoint.Application":nullptr;
    CLSID clsid{};return id&&SUCCEEDED(CLSIDFromProgID(id,&clsid));
}
fs::path Converter::FindLibreOffice(){
    wchar_t configured[32768]{};
    if(GetEnvironmentVariableW(L"LUMENPDF_LIBREOFFICE",configured,32768)&&fs::exists(configured))return configured;
    for(const wchar_t* root:{L"C:\\Program Files\\LibreOffice\\program\\soffice.com",L"C:\\Program Files (x86)\\LibreOffice\\program\\soffice.com"})
        if(fs::exists(root))return root;
    return {};
}
std::wstring Converter::DecodeText(const std::vector<unsigned char>& bytes,TextEncoding encoding){
    size_t offset=0;
    if(encoding==TextEncoding::Auto){
        if(bytes.size()>=3&&bytes[0]==0xef&&bytes[1]==0xbb&&bytes[2]==0xbf){encoding=TextEncoding::Utf8;offset=3;}
        else if(bytes.size()>=2&&bytes[0]==0xff&&bytes[1]==0xfe){encoding=TextEncoding::Utf16LE;offset=2;}
        else if(bytes.size()>=2&&bytes[0]==0xfe&&bytes[1]==0xff){encoding=TextEncoding::Utf16BE;offset=2;}
        else encoding=TextEncoding::Utf8;
    }
    std::wstring text;
    if(encoding==TextEncoding::Utf16LE||encoding==TextEncoding::Utf16BE){
        if((bytes.size()-offset)%2)throw std::runtime_error("Incomplete UTF-16 text");
        for(size_t i=offset;i+1<bytes.size();i+=2)text+=static_cast<wchar_t>(encoding==TextEncoding::Utf16LE?bytes[i]|(bytes[i+1]<<8):(bytes[i]<<8)|bytes[i+1]);
        (void)Utf8(text); // Reject unpaired surrogates.
    }else{
        if(bytes.empty())return {};
        const UINT codepage=encoding==TextEncoding::GB18030?54936:CP_UTF8;
        const char* data=reinterpret_cast<const char*>(bytes.data()+offset);const int size=static_cast<int>(bytes.size()-offset);
        if(!size)return {};
        const int n=MultiByteToWideChar(codepage,MB_ERR_INVALID_CHARS,data,size,nullptr,0);
        if(!n)throw std::runtime_error("Text encoding is not UTF-8; choose GB18030 or UTF-16 in conversion settings");
        text.resize(n);MultiByteToWideChar(codepage,MB_ERR_INVALID_CHARS,data,size,text.data(),n);
    }
    if(!text.empty()&&text.front()==0xfeff)text.erase(text.begin());
    if(text.find(L'\0')!=std::wstring::npos)throw std::runtime_error("Text contains NUL characters; check its encoding");
    return text;
}
ConversionResult Converter::Convert(const fs::path& input,const ConversionOptions& options,const Cancel& cancel,const ProgressSink& progress){
    CheckCancel(cancel);ReportProgress(progress,ProgressStage::Reading);CheckCancel(cancel);
    const auto kind=FileKind(input);
    if(kind==InputFileKind::Pdf){ReportProgress(progress,ProgressStage::ConversionReady,1,1);return {input,L"PDF"};}
    const auto directory=UniquePath(temporary_.path,L"");fs::create_directory(directory);
    // Preserve the source filename for the merged document's outline.
    const auto output=directory/(input.stem().wstring()+L".pdf");
    if(kind==InputFileKind::Text){
        const auto text=DecodeText(ReadBytes(input),options.encoding);
        Document::TextToPdf(text,output,cancel,progress);CheckCancel(cancel);
        ReportProgress(progress,ProgressStage::ConversionReady,1,1);return {output,L"本地文字排版"};
    }
    if(kind==InputFileKind::Image){
        Document::ImageToPdf(input,output,options.imageA4,cancel,progress);CheckCancel(cancel);
        ReportProgress(progress,ProgressStage::ConversionReady,1,1);return {output,L"本地图片"};
    }
    if(!IsOfficeKind(kind))throw std::runtime_error("Supported files: PDF, Word, Excel, PowerPoint, TXT, PNG, JPG");
    const bool useWord=options.office==OfficeBackend::Word||(options.office==OfficeBackend::Automatic&&OfficeAvailable(kind));
    const std::wstring officeName=kind==InputFileKind::Excel?L"Microsoft Excel":kind==InputFileKind::PowerPoint?L"Microsoft PowerPoint":L"Microsoft Word";
    if(progress){OperationProgress event;event.stage=ProgressStage::OfficeConversion;event.backend=useWord?officeName:L"LibreOffice";progress(event);}
    CheckCancel(cancel);
    if(useWord){
        const int code=RunProcess(ExecutablePath(),{OfficeWorkerFlag(kind),fs::absolute(input).wstring(),output.wstring()},cancel,kind==InputFileKind::Word?120:180,output.wstring()+L".owner");
        if(code){
            const auto error=output.wstring()+L".error";std::string message="Office conversion failed";
            if(fs::exists(error)){const auto data=ReadBytes(error);message.assign(data.begin(),data.end());}
            throw std::runtime_error(message);
        }
    }else{
        const auto office=options.libreOffice.empty()?FindLibreOffice():options.libreOffice;
        if(office.empty()||!fs::exists(office))throw std::runtime_error("Install Microsoft Office or LibreOffice, or select the Office backend in settings");
        const auto profile=directory/L"profile"/L"user"; fs::create_directories(profile); const std::string config=R"xml(<?xml version="1.0" encoding="UTF-8"?><oor:items xmlns:oor="http://openoffice.org/2001/registry"><item oor:path="/org.openoffice.Office.Common/Security/Scripting"><prop oor:name="MacroSecurityLevel" oor:op="fuse"><value>3</value></prop></item><item oor:path="/org.openoffice.Office.Writer/Content/Update"><prop oor:name="Link" oor:op="fuse"><value>2</value></prop></item></oor:items>)xml"; WriteBytes(profile/L"registrymodifications.xcu",std::vector<unsigned char>(config.begin(),config.end())); wchar_t profileUrl[32768]{};DWORD size=32768;
        CheckHr(UrlCreateFromPathW((directory/L"profile").c_str(),profileUrl,&size,0),"Cannot create LibreOffice profile URL");
        const int code=RunProcess(office,{L"-env:UserInstallation="+std::wstring(profileUrl),L"--headless",L"--convert-to",kind==InputFileKind::Excel?L"pdf:calc_pdf_Export":kind==InputFileKind::PowerPoint?L"pdf:impress_pdf_Export":L"pdf:writer_pdf_Export",L"--outdir",directory.wstring(),fs::absolute(input).wstring()},cancel);
        if(code)throw std::runtime_error("LibreOffice conversion failed");
    }
    CheckCancel(cancel);
    if(!fs::exists(output)||fs::file_size(output)==0)throw std::runtime_error("Office produced no PDF");
    ReportProgress(progress,ProgressStage::Validating);CheckCancel(cancel);
    Document verify;verify.Open(output);(void)verify.Info();CheckCancel(cancel);
    ReportProgress(progress,ProgressStage::ConversionReady,1,1);
    return {output,useWord?officeName:L"LibreOffice"};
}
}



