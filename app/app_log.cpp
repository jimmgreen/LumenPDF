#include "app_log.h"
#include "core/platform.h"
#include <windows.h>
#include <cstdio>
#include <mutex>
namespace lpdf::log {
namespace {
std::mutex lock;
std::filesystem::path folder,file;
constexpr uintmax_t kLimit=1024*1024;
// “模块名+偏移”（Release 无 PDB 时，配合链接生成的 LumenPDF.map 定位函数）。
void AppendAddress(std::wstring& out,DWORD64 address){
    HMODULE module=nullptr;wchar_t name[MAX_PATH]{};
    if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(address),&module)&&module&&GetModuleFileNameW(module,name,MAX_PATH)){
        const wchar_t* base=wcsrchr(name,L'\\');wchar_t offset[32];swprintf_s(offset,L"+0x%llx",static_cast<unsigned long long>(address-reinterpret_cast<DWORD64>(module)));
        out+=base?base+1:name;out+=offset;
    }else{wchar_t raw[32];swprintf_s(raw,L"0x%llx",static_cast<unsigned long long>(address));out+=raw;}
}
LONG WINAPI Crash(EXCEPTION_POINTERS* info){
    const auto* record=info?info->ExceptionRecord:nullptr;
    wchar_t head[160];swprintf_s(head,L"未处理的异常 0x%08lX，进程即将退出",record?record->ExceptionCode:0ul);
    std::wstring text=head;
    if(record&&record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION&&record->NumberParameters>=2){
        wchar_t av[96];swprintf_s(av,L"（%ls 地址 0x%llx）",record->ExceptionInformation[0]==1?L"写入":record->ExceptionInformation[0]==8?L"执行":L"读取",static_cast<unsigned long long>(record->ExceptionInformation[1]));text+=av;
    }
    // x64 展开数据回溯调用栈（不依赖 PDB）；栈已损坏时尽力而为。
    if(info&&info->ContextRecord){
        text+=L" 栈：";CONTEXT context=*info->ContextRecord;
        for(int i=0;i<24&&context.Rip;++i){
            if(i)text+=L" <- ";
            AppendAddress(text,context.Rip);
            DWORD64 base=0;auto* function=RtlLookupFunctionEntry(context.Rip,&base,nullptr);
            if(!function){context.Rip=*reinterpret_cast<DWORD64*>(context.Rsp);context.Rsp+=8;continue;}
            void* handler=nullptr;DWORD64 frame=0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,context.Rip,function,&context,&handler,&frame,nullptr);
        }
    }
    Write("FATAL",text);
    // 冒烟测试禁用了日志：LPDF_CRASH_LOG 指定文件时另写一份（诊断间歇性崩溃）。
    wchar_t crashFile[MAX_PATH]{};
    if(GetEnvironmentVariableW(L"LPDF_CRASH_LOG",crashFile,MAX_PATH)){
        FILE* out=nullptr;
        if(_wfopen_s(&out,crashFile,L"ab")==0&&out){const auto line=Utf8(text)+"\r\n";fwrite(line.data(),1,line.size(),out);fclose(out);}
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
}
void Init(const std::filesystem::path& where){
    std::lock_guard guard(lock);folder=where;file.clear();
    if(where.empty())return;
    std::error_code error;std::filesystem::create_directories(where,error);
    file=where/L"lumenpdf.log";
}
std::filesystem::path Folder(){std::lock_guard guard(lock);return folder;}
void Write(std::string_view level,std::wstring_view message){
    std::lock_guard guard(lock);
    if(file.empty())return;
    std::error_code error;
    if(std::filesystem::exists(file,error)&&std::filesystem::file_size(file,error)>kLimit){
        auto old=file;old+=L".1";std::filesystem::remove(old,error);std::filesystem::rename(file,old,error);
    }
    SYSTEMTIME t;GetLocalTime(&t);
    char stamp[64];sprintf_s(stamp,"%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentThreadId());
    std::wstring clean(message);for(auto& c:clean)if(c==L'\r'||c==L'\n')c=L' ';
    const std::string line=std::string(stamp)+std::string(level)+" "+Utf8(clean)+"\r\n";
    FILE* out=nullptr;
    if(_wfopen_s(&out,file.c_str(),L"ab")!=0||!out)return;
    fwrite(line.data(),1,line.size(),out);fclose(out);
}
void InstallCrashHandler(){SetUnhandledExceptionFilter(Crash);}
}
