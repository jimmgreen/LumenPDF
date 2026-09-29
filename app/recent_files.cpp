#include "recent_files.h"
#include "core/platform.h"
#include <windows.h>
#include <algorithm>
#include <cwchar>
#include <ctime>
#include <fstream>
#include <sstream>
#include <cmath>
#include <cstdio>
namespace lpdf {
namespace {
std::tm Local(int64_t t){std::tm value{};const __time64_t v=t;_localtime64_s(&value,&v);return value;}
int64_t DayStart(int64_t t){auto tm=Local(t);tm.tm_hour=0;tm.tm_min=0;tm.tm_sec=0;tm.tm_isdst=-1;return static_cast<int64_t>(_mktime64(&tm));}
std::wstring Two(int v){wchar_t b[8];swprintf_s(b,L"%02d",v);return b;}
}
int64_t UnixNow(){return static_cast<int64_t>(_time64(nullptr));}
bool RecentFiles::SamePath(const fs::path& a,const fs::path& b){
    return _wcsicmp(a.lexically_normal().c_str(),b.lexically_normal().c_str())==0;
}
std::vector<RecentEntry> RecentFiles::Parse(std::string_view text){
    std::vector<RecentEntry> result;
    size_t start=0;
    while(start<text.size()){
        size_t end=text.find('\n',start);if(end==std::string_view::npos)end=text.size();
        std::string_view line=text.substr(start,end-start);start=end+1;
        if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);
        if(line.empty()||line=="lumen-recent 2")continue;
        RecentEntry entry;
        // v2："<opened>\t<pinned>\t<utf8 path>"；v1：整行即路径（旧版本写入的格式）。
        const size_t a=line.find('\t'),b=a==std::string_view::npos?a:line.find('\t',a+1);
        try{
            if(b!=std::string_view::npos){
                entry.opened=std::stoll(std::string(line.substr(0,a)));
                entry.pinned=line.substr(a+1,b-a-1)=="1";
                entry.path=Wide(line.substr(b+1));
            }else entry.path=Wide(line);
        }catch(...){continue;}
        if(entry.path.empty()||!entry.path.is_absolute())continue;
        if(std::any_of(result.begin(),result.end(),[&](const RecentEntry& e){return SamePath(e.path,entry.path);}))continue;
        result.push_back(std::move(entry));
    }
    return result;
}
std::string RecentFiles::Serialize(const std::vector<RecentEntry>& entries){
    std::string out="lumen-recent 2\n";
    for(const auto& e:entries)out+=std::to_string(e.opened)+"\t"+(e.pinned?"1":"0")+"\t"+Utf8(e.path.wstring())+"\n";
    return out;
}
void RecentFiles::Sort(){
    std::stable_sort(entries_.begin(),entries_.end(),[](const RecentEntry& a,const RecentEntry& b){
        if(a.pinned!=b.pinned)return a.pinned;return a.opened>b.opened;});
    // 超出上限时优先丢弃最旧的未固定项。
    while(entries_.size()>kLimit){
        auto victim=std::find_if(entries_.rbegin(),entries_.rend(),[](const RecentEntry& e){return !e.pinned;});
        if(victim==entries_.rend())break;
        entries_.erase(std::next(victim).base());
    }
}
void RecentFiles::Load(const fs::path& store){
    store_=store;entries_.clear();
    std::ifstream in(store,std::ios::binary);if(!in)return;
    std::stringstream buffer;buffer<<in.rdbuf();
    entries_=Parse(buffer.str());Sort();
}
bool RecentFiles::Save()const{
    if(readOnly_||store_.empty())return false;
    std::error_code error;fs::create_directories(store_.parent_path(),error);
    auto temp=store_;temp+=L".tmp";
    {
        std::ofstream out(temp,std::ios::binary|std::ios::trunc);if(!out)return false;
        const auto text=Serialize(entries_);out.write(text.data(),static_cast<std::streamsize>(text.size()));
        if(!out.good()){out.close();fs::remove(temp,error);return false;}
    }
    if(!MoveFileExW(temp.c_str(),store_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){fs::remove(temp,error);return false;}
    return true;
}
void RecentFiles::Touch(const fs::path& path,int64_t now){
    if(path.empty())return;
    std::error_code error;auto absolute=fs::absolute(path,error);if(error)absolute=path;
    bool pinned=false;
    entries_.erase(std::remove_if(entries_.begin(),entries_.end(),[&](const RecentEntry& e){if(SamePath(e.path,absolute)){pinned=e.pinned;return true;}return false;}),entries_.end());
    entries_.push_back({absolute.lexically_normal(),now,pinned});Sort();
}
void RecentFiles::Remove(const fs::path& path){
    entries_.erase(std::remove_if(entries_.begin(),entries_.end(),[&](const RecentEntry& e){return SamePath(e.path,path);}),entries_.end());
}
void RecentFiles::Pin(const fs::path& path,bool pinned){
    for(auto& e:entries_)if(SamePath(e.path,path))e.pinned=pinned;
    Sort();
}
bool RecentFiles::Pinned(const fs::path& path)const{
    return std::any_of(entries_.begin(),entries_.end(),[&](const RecentEntry& e){return e.pinned&&SamePath(e.path,path);});
}
void RecentFiles::Clear(bool keepPinned){
    if(!keepPinned){entries_.clear();return;}
    entries_.erase(std::remove_if(entries_.begin(),entries_.end(),[](const RecentEntry& e){return !e.pinned;}),entries_.end());
}
RecentGroup GroupOf(const RecentEntry& entry,int64_t now){
    if(entry.pinned)return RecentGroup::Pinned;
    const int64_t today=DayStart(now);
    if(entry.opened>=today)return RecentGroup::Today;
    if(entry.opened>=today-86400)return RecentGroup::Yesterday;
    if(entry.opened>=today-6*86400)return RecentGroup::Week;
    return RecentGroup::Earlier;
}
std::wstring_view GroupTitle(RecentGroup group){
    switch(group){
        case RecentGroup::Pinned:return L"已固定";case RecentGroup::Today:return L"今天";
        case RecentGroup::Yesterday:return L"昨天";case RecentGroup::Week:return L"近 7 天";default:return L"更早";
    }
}
std::wstring RelativeTime(int64_t then,int64_t now){
    if(then<=0)return L"较早打开";
    const int64_t delta=now-then;
    if(delta>=0&&delta<60)return L"刚刚";
    if(delta>=0&&delta<3600)return std::to_wstring(delta/60)+L" 分钟前";
    const auto tm=Local(then);const auto clock=Two(tm.tm_hour)+L":"+Two(tm.tm_min);
    const int64_t today=DayStart(now);
    if(then>=today)return L"今天 "+clock;
    if(then>=today-86400)return L"昨天 "+clock;
    if(then>=today-6*86400){static const wchar_t* days[]={L"周日",L"周一",L"周二",L"周三",L"周四",L"周五",L"周六"};return std::wstring(days[tm.tm_wday])+L" "+clock;}
    return std::to_wstring(tm.tm_year+1900)+L"-"+Two(tm.tm_mon+1)+L"-"+Two(tm.tm_mday);
}
std::vector<ReadingPosition> ReadingPositions::Parse(std::string_view text){
    std::vector<ReadingPosition> result;size_t start=0;
    while(start<text.size()){
        size_t end=text.find('\n',start);if(end==std::string_view::npos)end=text.size();
        std::string_view line=text.substr(start,end-start);start=end+1;
        if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);
        if(line.empty()||line=="lumen-positions 1")continue;
        // "<saved>\t<page>\t<offset>\t<fit>\t<zoom>\t<utf8 path>"
        std::vector<std::string_view> fields;size_t from=0;
        for(int i=0;i<5;++i){const size_t tab=line.find('\t',from);if(tab==std::string_view::npos)break;fields.push_back(line.substr(from,tab-from));from=tab+1;}
        if(fields.size()!=5)continue;
        ReadingPosition p;
        try{
            p.saved=std::stoll(std::string(fields[0]));p.page=std::stoi(std::string(fields[1]));
            p.offset=std::stof(std::string(fields[2]));p.fit=std::stoi(std::string(fields[3]));p.zoom=std::stof(std::string(fields[4]));
            p.path=Wide(line.substr(from));
        }catch(...){continue;}
        if(p.path.empty()||!p.path.is_absolute()||p.page<0||p.page>1000000||p.fit<0||p.fit>2)continue;
        if(!std::isfinite(p.offset)||p.offset<0)p.offset=0;
        if(!std::isfinite(p.zoom)||p.zoom<.1f||p.zoom>8)p.zoom=1;
        if(std::any_of(result.begin(),result.end(),[&](const ReadingPosition& e){return RecentFiles::SamePath(e.path,p.path);}))continue;
        result.push_back(std::move(p));
    }
    return result;
}
std::string ReadingPositions::Serialize(const std::vector<ReadingPosition>& items){
    std::string out="lumen-positions 1\n";char number[64];
    for(const auto& p:items){
        out+=std::to_string(p.saved)+"\t"+std::to_string(p.page)+"\t";
        snprintf(number,sizeof(number),"%.2f",p.offset);out+=number;out+="\t"+std::to_string(p.fit)+"\t";
        snprintf(number,sizeof(number),"%.4f",p.zoom);out+=number;out+="\t"+Utf8(p.path.wstring())+"\n";
    }
    return out;
}
void ReadingPositions::Load(const fs::path& store){
    store_=store;items_.clear();dirty_=false;
    std::ifstream in(store,std::ios::binary);if(!in)return;
    std::stringstream buffer;buffer<<in.rdbuf();items_=Parse(buffer.str());
    if(items_.size()>kLimit)items_.resize(kLimit);
}
bool ReadingPositions::Save(){
    if(readOnly_||store_.empty()||!dirty_)return false;
    std::error_code error;fs::create_directories(store_.parent_path(),error);
    {
        // 其它窗口可能已写入新位置：以磁盘为底，只覆盖本进程改动过的条目。
        std::ifstream in(store_,std::ios::binary);
        if(in){
            std::stringstream buffer;buffer<<in.rdbuf();auto disk=Parse(buffer.str());
            auto mine=[&](const fs::path& p){return std::any_of(touched_.begin(),touched_.end(),[&](const fs::path& t){return RecentFiles::SamePath(t,p);});};
            std::vector<ReadingPosition> merged;
            for(auto& item:items_)if(mine(item.path))merged.push_back(item);
            for(auto& item:disk)if(!mine(item.path))merged.push_back(std::move(item));
            std::stable_sort(merged.begin(),merged.end(),[](const ReadingPosition& a,const ReadingPosition& b){return a.saved>b.saved;});
            if(merged.size()>kLimit)merged.resize(kLimit);
            items_=std::move(merged);
        }
    }
    auto temp=store_;temp+=L".tmp";
    {
        std::ofstream out(temp,std::ios::binary|std::ios::trunc);if(!out)return false;
        const auto text=Serialize(items_);out.write(text.data(),static_cast<std::streamsize>(text.size()));
        if(!out.good()){out.close();fs::remove(temp,error);return false;}
    }
    if(!MoveFileExW(temp.c_str(),store_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){fs::remove(temp,error);return false;}
    dirty_=false;return true;
}
const ReadingPosition* ReadingPositions::Find(const fs::path& path)const{
    for(const auto& p:items_)if(RecentFiles::SamePath(p.path,path))return &p;
    return nullptr;
}
bool ReadingPositions::Remember(ReadingPosition position){
    if(position.path.empty())return false;
    std::error_code error;auto absolute=fs::absolute(position.path,error);if(!error)position.path=absolute.lexically_normal();
    auto it=std::find_if(items_.begin(),items_.end(),[&](const ReadingPosition& e){return RecentFiles::SamePath(e.path,position.path);});
    if(it!=items_.end()){
        const bool same=it->page==position.page&&std::abs(it->offset-position.offset)<.5f&&it->fit==position.fit&&std::abs(it->zoom-position.zoom)<.001f;
        if(same&&it==items_.begin())return false;
        items_.erase(it);
    }
    if(std::none_of(touched_.begin(),touched_.end(),[&](const fs::path& t){return RecentFiles::SamePath(t,position.path);}))touched_.push_back(position.path);
    items_.insert(items_.begin(),std::move(position));
    if(items_.size()>kLimit)items_.resize(kLimit);
    dirty_=true;return true;
}
void ReadingPositions::Forget(const fs::path& path){
    const auto before=items_.size();
    items_.erase(std::remove_if(items_.begin(),items_.end(),[&](const ReadingPosition& e){return RecentFiles::SamePath(e.path,path);}),items_.end());
    if(items_.size()!=before)dirty_=true;
}
}
