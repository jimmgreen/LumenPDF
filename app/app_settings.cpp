#include "app_settings.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <windows.h>
namespace lpdf {
AppSettings AppSettings::Parse(std::string_view text){
    AppSettings s;std::istringstream in{std::string(text)};std::string line;
    auto number=[](const std::string& v,int lo,int hi,int fallback){
        char* end=nullptr;const long n=std::strtol(v.c_str(),&end,10);
        if(v.empty()||end==v.c_str())return fallback;return static_cast<int>(std::clamp<long>(n,lo,hi));
    };
    while(std::getline(in,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        const auto eq=line.find('=');if(eq==std::string::npos)continue;
        const auto key=line.substr(0,eq),value=line.substr(eq+1);
        if(key=="tone")s.tone=number(value,0,2,s.tone);
        else if(key=="layout")s.layout=number(value,0,2,s.layout);
        else if(key=="fit")s.defaultFit=number(value,0,1,s.defaultFit);
        else if(key=="restore")s.restorePosition=number(value,0,1,1)!=0;
        else if(key=="external_new_window")s.externalNewWindow=number(value,0,1,0)!=0;
        else if(key=="logging")s.logging=number(value,0,1,1)!=0;
        else if(key=="asked_default")s.askedDefault=number(value,0,1,0)!=0;
        else if(key=="export_dpi")s.exportDpi=number(value,72,600,s.exportDpi);
        else if(key=="sidebar_width")s.sidebarWidth=number(value,180,600,s.sidebarWidth);
        else if(key=="sidebar_tab")s.sidebarTab=number(value,0,3,s.sidebarTab);
        else if(key=="paged")s.paged=number(value,0,1,0)!=0;
        else if(key=="auto_reload")s.autoReload=number(value,0,1,0)!=0;
        else if(key=="speech_rate")s.speechRate=number(value,-10,10,0);
        else if(key=="speech_voice")s.speechVoice=value;
        else if(key=="auto_update")s.autoUpdate=number(value,0,1,1)!=0;
        else if(key=="last_update_check"){char* end=nullptr;const long long t=std::strtoll(value.c_str(),&end,10);if(end!=value.c_str()&&t>=0)s.lastUpdateCheck=t;}
        else if(key=="skip_version"){if(value.size()<=40)s.skipVersion=value;}
    }
    return s;
}
std::string AppSettings::Serialize()const{
    std::ostringstream out;
    out<<"lumen-settings 1\n"<<"tone="<<tone<<"\nlayout="<<layout<<"\nfit="<<defaultFit<<"\nrestore="<<(restorePosition?1:0)
       <<"\nexternal_new_window="<<(externalNewWindow?1:0)<<"\nlogging="<<(logging?1:0)<<"\nexport_dpi="<<exportDpi<<"\nasked_default="<<(askedDefault?1:0)
       <<"\nsidebar_width="<<sidebarWidth<<"\nsidebar_tab="<<sidebarTab<<"\npaged="<<(paged?1:0)<<"\nauto_reload="<<(autoReload?1:0)<<"\nspeech_rate="<<speechRate<<"\nspeech_voice="<<speechVoice<<"\nauto_update="<<(autoUpdate?1:0)<<"\nlast_update_check="<<lastUpdateCheck<<"\nskip_version="<<skipVersion<<"\n";
    return out.str();
}
AppSettings LoadSettingsFile(const std::filesystem::path& file){
    std::ifstream in(file,std::ios::binary);if(!in)return {};
    std::ostringstream text;text<<in.rdbuf();return AppSettings::Parse(text.str());
}
bool SaveSettingsFile(const std::filesystem::path& file,const AppSettings& settings){
    if(file.empty())return false;
    std::error_code error;std::filesystem::create_directories(file.parent_path(),error);
    auto temp=file;temp+=L".tmp";
    {std::ofstream out(temp,std::ios::binary|std::ios::trunc);if(!out)return false;out<<settings.Serialize();out.flush();if(!out.good()){out.close();std::filesystem::remove(temp,error);return false;}}
    if(!MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){std::filesystem::remove(temp,error);return false;}
    return true;
}
}
