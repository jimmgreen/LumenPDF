#include <lumen/Main.h>
#include "application.h"
#include "shell_integration.h"
#include "update_install.h"
#include "core/platform.h"
#include <windows.h>
int lumen_main(std::span<const std::wstring_view> args) {
    if(args.size()==4){const int worker=lpdf::OfficeWorkerMain(args[1],args[2],args[3]);if(worker>=0)return worker;}   // Word / Excel / PowerPoint 转换子进程
    try{
        // 安装/卸载程序调用：只改注册表，不显示窗口。
        if(args.size()>=2&&args[1]==L"--register-shell"){
            const auto exe=lpdf::ExecutablePath();
            const bool merge=args.size()<3||args[2]!=L"--no-merge-menu";
            auto error=lpdf::RegisterPdfHandler(exe,false);
            if(error.empty()&&merge)error=lpdf::RegisterMergeMenu(exe);
            return error.empty()?0:2;
        }
        if(args.size()>=2&&args[1]==L"--unregister-shell"){lpdf::UnregisterShell();return 0;}
        // 便携版在线升级助手：由暂存目录中的新版本运行，等旧进程退出后替换文件（失败则回滚）。
        if(args.size()==5&&args[1]==L"--apply-update")return lpdf::update::ApplyUpdateMain(lpdf::fs::path(args[2]),lpdf::fs::path(args[3]),std::wcstoul(std::wstring(args[4]).c_str(),nullptr,10));
        if(args.size()>=2&&args[1]==L"--register-merge-menu")return lpdf::RegisterMergeMenu(lpdf::ExecutablePath()).empty()?0:2;
        const bool smoke=args.size()>1&&args[1]==L"--smoke";
        const bool separate=args.size()>1&&args[1]==L"--new-window";
        // 右键“使用 LumenPDF 合并”：每个选中的文件各启动一次，全部汇总到第一个窗口的合并列表。
        const bool merge=args.size()>2&&args[1]==L"--merge";
        const auto index=(smoke||separate||merge)?2u:1u;
        const lpdf::fs::path initial=args.size()>index?lpdf::fs::path(args[index]):lpdf::fs::path{};
        // 再次双击 PDF 时交给已打开的窗口；测试与 --new-window 始终独立运行。
        std::unique_ptr<lpdf::SingleInstance> instance;
        if(!smoke&&!separate){
            instance=std::make_unique<lpdf::SingleInstance>();
            if(instance->Secondary()){
                if(merge?instance->ForwardMerge(initial):instance->Forward(initial))return 0;
                // 主实例无响应（仍在启动或已挂起）：合并请求不能丢，退为独立窗口处理。
            }
        }
        lpdf::Application application;
        if(instance&&!instance->Secondary())instance->Listen([&application](lpdf::fs::path path,bool toMerge){
            if(toMerge)application.AcceptMerge(path);else application.AcceptExternal(path);
        });
        if(merge)return application.Run({},smoke,initial);
        return application.Run(initial,smoke);
    }catch(const std::exception& e){MessageBoxW(nullptr,lpdf::Wide(e.what()).c_str(),L"LumenPDF",MB_ICONERROR);return 1;}
}