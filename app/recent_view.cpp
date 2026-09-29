#include "recent_view.h"
#include "ui.h"
#include <lumen/Animate.h>
#include <lumen/Menu.h>
#include <lumen/Window.h>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
namespace lpdf {
namespace {
using lumen::Color;using lumen::TextRole;using lumen::Align;
Color C(unsigned rgb,float alpha=1){return Color::Hex(rgb,alpha);}
constexpr float kRow=58,kRowGap=2,kHeader=34,kCardH=228,kCardGap=16,kCardMin=190,kThumbH=150,kBar=10;
std::wstring Lower(std::wstring s){for(auto& c:s)c=static_cast<wchar_t>(std::towlower(c));return s;}
// 超宽文本截断：middle=true 保留首尾（路径），否则保留开头（文件名）。二分查找，测量次数 O(log n)。
std::wstring Elide(lumen::Painter& p,const std::wstring& text,TextRole role,float width,bool middle){
    if(width<=8||text.empty())return {};
    if(p.MeasureText(text,role).w<=width)return text;
    auto make=[&](size_t keep){
        if(!middle)return text.substr(0,keep)+L"…";
        const size_t head=std::min<size_t>(keep/3,text.size()),tail=std::min(keep-head,text.size()-head);
        return text.substr(0,head)+L"…"+text.substr(text.size()-tail);
    };
    size_t lo=0,hi=text.size();
    while(lo<hi){const size_t mid=(lo+hi+1)/2;if(p.MeasureText(make(mid),role).w<=width)lo=mid;else hi=mid-1;}
    return make(lo);
}
}
std::wstring FormatBytes(uint64_t bytes){
    const wchar_t* units[]={L"B",L"KB",L"MB",L"GB"};double v=static_cast<double>(bytes);int u=0;
    while(v>=1024&&u<3){v/=1024;++u;}
    wchar_t b[32];if(u==0)swprintf_s(b,L"%llu B",static_cast<unsigned long long>(bytes));else swprintf_s(b,v<10?L"%.1f %s":L"%.0f %s",v,units[u]);
    return b;
}
RecentView::RecentView(){AccessibleName(L"最近打开的文件");}
void RecentView::Entries(const std::vector<RecentEntry>& entries){
    std::vector<Item> next;next.reserve(entries.size());
    for(const auto& e:entries){
        Item item;item.entry=e;item.kind=FileKind(e.path);item.name=e.path.filename().wstring();item.folder=e.path.parent_path().wstring();
        item.lowered=Lower(item.name+L"\n"+item.folder);
        // 同一路径、同一次打开记录的信息与缩略图直接复用；重新打开/保存后时间变化，重新获取。
        auto old=std::find_if(items_.begin(),items_.end(),[&](const Item& o){return o.entry.opened==e.opened&&RecentFiles::SamePath(o.entry.path,e.path);});
        if(old!=items_.end()){item.info=std::move(old->info);item.requested=old->requested;item.bitmap=std::move(old->bitmap);}
        next.push_back(std::move(item));
    }
    items_=std::move(next);Layout();
    selected_=rows_.empty()?-1:std::clamp(selected_,-1,static_cast<int>(rows_.size())-1);
    Invalidate();
}
void RecentView::Filter(std::wstring text){
    auto lowered=Lower(std::move(text));
    while(!lowered.empty()&&lowered.back()==L' ')lowered.pop_back();
    if(lowered==filter_)return;
    filter_=std::move(lowered);scroll_=target_=0;selected_=-1;hover_={};Layout();Invalidate();
}
void RecentView::AcceptInfo(const fs::path& path,int64_t opened,RecentInfo info){
    for(auto& item:items_)if(item.entry.opened==opened&&RecentFiles::SamePath(item.entry.path,path)){
        const bool relayout=(info.state==RecentInfo::State::Missing)!=(item.info.state==RecentInfo::State::Missing);
        item.info=std::move(info);item.bitmap.Reset();
        if(relayout)Layout();
        Invalidate();return;
    }
}
lumen::Size RecentView::Measure(lumen::Size a,const lumen::Theme&){return {std::max(320.0f,a.w),std::max(200.0f,a.h)};}
void RecentView::Arrange(const lumen::Rect& r){Control::Arrange(r);Layout();}
void RecentView::Layout(){
    cards_.clear();rows_.clear();headers_.clear();
    const float width=std::max(0.0f,absolute_.w-kBar);float y=0;now_=UnixNow();
    if(filter_.empty()&&items_.size()>=3){
        // 继续阅读：最近打开且仍存在的文件，按时间倒序，一行放得下几张放几张。
        std::vector<int> recent;
        for(int i=0;i<static_cast<int>(items_.size());++i)if(items_[i].info.state!=RecentInfo::State::Missing)recent.push_back(i);
        std::stable_sort(recent.begin(),recent.end(),[&](int a,int b){return items_[a].entry.opened>items_[b].entry.opened;});
        const int columns=std::clamp(static_cast<int>((width+kCardGap)/(kCardMin+kCardGap)),2,5);
        const int count=std::min(columns,static_cast<int>(recent.size()));
        if(count>0){
            headers_.push_back({L"继续阅读",y});y+=kHeader;
            const float cardW=(width-(columns-1)*kCardGap)/columns;
            for(int i=0;i<count;++i)cards_.push_back({recent[i],{i*(cardW+kCardGap),y,cardW,kCardH}});
            y+=kCardH+22;
        }
    }
    std::optional<RecentGroup> last;int matches=0;
    for(int i=0;i<static_cast<int>(items_.size());++i){
        const auto& item=items_[i];
        if(!filter_.empty()&&item.lowered.find(filter_)==std::wstring::npos)continue;
        ++matches;
        if(filter_.empty()){
            const auto group=GroupOf(item.entry,now_);
            if(group!=last){if(last)y+=10;headers_.push_back({std::wstring(GroupTitle(group)),y});y+=kHeader;last=group;}
        }else if(matches==1){headers_.push_back({L"筛选结果",y});y+=kHeader;}
        rows_.push_back({i,{0,y,width,kRow}});y+=kRow+kRowGap;
    }
    if(!filter_.empty()&&!headers_.empty())headers_.front().text=L"筛选结果 · "+std::to_wstring(matches);
    content_=rows_.empty()&&cards_.empty()?0:y+12;
    const float limit=std::max(0.0f,content_-absolute_.h);
    scroll_=std::clamp(scroll_,0.0f,limit);target_=std::clamp(target_,0.0f,limit);
}
lumen::Rect RecentView::ActionRect(const lumen::Rect& row,int index)const{
    return {row.x+row.w-10-(3-index)*34.0f,row.y+(row.h-30)/2,30,30};
}
RecentView::Hit RecentView::HitTest(lumen::Point local)const{
    if(rows_.empty()&&cards_.empty()){
        const lumen::Rect button{absolute_.w*.5f-70,emptyButtonY_,140,36};
        if(filter_.empty()&&button.Contains(local))return {Part::Empty,-1};
        return {};
    }
    if(local.x<0||local.y<0||local.x>absolute_.w||local.y>absolute_.h)return {};
    const lumen::Point p{local.x,local.y+scroll_};
    for(const auto& c:cards_)if(c.rect.Contains(p))return {Part::Card,c.item};
    for(const auto& r:rows_)if(r.rect.Contains(p)){
        for(int i=0;i<3;++i)if(ActionRect(r.rect,i).Contains(p))return {i==0?Part::Pin:i==1?Part::Reveal:Part::Remove,r.item};
        return {Part::Row,r.item};
    }
    return {};
}
void RecentView::Prepare(lumen::Painter& p){
    if(device_!=p.DeviceIdentity()){for(auto& item:items_)item.bitmap.Reset();device_=p.DeviceIdentity();}
    for(auto& item:items_){
        const auto& t=item.info.thumb;
        if(!item.bitmap&&!t.bgra.empty()&&t.width>0&&t.height>0)
            item.bitmap.Attach(p.CreateBitmapBgra(static_cast<uint32_t>(t.width),static_cast<uint32_t>(t.height),t.bgra.data(),static_cast<uint32_t>(t.stride)));
    }
}
const std::wstring& RecentView::ShortFolder(lumen::Painter& p,const Item& item,float width)const{
    if(std::abs(item.shortWidth-width)>.5f){item.shortFolder=Elide(p,item.folder,TextRole::Caption,width,true);item.shortWidth=width;}
    return item.shortFolder;
}
void RecentView::Draw(lumen::Painter& p,const lumen::Theme& theme){
    now_=UnixNow();
    p.PushClip(absolute_);
    if(rows_.empty()&&cards_.empty()){DrawEmpty(p,theme);p.PopClip();return;}
    const float top=scroll_-40,bottom=scroll_+absolute_.h+40;
    auto want=[&](Item& item){if(!item.requested&&request_info){item.requested=true;request_info(item.entry.path,item.entry.opened);}};
    for(const auto& h:headers_){
        if(h.y+kHeader<top||h.y>bottom)continue;
        const auto r=Screen({0,h.y,absolute_.w-kBar,kHeader-8});
        p.DrawText(h.text,{r.x+2,r.y,r.w,r.h},TextRole::CaptionStrong,C(0x9a9a9a));
        const float tw=p.MeasureText(h.text,TextRole::CaptionStrong).w;
        const float ly=r.y+r.h*.5f+.5f;
        if(r.x+tw+14<r.Right())p.DrawLine({r.x+tw+14,ly},{r.Right(),ly},C(0xffffff,.07f),1);
    }
    for(const auto& c:cards_)if(c.rect.Bottom()>=top&&c.rect.y<=bottom){want(items_[c.item]);DrawCard(p,theme,c);}
    for(const auto& r:rows_)if(r.rect.Bottom()>=top&&r.rect.y<=bottom){want(items_[r.item]);DrawRow(p,theme,r);}
    if(content_>absolute_.h){
        const float track=absolute_.h-8,h=std::max(28.0f,track*absolute_.h/content_);
        const float y=absolute_.y+4+(track-h)*scroll_/std::max(1.0f,content_-absolute_.h);
        p.FillRoundedRect({absolute_.Right()-5,y,3,h},1.5f,C(0xffffff,.22f));
    }
    p.PopClip();
}
void RecentView::DrawCard(lumen::Painter& p,const lumen::Theme& theme,const Slot& slot)const{
    const auto& item=items_[slot.item];const auto r=Screen(slot.rect);
    const bool hot=hover_.item==slot.item&&hover_.part==Part::Card,down=hot&&pressed_.part==Part::Card&&pressed_.item==slot.item;
    if(hot)p.DrawGlow(r,12,C(0xffffff,.05f),.7f);
    p.FillRoundedRect(r,12,C(down?0x1d1d1d:hot?0x1a1a1a:0x151515));
    p.StrokeRoundedRect(r.Inset(.5f,.5f),12,C(hot?0x3c3c3c:0x262626),1);
    const lumen::Rect well{r.x+12,r.y+12,r.w-24,kThumbH};
    p.FillRoundedRect(well,8,C(0x0e0e0e));
    const auto& info=item.info;
    if(item.bitmap&&info.thumb.width>0){
        const float sx=(well.w-24)/info.thumb.width,sy=(well.h-20)/info.thumb.height,s=std::min(sx,sy);
        const float w=info.thumb.width*s,h=info.thumb.height*s;
        const lumen::Rect page{well.x+(well.w-w)*.5f,well.y+(well.h-h)*.5f,w,h};
        p.DrawGlow(page,1,C(0xffffff,hot?.06f:.035f),.35f);
        p.FillRect(page.Inset(-1,-1),C(0x000000,.6f));
        p.DrawBitmap(item.bitmap.Get(),page,true);
    }else{
        const auto glyph=info.locked?std::wstring_view(lumen::icon::kLock):ui::FileGlyph(item.kind);
        p.DrawIcon(glyph,well,30,C(info.state==RecentInfo::State::Unknown?0x3a3a3a:0x707070));
    }
    if(item.entry.pinned){
        const lumen::Rect badge{well.Right()-30,well.y+6,24,24};
        p.FillRoundedRect(badge,12,C(0x000000,.55f));p.DrawIcon(lumen::icon::kPin,badge,12,C(0xf0f0f0));
    }
    auto name=Elide(p,item.name,TextRole::BodyStrong,r.w-28,false);
    p.DrawText(name,{r.x+14,well.Bottom()+10,r.w-28,22},TextRole::BodyStrong,theme.text);
    std::wstring meta=RelativeTime(item.entry.opened,now_);
    if(info.pages>0)meta+=L"  ·  "+std::to_wstring(info.pages)+L" 页";
    else if(info.locked)meta+=L"  ·  受密码保护";
    p.DrawText(Elide(p,meta,TextRole::Caption,r.w-28,false),{r.x+14,well.Bottom()+34,r.w-28,18},TextRole::Caption,C(0x8a8a8a));
}
void RecentView::DrawRow(lumen::Painter& p,const lumen::Theme& theme,const Slot& slot)const{
    const auto& item=items_[slot.item];const auto r=Screen(slot.rect);
    const bool hot=hover_.item==slot.item&&hover_.part!=Part::Card&&hover_.part!=Part::None;
    const bool down=pressed_.item==slot.item&&pressed_.part==Part::Row&&hot;
    const bool missing=item.info.state==RecentInfo::State::Missing;
    const int index=static_cast<int>(&slot-rows_.data());
    if(hot||down)p.FillRoundedRect(r,8,C(0xffffff,down?.07f:.045f));
    if(focused_&&index==selected_)p.StrokeRoundedRect(r.Inset(.5f,.5f),8,C(0xffffff,.45f),1);
    const lumen::Rect tile{r.x+10,r.y+(r.h-38)/2,38,38};
    if(missing){
        // 状态色只作附加提示：文字仍写明“文件已移动或删除”。
        p.FillRoundedRect(tile,9,theme.warning_subtle);
        p.DrawIcon(lumen::icon::kWarning,tile,17,theme.warning);
    }else if(item.info.locked){
        p.FillRoundedRect(tile,9,C(0x1c1c1c));p.StrokeRoundedRect(tile.Inset(.5f,.5f),9,C(0x2c2c2c),1);
        p.DrawIcon(lumen::icon::kLock,tile,17,C(0xd8d8d8));
    }else ui::DrawFileIcon(p,item.kind,tile,38,17);
    // 右侧信息列：窄窗口时省略大小列；悬停时换成操作按钮。
    const bool wide=r.w>=620;
    const float metaW=wide?250.0f:130.0f;
    const float textX=tile.Right()+12,textW=std::max(40.0f,r.Right()-metaW-16-textX);
    auto name=Elide(p,item.name,TextRole::Body,textW-(item.entry.pinned?20:0),false);
    p.DrawText(name,{textX,r.y+9,textW,22},TextRole::Body,missing?theme.text_disabled:theme.text);
    if(item.entry.pinned){
        const float nw=p.MeasureText(name,TextRole::Body).w;
        p.DrawIcon(lumen::icon::kPin,{textX+nw+6,r.y+13,14,14},11,C(0xbdbdbd));
    }
    p.DrawText(missing?L"文件已移动或删除 · "+item.folder:ShortFolder(p,item,textW),{textX,r.y+31,textW,18},TextRole::Caption,C(missing?0x5c5c5c:0x808080));
    if(hot){
        const wchar_t* glyphs[]={lumen::icon::kPin,lumen::icon::kFolderOpen,lumen::icon::kClose};
        const Part parts[]={Part::Pin,Part::Reveal,Part::Remove};
        for(int i=0;i<3;++i){
            const auto a=Screen(ActionRect(slot.rect,i));
            const bool over=hover_.part==parts[i];const bool disabled=missing&&i<2;
            if(over&&!disabled)p.FillRoundedRect(a,7,C(0xffffff,pressed_.part==parts[i]?.14f:.09f));
            const bool active=i==0&&item.entry.pinned;
            p.DrawIcon(glyphs[i],a,14,C(disabled?0x444444:active?0xffffff:over?0xeeeeee:0x9a9a9a));
        }
        return;
    }
    const float right=r.Right()-16;
    std::wstring size;
    if(item.info.state==RecentInfo::State::Present){
        size=FormatBytes(item.info.bytes);
        if(item.info.pages>0)size=std::to_wstring(item.info.pages)+L" 页 · "+size;
    }
    if(wide){
        p.DrawText(RelativeTime(item.entry.opened,now_),{right-250,r.y,120,r.h},TextRole::Caption,C(0x9a9a9a));
        p.DrawText(size,{right-125,r.y,125,r.h},TextRole::Caption,C(0x7a7a7a),Align::Trailing);
    }else p.DrawText(RelativeTime(item.entry.opened,now_),{right-130,r.y,130,r.h},TextRole::Caption,C(0x9a9a9a),Align::Trailing);
}
void RecentView::DrawEmpty(lumen::Painter& p,const lumen::Theme& theme)const{
    const bool filtered=!filter_.empty()&&!items_.empty();
    const float cx=absolute_.x+absolute_.w*.5f;
    const float y=absolute_.y+std::max(24.0f,absolute_.h*.5f-120);
    const lumen::Rect circle{cx-38,y,76,76};
    p.DrawGlow(circle,38,C(0xffffff,.035f),.8f);
    p.FillRoundedRect(circle,38,C(0x161616));p.StrokeRoundedRect(circle.Inset(.5f,.5f),38,C(0x2e2e2e),1);
    p.DrawIcon(filtered?lumen::icon::kSearch:lumen::icon::kClock,circle,28,C(0xd0d0d0));
    const float w=std::min(absolute_.w-32,420.0f);
    p.DrawText(filtered?L"没有匹配的文件":L"还没有最近打开的文档",{cx-w*.5f,circle.Bottom()+22,w,26},TextRole::BodyStrong,theme.text,Align::Center);
    p.DrawText(filtered?L"换个文件名或文件夹关键字试试":L"打开过的 PDF、Word、TXT 与图片会出现在这里",{cx-w*.5f,circle.Bottom()+50,w,20},TextRole::Caption,C(0x8a8a8a),Align::Center);
    if(!filtered)p.DrawText(L"下次启动即可一键继续阅读",{cx-w*.5f,circle.Bottom()+70,w,20},TextRole::Caption,C(0x8a8a8a),Align::Center);
    const_cast<RecentView*>(this)->emptyButtonY_=circle.Bottom()+108-absolute_.y;
    if(filtered)return;
    const lumen::Rect button{cx-70,absolute_.y+emptyButtonY_,140,36};
    const bool hot=hover_.part==Part::Empty;
    p.FillRoundedRect(button,7,hot?theme.accent_hover:theme.accent);
    p.DrawIcon(lumen::icon::kFolderOpen,{button.x+16,button.y,18,button.h},14,theme.accent_text);
    p.DrawText(L"打开文档",{button.x+38,button.y,button.w-50,button.h},TextRole::BodyStrong,theme.accent_text,Align::Center);
}
bool RecentView::OnAnimate(float dt){
    const bool base=Control::OnAnimate(dt);
    const float before=scroll_;
    const bool moving=lumen::EaseTo(scroll_,target_,dt,18.0f,.1f);
    if(before!=scroll_)Invalidate();
    return base||moving;
}
void RecentView::ScrollTo(float y,bool smooth){
    target_=std::clamp(y,0.0f,std::max(0.0f,content_-absolute_.h));
    if(smooth)Animate();else{scroll_=target_;Invalidate();}
}
bool RecentView::OnWheel(float delta){
    if(content_<=absolute_.h)return false;
    UINT lines=3;SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
    const float step=lines==WHEEL_PAGESCROLL?absolute_.h*.85f:36.0f*lines;
    if(delta*(scroll_-target_)>0)target_=scroll_;
    ScrollTo(target_-delta*step,std::abs(delta)>=.35f);
    hover_={};return true;
}
void RecentView::Reveal(int row){
    if(row<0||row>=static_cast<int>(rows_.size()))return;
    const auto& r=rows_[row].rect;
    if(r.y<target_+8)ScrollTo(r.y-(row==0?kHeader+8:8));
    else if(r.Bottom()>target_+absolute_.h-8)ScrollTo(r.Bottom()-absolute_.h+8);
}
bool RecentView::OnKey(uint32_t key){
    if(rows_.empty())return false;
    const int last=static_cast<int>(rows_.size())-1;
    if(key==VK_DOWN){selected_=std::min(last,selected_+1);Reveal(selected_);Invalidate();return true;}
    if(key==VK_UP){selected_=std::max(0,selected_<0?0:selected_-1);Reveal(selected_);Invalidate();return true;}
    if(key==VK_HOME){selected_=0;Reveal(0);Invalidate();return true;}
    if(key==VK_END){selected_=last;Reveal(last);Invalidate();return true;}
    if(selected_<0)return false;
    if(key==VK_RETURN){Activate(rows_[selected_].item,Part::Row);return true;}
    if(key==VK_DELETE){Activate(rows_[selected_].item,Part::Remove);return true;}
    return false;
}
void RecentView::OnMouseMove(lumen::Point local,uint32_t){
    const auto hit=HitTest(local);
    if(hit.part!=hover_.part||hit.item!=hover_.item){hover_=hit;Invalidate();}
}
void RecentView::OnMouseDown(lumen::Point local,uint32_t buttons){
    if(!(buttons&1))return;Focus();pressed_=HitTest(local);
    for(int i=0;i<static_cast<int>(rows_.size());++i)if(rows_[i].item==pressed_.item&&pressed_.part!=Part::Card){selected_=i;break;}
    Invalidate();
}
void RecentView::OnMouseUp(lumen::Point local,uint32_t){
    const auto hit=HitTest(local);const auto was=pressed_;pressed_={};Invalidate();
    if(hit.part!=Part::None&&hit.part==was.part&&hit.item==was.item)Activate(hit.item,hit.part);
}
void RecentView::OnMouseLeave(){hover_={};pressed_={};Invalidate();}
void RecentView::OnFocusChanged(bool focused){Control::OnFocusChanged(focused);focused_=focused;if(focused&&selected_<0&&!rows_.empty())selected_=0;Invalidate();}
void RecentView::Activate(int index,Part part){
    if(part==Part::Empty){if(browse)browse();return;}
    if(index<0||index>=static_cast<int>(items_.size()))return;
    // 回调可能重建列表，先复制所需数据。
    const auto path=items_[index].entry.path;const bool pinned=items_[index].entry.pinned;
    const bool missing=items_[index].info.state==RecentInfo::State::Missing;
    switch(part){
        case Part::Card:case Part::Row:if(open)open(path);break;
        case Part::Pin:if(!missing&&pin)pin(path,!pinned);break;
        case Part::Reveal:if(!missing&&reveal)reveal(path);break;
        case Part::Remove:if(remove)remove(path);break;
        default:break;
    }
}
bool RecentView::ShowContextMenu(lumen::Point window_dip){
    auto* window=WindowOf();if(!window)return false;
    const auto hit=HitTest({window_dip.x-absolute_.x,window_dip.y-absolute_.y});
    if(hit.item<0||hit.item>=static_cast<int>(items_.size()))return false;
    const auto path=items_[hit.item].entry.path;const bool pinned=items_[hit.item].entry.pinned;
    const bool missing=items_[hit.item].info.state==RecentInfo::State::Missing;
    lumen::Menu menu;
    menu.AddItem(L"打开",[this,path]{if(open)open(path);}).Glyph(lumen::icon::kOpenFile).Shortcut(L"Enter").Disabled(missing);
    menu.AddItem(pinned?L"取消固定":L"固定到顶部",[this,path,pinned]{if(pin)pin(path,!pinned);}).Glyph(lumen::icon::kPin).Disabled(missing);
    menu.AddItem(L"在文件夹中显示",[this,path]{if(reveal)reveal(path);}).Glyph(lumen::icon::kFolderOpen).Disabled(missing);
    menu.AddItem(L"复制文件路径",[this,path]{if(copy_path)copy_path(path);}).Glyph(lumen::icon::kCopy);
    menu.AddSeparator();
    menu.AddItem(L"从列表中移除",[this,path]{if(remove)remove(path);}).Glyph(lumen::icon::kClose).Shortcut(L"Delete");
    hover_={};Invalidate();
    menu.Popup(*window,window_dip);
    return true;
}
lumen::CursorShape RecentView::CursorAt(lumen::Point local)const{
    return HitTest(local).part==Part::None?lumen::CursorShape::Arrow:lumen::CursorShape::Hand;
}
}
