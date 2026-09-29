// 最近打开：持久化格式、旧格式迁移、去重/固定/上限、分组与相对时间，以及主页视图的交互。
#include "app/recent_files.h"
#include "app/recent_view.h"
#include "app/print_job.h"
#include "app/app_settings.h"
#include "app/app_log.h"
#include <fstream>
#include <cmath>
#include <lumen/App.h>
#include <windows.h>
#include <ctime>
#include <iostream>
#include <stdexcept>
namespace lpdf {
static int recentAssertions=0;
static void Check(bool value,const char* what){++recentAssertions;if(!value)throw std::runtime_error(what);}
struct RecentViewTestAccess{
static void Run(const fs::path& dir){
    const int64_t now=UnixNow();
    std::vector<RecentEntry> entries;
    for(int i=0;i<8;++i)entries.push_back({dir/(L"文件"+std::to_wstring(i)+L".pdf"),now-i*3600*20,i==5});
    RecentView view;
    std::vector<fs::path> requested,opened,removed;std::vector<std::pair<fs::path,bool>> pins;
    view.request_info=[&](const fs::path& p,int64_t){requested.push_back(p);};
    view.open=[&](const fs::path& p){opened.push_back(p);};
    view.remove=[&](const fs::path& p){removed.push_back(p);};
    view.pin=[&](const fs::path& p,bool v){pins.push_back({p,v});};
    view.Entries(entries);view.Arrange({0,0,900,600});
    Check(view.rows_.size()==8,"every entry must be listed");
    Check(!view.cards_.empty()&&view.cards_.size()<=5,"continue-reading cards missing");
    Check(view.items_[view.cards_[0].item].entry.opened==now,"first card must be the most recent file");
    Check(!view.headers_.empty()&&view.headers_[0].text==L"继续阅读","continue header missing");
    // 分组标题按时间顺序且不重复。
    int groups=0;for(const auto& h:view.headers_)if(h.text==L"今天"||h.text==L"昨天"||h.text==L"近 7 天"||h.text==L"更早")++groups;
    Check(groups>=2,"time groups missing");
    // 点击行打开；点击操作按钮不打开。
    const auto row=view.rows_[0].rect;
    view.OnMouseMove({row.x+100,row.y+20-view.scroll_},0);
    view.OnMouseDown({row.x+100,row.y+20-view.scroll_},1);view.OnMouseUp({row.x+100,row.y+20-view.scroll_},0);
    Check(opened.size()==1&&opened[0]==view.items_[view.rows_[0].item].entry.path,"row click did not open");
    const auto pin=view.ActionRect(row,0);
    view.OnMouseMove({pin.x+10,pin.y+10-view.scroll_},0);
    view.OnMouseDown({pin.x+10,pin.y+10-view.scroll_},1);view.OnMouseUp({pin.x+10,pin.y+10-view.scroll_},0);
    Check(pins.size()==1&&opened.size()==1,"pin button opened the file or did nothing");
    const auto x=view.ActionRect(row,2);
    view.OnMouseDown({x.x+10,x.y+10-view.scroll_},1);view.OnMouseUp({x.x+10,x.y+10-view.scroll_},0);
    Check(removed.size()==1&&opened.size()==1,"remove button failed");
    // 按下后移出再松开，不触发。
    view.OnMouseDown({row.x+100,row.y+20-view.scroll_},1);view.OnMouseUp({row.x+100,row.y+500},0);
    Check(opened.size()==1,"drag-off click still opened");
    // 键盘：下移 + Enter 打开第二行，Delete 移除。
    view.selected_=0;view.OnKey(VK_DOWN);view.OnKey(VK_RETURN);
    Check(opened.size()==2&&opened[1]==view.items_[view.rows_[1].item].entry.path,"keyboard open failed");
    view.OnKey(VK_DELETE);Check(removed.size()==2,"keyboard remove failed");
    // 筛选：不区分大小写、匹配文件夹；筛选时不显示卡片。
    view.Filter(L"文件3");Check(view.rows_.size()==1&&view.cards_.empty(),"filter by name failed");
    view.Filter(L"NOPE");Check(view.rows_.empty(),"filter should show empty result");
    view.Filter(L"");Check(view.rows_.size()==8,"clearing filter did not restore rows");
    // 信息回填：缺失文件不进入继续阅读卡片。
    view.AcceptInfo(view.items_[view.cards_[0].item].entry.path,view.items_[view.cards_[0].item].entry.opened,RecentInfo{RecentInfo::State::Missing});
    for(const auto& c:view.cards_)Check(view.items_[c.item].info.state!=RecentInfo::State::Missing,"missing file shown as a card");
    // 重建列表时保留已获取的信息，不重复请求。
    RecentInfo present;present.state=RecentInfo::State::Present;present.pages=12;present.bytes=2048;
    view.AcceptInfo(entries[1].path,entries[1].opened,present);
    view.Entries(entries);
    bool kept=false;for(const auto& item:view.items_)if(item.entry.path==entries[1].path)kept=item.info.pages==12;
    Check(kept,"info lost on refresh");
    // 滚动：内容超出时滚轮生效且不越界。
    view.Arrange({0,0,900,300});view.OnWheel(-20);for(int i=0;i<200;++i)view.OnAnimate(1.0f/60);
    Check(view.scroll_>0&&view.scroll_<=view.content_-300+.5f,"wheel scrolling out of range");
    // 少于 3 个文件时不显示卡片，只显示列表。
    view.Entries({entries[0],entries[1]});view.Arrange({0,0,900,600});
    Check(view.cards_.empty()&&view.rows_.size()==2,"small history should be a plain list");
    view.Entries({});Check(view.rows_.empty()&&view.cards_.empty(),"empty history");
    bool browsed=false;view.browse=[&]{browsed=true;};view.emptyButtonY_=200;
    view.OnMouseDown({450,215},1);view.OnMouseUp({450,215},0);Check(browsed,"empty-state open button");
}
};
}
using namespace lpdf;
int main(){
    try{
        wchar_t temp[MAX_PATH];GetTempPathW(MAX_PATH,temp);
        const fs::path dir=fs::path(temp)/L"lumenpdf-recent-test";std::error_code error;fs::remove_all(dir,error);fs::create_directories(dir);
        const int64_t now=UnixNow();
        // 解析：v2 行、旧格式整行路径、相对路径与重复路径被丢弃。
        const auto parsed=RecentFiles::Parse("lumen-recent 2\r\n100\t1\tC:\\a\\甲.pdf\n200\t0\tC:\\A\\甲.PDF\nC:\\b\\乙.pdf\nrelative.pdf\n\nbad\tline\n");
        Check(parsed.size()==2&&parsed[0].pinned&&parsed[0].opened==100&&parsed[1].path==fs::path(L"C:\\b\\乙.pdf"),"parse");
        Check(RecentFiles::Parse(RecentFiles::Serialize(parsed)).size()==2,"serialize round trip");
        RecentFiles r;r.Load(dir/L"store.txt");
        for(int i=0;i<40;++i)r.Touch(dir/(std::to_wstring(i)+L".pdf"),now-1000+i);
        Check(r.Entries().size()==RecentFiles::kLimit,"limit");
        Check(r.Entries().front().path.filename()==L"39.pdf","newest first");
        r.Pin(dir/L"10.pdf",true);
        Check(r.Entries().front().path.filename()==L"10.pdf"&&r.Pinned(dir/L"10.pdf"),"pinned first");
        for(int i=40;i<80;++i)r.Touch(dir/(std::to_wstring(i)+L".pdf"),now+i);
        Check(r.Pinned(dir/L"10.pdf"),"pinned entry evicted by limit");
        r.Touch(dir/L"10.PDF",now+500);
        size_t tens=0;for(const auto& e:r.Entries())tens+=RecentFiles::SamePath(e.path,dir/L"10.pdf")?1:0;
        Check(tens==1&&r.Pinned(dir/L"10.pdf"),"case-insensitive dedupe must keep pin");
        Check(r.Save(),"save");
        RecentFiles reloaded;reloaded.Load(dir/L"store.txt");
        Check(reloaded.Entries().size()==r.Entries().size()&&reloaded.Entries().front().pinned,"reload");
        reloaded.Clear(true);Check(reloaded.Entries().size()==1,"clear keeps pinned");
        reloaded.Remove(dir/L"10.pdf");Check(reloaded.Entries().empty(),"remove");
        RecentFiles locked;locked.Load(dir/L"store.txt");locked.ReadOnly(true);locked.Clear(false);
        Check(!locked.Save(),"read-only store must not write");
        RecentFiles still;still.Load(dir/L"store.txt");Check(!still.Entries().empty(),"read-only store was modified");
        // 分组与相对时间。
        Check(GroupOf({{},now-30,false},now)==RecentGroup::Today||GroupOf({{},now-30,false},now)==RecentGroup::Yesterday,"today group");
        Check(GroupOf({{},now-30*86400,false},now)==RecentGroup::Earlier,"earlier group");
        Check(GroupOf({{},now,true},now)==RecentGroup::Pinned,"pinned group");
        Check(RelativeTime(now-10,now)==L"刚刚"&&RelativeTime(now-300,now)==L"5 分钟前","relative minutes");
        Check(RelativeTime(now-40*86400,now).size()==10,"absolute date");
        Check(FormatBytes(512)==L"512 B"&&FormatBytes(1536)==L"1.5 KB"&&FormatBytes(50ull*1024*1024)==L"50 MB","bytes");
        // 阅读位置：解析容错、去重、上限、原子保存与只读模式。
        {
            const auto items=ReadingPositions::Parse("lumen-positions 1\n10\t4\t120.50\t2\t1.0000\tC:\\a\\甲.pdf\n11\t1\t0\t1\t1\tC:\\A\\甲.PDF\n12\t-1\t0\t1\t1\tC:\\b.pdf\n13\t2\tnan\t9\t1\tC:\\c.pdf\nbad line\n14\t3\t5\t0\t99\tC:\\d.pdf\n");
            Check(items.size()==2&&items[0].page==4&&std::abs(items[0].offset-120.5f)<.01f&&items[0].fit==2,"positions parse");
            Check(items[1].path==fs::path(L"C:\\d.pdf")&&items[1].zoom==1,"invalid zoom must reset");
            Check(ReadingPositions::Parse(ReadingPositions::Serialize(items)).size()==2,"positions round trip");
            ReadingPositions store;store.Load(dir/L"positions.txt");
            Check(store.Remember({dir/L"x.pdf",5,33,1,1,now}),"remember");
            Check(!store.Remember({dir/L"X.PDF",5,33,1,1,now+1}),"unchanged position must not mark dirty again");
            Check(store.Remember({dir/L"x.pdf",6,0,2,1,now+2})&&store.Size()==1,"update in place");
            for(int i=0;i<320;++i)store.Remember({dir/(L"p"+std::to_wstring(i)+L".pdf"),i,0,1,1,now+i});
            Check(store.Size()==ReadingPositions::kLimit&&store.Find(dir/L"p319.pdf")&&!store.Find(dir/L"x.pdf"),"positions limit keeps newest");
            {
                // 两个窗口共享存储：各自保存时保留对方写入的位置。
                ReadingPositions w1,w2;const auto shared=dir/L"shared-positions.txt";
                w1.Load(shared);w2.Load(shared);
                w1.Remember({dir/L"one.pdf",3,0,1,1,now});Check(w1.Save(),"window 1 save");
                w2.Remember({dir/L"two.pdf",7,0,1,1,now+1});Check(w2.Save(),"window 2 save");
                ReadingPositions check;check.Load(shared);
                Check(check.Find(dir/L"one.pdf")&&check.Find(dir/L"one.pdf")->page==3&&check.Find(dir/L"two.pdf"),"multi-window positions merge");
            }
            Check(store.Save()&&!store.Dirty(),"positions save");
            ReadingPositions again;again.Load(dir/L"positions.txt");
            Check(again.Size()==ReadingPositions::kLimit&&again.Find(dir/L"P319.pdf")->page==319,"positions reload");
            again.ReadOnly(true);again.Forget(dir/L"p319.pdf");Check(!again.Save(),"read-only positions must not write");
        }
        // 打印版面：等比适配、居中、自动旋转，以及“仅缩小”模式。
        {
            const auto a4=PlacePage(595,842,4800,6600,600,600,true,true);
            Check(!a4.rotate&&a4.width<=4800&&a4.height<=6600&&(a4.width==4800||a4.height==6600),"portrait fit");
            Check(std::abs(a4.x-(4800-a4.width)/2)<=1&&std::abs(a4.y-(6600-a4.height)/2)<=1,"centered");
            const auto wide=PlacePage(842,595,4800,6600,600,600,true,true);
            Check(wide.rotate&&wide.width<wide.height,"landscape page rotates on portrait paper");
            const auto keep=PlacePage(842,595,4800,6600,600,600,true,false);
            Check(!keep.rotate&&keep.width==4800,"auto-rotate off");
            const auto tiny=PlacePage(200,200,4800,6600,600,600,false,true);
            Check(std::abs(tiny.scale-1)<.001f&&tiny.width==1667,"shrink-only keeps natural size");
            const auto none=PlacePage(0,842,4800,6600,600,600,true,true);Check(none.width==0,"invalid page");
        }
        {
            PrintSettings s;s.pages={0,1,2,3,4};s.pagesPerSheet=4;
            auto sheets=PlanPrint(s);Check(sheets.size()==2&&sheets[0]==PrintSheet({0,1,2,3})&&sheets[1]==PrintSheet({4}),"n-up incomplete final sheet");
            s.parity=1;s.reverse=true;sheets=PlanPrint(s);Check(sheets.size()==1&&sheets[0]==PrintSheet({4,2,0}),"odd reverse");
            s.parity=2;sheets=PlanPrint(s);Check(sheets[0]==PrintSheet({3,1}),"even reverse");
            s.parity=0;s.reverse=false;s.booklet=true;sheets=PlanPrint(s);
            Check(sheets==std::vector<PrintSheet>({{-1,0},{1,-1},{-1,2},{3,4}}),"booklet pad and impose");
            s.bookletSide=1;Check(PlanPrint(s)==std::vector<PrintSheet>({{-1,0},{-1,2}}),"booklet fronts");
            s.bookletSide=2;Check(PlanPrint(s)==std::vector<PrintSheet>({{1,-1},{3,4}}),"booklet backs");
            s.pages.clear();Check(PlanPrint(s).empty(),"empty print range");
        }
        // 设置：往返、默认值、越界夹紧、未知键与坏行忽略；保存为原子替换。
        {
            AppSettings d;Check(d.tone==0&&d.layout==0&&d.restorePosition&&!d.externalNewWindow&&d.exportDpi==150,"settings defaults");
            Check(d.sidebarWidth==260&&d.sidebarTab==0&&!d.paged&&!d.autoReload,"reading settings defaults");
            AppSettings s;s.tone=2;s.layout=1;s.defaultFit=1;s.restorePosition=false;s.externalNewWindow=true;s.logging=false;s.exportDpi=300;s.askedDefault=true;
            s.sidebarWidth=333;s.sidebarTab=2;s.paged=true;s.autoReload=true;
            Check(AppSettings::Parse(s.Serialize())==s,"settings round trip");
            const auto clamped=AppSettings::Parse("sidebar_width=5000\nsidebar_tab=9\npaged=7\n");
            Check(clamped.sidebarWidth==600&&clamped.sidebarTab==3&&clamped.paged,"reading settings clamped");
            const auto odd=AppSettings::Parse("lumen-settings 1\r\ntone=9\r\nlayout=-4\r\nexport_dpi=abc\r\nfuture=1\r\nbroken\r\nexport_dpi=5000\r\n");
            Check(odd.tone==2&&odd.layout==0&&odd.exportDpi==600,"settings clamp and ignore unknown");
            Check(AppSettings::Parse("tone=1\n")==[]{AppSettings a;a.tone=1;return a;}(),"legacy tone-only file");
            const auto file=dir/L"设置/settings.txt";
            Check(SaveSettingsFile(file,s)&&LoadSettingsFile(file)==s&&!fs::exists(fs::path(file).concat(L".tmp")),"settings file save/load");
            Check(LoadSettingsFile(dir/L"missing.txt")==AppSettings{},"missing settings file gives defaults");
        }
        // 日志：写入、换行清理、超过上限轮换。
        {
            log::Init(dir/L"logs");log::Info(L"打开 测试.pdf\r\n第二行");
            std::ifstream in(dir/L"logs/lumenpdf.log",std::ios::binary);std::string line;std::getline(in,line);in.close();
            Check(line.find("INFO")!=std::string::npos&&line.find("\xE7\xAC\xAC\xE4\xBA\x8C")!=std::string::npos&&line.find('\n')==std::string::npos,"log line");
            {std::ofstream big(dir/L"logs/lumenpdf.log",std::ios::binary|std::ios::app);big<<std::string(1100*1024,'x');}
            log::Warn(L"after rotate");
            Check(fs::exists(dir/L"logs/lumenpdf.log.1")&&fs::file_size(dir/L"logs/lumenpdf.log")<1024,"log rotation");
            log::Init({});log::Error(L"disabled");
            Check(fs::file_size(dir/L"logs/lumenpdf.log")<1024,"disabled log writes nothing");
        }
        lumen::App app;RecentViewTestAccess::Run(dir);
        fs::remove_all(dir,error);
        std::cout<<"PASS recent files store, grouping and home view: "<<recentAssertions<<" assertions\n";
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
    return 0;
}