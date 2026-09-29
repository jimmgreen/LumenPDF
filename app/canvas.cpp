#include "canvas.h"
#include "annotation_painter.h"
#include "text_layout.h"
#include "core/annotation_geometry.h"
#include <lumen/Window.h>
#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <numeric>
namespace lpdf {
namespace {
lumen::Color C(unsigned rgb,float alpha=1){return lumen::Color::Hex(rgb,alpha);}
// 画布内所有过渡（滚动、网格让位、浮起、落位、悬停）共用同一条指数缓动，保证动效一致。
constexpr float kMotion=18.0f;
Rect Between(Point a,Point b){return {std::min(a.x,b.x),std::min(a.y,b.y),std::abs(b.x-a.x),std::abs(b.y-a.y)};}
}
PdfCanvas::PdfCanvas(View view):view_(view){Clip(true);AccessibleName(view==View::Thumbnails?L"页面缩略图":L"PDF 文档");}
lumen::Size PdfCanvas::Measure(lumen::Size a,const lumen::Theme&){return {std::max(80.0f,a.w),std::max(120.0f,a.h)};}
void PdfCanvas::Arrange(const lumen::Rect& r){
    const bool resized=std::abs(r.w-absolute_.w)>.5f||std::abs(r.h-absolute_.h)>.5f;
    Control::Arrange(r);LayoutPages();
    if(pending_view_&&!layout_.empty()&&view_==View::Reading){const auto state=*pending_view_;pending_view_.reset();ApplyView(state);}
    else if(resized&&view_==View::Reading&&fit_==Fit::Page&&!draft_)GoTo(current_page_);
    PlaceEditor();
}
void PdfCanvas::DocumentPages(std::vector<PageInfo> pages,uint64_t generation){
    // 页面几何未变时保留旧瓦片作为底图，避免每次编辑后整页闪白；几何变化则全部丢弃。
    if(page_drag_.active){page_drag_=PageDrag{};order_.clear();dragging_=false;}
    bool same=pages.size()==pages_.size();
    for(size_t i=0;same&&i<pages.size();++i)same=std::abs(pages[i].width-pages_[i].width)<.01f&&std::abs(pages[i].height-pages_[i].height)<.01f&&std::abs(pages[i].originX-pages_[i].originX)<.01f&&std::abs(pages[i].originY-pages_[i].originY)<.01f;
    if(generation!=generation_){links_.clear();links_pending_.clear();hover_link_=-1;hover_link_page_=-1;press_link_.reset();hover_field_=-1;hover_field_page_=-1;press_field_.reset();text_selection_={};selecting_text_=false;}
    if(pages.empty())pending_view_.reset();
    pages_=std::move(pages);generation_=generation;if(!same){tiles_.clear();text_pages_.clear();settling_.reset();zoom_back_.clear();DropLens();speech_page_=-1;speech_boxes_.clear();}
    for(auto& [key,token]:pending_){(void)key;token->store(true);}pending_.clear();annotations_.clear();selected_pages_.clear();search_.reset();search_hits_.clear();search_current_=-1;hover_page_=-1;
    ghost_pending_.reset();if(ghost_&&ghost_->request.generation!=generation_&&!settling_)ghost_.reset();
    current_page_=std::clamp(current_page_,0,std::max(0,static_cast<int>(pages_.size())-1));LayoutPages();
}
void PdfCanvas::Display(View view){
    if(view!=View::Reading)ZoomBox(false);
    view_=view;page_drag_=PageDrag{};order_.clear();lifted_.clear();lift_=0;motion_=false;shown_.clear();
    scroll_=target_scroll_=0;text_selection_={};selecting_text_=false;hover_link_=-1;LayoutPages();Invalidate();}
void PdfCanvas::EditingTool(Tool tool){
    if(page_drag_.active)EndPageDrag(false);
    tool_=tool;moving_.reset();dragging_=false;resizing_=false;stroke_.clear();ghost_pending_.reset();
    if(tool!=Tool::Select){text_selection_={};selecting_text_=false;hover_link_=-1;}
    if(highlight_cancel_)highlight_cancel_->store(true);highlight_preview_.clear();
    RequestVisible();Invalidate();
}
void PdfCanvas::RequestHighlight(){
    if(highlight_cancel_)highlight_cancel_->store(true);
    if(!request_highlight||down_page_<0)return;
    if(IsMarkupTool(tool_)&&area_highlight_)return;
    if(tool_==Tool::Select&&(!selecting_text_||area_select_))return;
    highlight_cancel_=std::make_shared<std::atomic_bool>(false);
    if(tool_==Tool::Select){span_requested_.clear();RequestSpans();return;}
    request_highlight({down_page_,down_point_,last_point_,generation_,highlight_cancel_});
}
void PdfCanvas::AcceptHighlight(HighlightRequest request,std::vector<Quad> quads){
    if(request.cancelled->load()||request.cancelled!=highlight_cancel_||request.generation!=generation_)return;
    if(tool_==Tool::Select){
        // 松手后的最终请求同样被接受，因此不要求仍在拖动。范围已变的旧回复（拖动中）丢弃。
        if(!selecting_text_)return;
        auto same=[](Point a,Point b){return a.x==b.x&&a.y==b.y;};
        auto it=std::find_if(text_selection_.spans.begin(),text_selection_.spans.end(),[&](const TextSpan& s){return s.page==request.page;});
        if(it==text_selection_.spans.end())return;
        if(request.snap){
            // 双击 / 三击：吸附后的范围成为选择本身；空白处没有吸附到文字则不留选择。
            if(quads.empty()){text_selection_={};selecting_text_=false;Invalidate();return;}
            text_selection_.start=it->start=request.start;text_selection_.end=it->end=request.end;
        }else if(!same(it->start,request.start)||!same(it->end,request.end)||it->fromStart!=request.fromStart||it->toEnd!=request.toEnd)return;
        it->quads=std::move(quads);it->ready=true;
        if(it->page==text_selection_.page)text_selection_.quads=it->quads;
        if(!dragging_&&selection_changed)selection_changed(text_selection_);
        Invalidate();return;
    }
    if(!dragging_||!IsMarkupTool(tool_))return;
    highlight_preview_=std::move(quads);Invalidate();
}
void PdfCanvas::DrawVector(lumen::Painter& painter,int page,const Annotation& value)const{
    DrawAnnotationPreview(painter,value,layout_[page].scale,[&](Point p){auto r=ScreenRect(page,{p.x,p.y,0,0});return lumen::Point{r.x,r.y};});
}
int PdfCanvas::Hidden(int page)const{
    if(hide_annotations_)return -2;
    if(draft_)return draft_page_==page?draft_->id:-1;
    if(moving_&&down_page_==page&&!(moving_->type==Tool::Text&&moving_->lumenText))return moving_->id;
    // 落下后、文档尚未更新前，继续隐藏原位内容，由预览位图承担显示；文档更新后改请求完整瓦片。
    if(settling_&&settling_->page==page&&settling_->generation==generation_)return settling_->id;
    return -1;
}
float PdfCanvas::TileScale(int page)const{return std::clamp(std::round(layout_[page].scale*dpi_*100)/100.0f,.05f,8.0f);}
void PdfCanvas::SelectedPages(std::vector<int> p){selected_pages_=std::move(p);Invalidate();}
void PdfCanvas::LayoutPages(){
    content_width_=absolute_.w;layout_.clear();if(absolute_.w<=1||absolute_.h<=1)return;
    if(view_==View::Reading&&spread_){LayoutSpread();return;}
    const int n=static_cast<int>(pages_.size());const bool grid=Grid();
    const int columns=view_==View::Pages?std::max(1,static_cast<int>(absolute_.w/(220*grid_zoom_))):1;
    const float cell=(absolute_.w-24)/columns;float y=view_==View::Reading?PadTop():16.0f,rowHeight=0;
    // 网格按显示顺序排布：拖动中 order_ 为预览顺序，layout_ 仍按原页码索引，便于位置缓动与瓦片复用。
    std::vector<int> order=grid?order_:std::vector<int>{};
    if(static_cast<int>(order.size())!=n){order.resize(n);std::iota(order.begin(),order.end(),0);}
    layout_.resize(n);grid_rows_.clear();grid_columns_=columns;grid_cell_=cell;
    for(int pos=0;pos<n;++pos){
        const int i=order[pos];const auto& page=pages_[i];
        float available=view_==View::Reading?std::max(60.0f,absolute_.w-(presenting_?8.0f:52.0f)):std::max(45.0f,cell-32);
        const float pw=DisplayWidth(page),ph=DisplayHeight(page);
        float scale=available/std::max(1.0f,pw);
        if(view_==View::Reading&&fit_==Fit::Page)scale=std::min(scale,std::max(40.0f,absolute_.h-(presenting_?8.0f:40.0f))/std::max(1.0f,ph));
        if(view_==View::Reading&&fit_==Fit::Width)scale=std::min(scale,8.0f*(96.0f/72.0f));
        if(view_==View::Reading&&fit_==Fit::None)scale=zoom_*(96.0f/72.0f);
        if(grid)scale=std::min(scale,view_==View::Thumbnails?0.19f:0.28f*grid_zoom_);
        const float w=pw*scale,h=ph*scale;
        const int col=pos%columns;
        if(col==0&&pos){y+=rowHeight+24;rowHeight=0;}
        if(col==0)grid_rows_.push_back(y);
        const float x=view_==View::Reading?std::max(12.0f,(absolute_.w-w)/2)-horizontal_:12+col*cell+(cell-w)/2;
        content_width_=std::max(content_width_,w+24);layout_[i]={{x,y,w,h},scale,std::to_wstring(pos+1),pos/columns};
        rowHeight=std::max(rowHeight,h+24);
    }
    if(grid){
        // 没有进行中的过渡时直接落位（窗口缩放、切换视图不拖泥带水）；过渡中只更新尺寸，位置交给缓动。
        if(shown_.size()!=layout_.size()){shown_.assign(layout_.size(),{});motion_=false;lifted_.clear();lift_=0;}
        if(hover_mix_.size()!=layout_.size())hover_mix_.assign(layout_.size(),0.0f);
        for(int i=0;i<n;++i){const auto& t=layout_[i].rect;auto& v=shown_[i];if(!motion_)v=t;else{v.w=t.w;v.h=t.h;}}
    }else{shown_.clear();hover_mix_.clear();}
    FinishLayout(y+rowHeight+(view_==View::Reading&&presenting_?absolute_.h*.5f:16.0f));
}
void PdfCanvas::LayoutSpread(){
    // 每行一对页面；两页共用同一缩放比例（按较大页面计算），行内垂直居中、整体水平居中。
    constexpr float gap=12;
    float y=PadTop();const size_t n=pages_.size();size_t i=0;int row=0;
    const float fitWidth=std::max(60.0f,(absolute_.w-(presenting_?8.0f:52.0f)-gap)/2);
    while(i<n){
        const size_t count=(spread_cover_&&i==0)||i+1>=n?1:2;
        float maxW=1,maxH=1;
        for(size_t k=i;k<i+count;++k){maxW=std::max(maxW,DisplayWidth(pages_[k]));maxH=std::max(maxH,DisplayHeight(pages_[k]));}
        float scale=fitWidth/maxW;
        if(fit_==Fit::Page)scale=std::min(scale,std::max(40.0f,absolute_.h-(presenting_?8.0f:40.0f))/maxH);
        if(fit_==Fit::Width)scale=std::min(scale,8.0f*(96.0f/72.0f));
        if(fit_==Fit::None)scale=zoom_*(96.0f/72.0f);
        const float cellW=maxW*scale,rowH=maxH*scale;
        const float rowW=cellW*2+gap;
        const float left=std::max(12.0f,(absolute_.w-rowW)/2)-horizontal_;
        for(size_t k=i;k<i+count;++k){
            const float w=DisplayWidth(pages_[k])*scale,h=DisplayHeight(pages_[k])*scale;
            // 单页行：封面放在右侧（与书籍一致），末页落单时放左侧。
            const bool right=count==2?k==i+1:(spread_cover_&&i==0);
            const float x=left+(right?cellW+gap:0)+(right?0:cellW-w);
            layout_.push_back({{x,y+(rowH-h)/2,w,h},scale,std::to_wstring(k+1),row});
        }
        content_width_=std::max(content_width_,rowW+24);
        y+=rowH+24;i+=count;++row;
    }
    FinishLayout(presenting_?y-24+absolute_.h*.5f:y);
}
void PdfCanvas::FinishLayout(float height){
    content_height_=layout_.empty()?0:height;
    scroll_=std::clamp(scroll_,0.0f,std::max(0.0f,content_height_-absolute_.h));
    target_scroll_=std::clamp(target_scroll_,0.0f,std::max(0.0f,content_height_-absolute_.h));
    PagedClamp();
    RequestVisible();Invalidate();if(view_==View::Reading&&zoom_changed)zoom_changed(ActualZoom());
}
void PdfCanvas::Spread(bool on,bool cover){
    if(on==spread_&&cover==spread_cover_)return;
    const int page=current_page_;spread_=on;spread_cover_=cover;horizontal_=0;
    LayoutPages();if(view_==View::Reading){GoTo(page);if(page_changed)page_changed(current_page_);}
}
void PdfCanvas::RequestVisible(){
    visible_.clear();if(!request_tile||absolute_.h<=0)return;
    std::set<TileKey> wanted;
    // 新请求先收集，再按与视口中心的距离排序提交：屏幕中央的瓦片最先渲染，预取边距最后。
    std::vector<std::pair<float,TileRequest>> fresh;
    const float centerY=scroll_+absolute_.h*.5f,centerX=absolute_.w*.5f;
    const int pagedRow=PagedActive()?layout_[std::clamp(current_page_,0,static_cast<int>(layout_.size())-1)].row:-1;
    for(size_t i=0;i<layout_.size();++i){
        const auto& l=layout_[i];
        if(pagedRow>=0&&l.row!=pagedRow)continue;
        auto inView=[&](const lumen::Rect& r){return r.Bottom()>=scroll_-180&&r.y<=scroll_+absolute_.h+180;};
        // 网格中页面可能正在移动：目标位置或当前显示位置任一在屏即需要；浮起的页面始终需要。
        if(Grid()?!(inView(l.rect)||inView(PageRect(static_cast<int>(i)))||Lifted(static_cast<int>(i))):!inView(l.rect))continue;
        visible_.push_back(static_cast<int>(i));
        if(view_==View::Reading&&request_links&&!links_.contains(static_cast<int>(i))&&links_pending_.insert(static_cast<int>(i)).second)request_links(static_cast<int>(i),generation_);
        const float scale=TileScale(static_cast<int>(i));const int hidden=Hidden(static_cast<int>(i));
        const float step=768/scale;
        const auto& page=pages_[i];
        // 视口（上下各预取 180）与页面显示框的交集，反算回页面坐标（视图旋转时横纵互换）。
        float top=0,bottom=page.height,left=0,right=page.width;
        if(!Grid()){
            const float x0=std::max(0.0f,l.rect.x),x1=std::min(absolute_.w,l.rect.Right()),y0=std::max(scroll_-180,l.rect.y),y1=std::min(scroll_+absolute_.h+180,l.rect.Bottom());
            if(x1<=x0||y1<=y0){top=bottom=left=right=0;}
            else{
                const auto a=PagePoint(static_cast<int>(i),{x0,y0-scroll_}),b=PagePoint(static_cast<int>(i),{x1,y1-scroll_});
                left=std::clamp(std::min(a.x,b.x)-page.originX,0.0f,page.width);right=std::clamp(std::max(a.x,b.x)-page.originX,0.0f,page.width);
                top=std::clamp(std::min(a.y,b.y)-page.originY,0.0f,page.height);bottom=std::clamp(std::max(a.y,b.y)-page.originY,0.0f,page.height);
            }
        }
        const int startY=std::max(0,static_cast<int>(top/step));
        const int endY=bottom<=top?startY:std::max(startY,static_cast<int>(std::ceil(bottom/step)));
        const int columns=std::max(1,static_cast<int>(std::ceil(page.width/step)));
        const int startX=std::max(0,static_cast<int>(left/step));const int endX=right<=left?startX:std::min(columns,static_cast<int>(std::ceil(right/step)));for(int y=startY;y<endY;++y)for(int x=startX;x<endX;++x){
            TileKey key{static_cast<int>(i),static_cast<int>(std::lround(scale*100)),x,y,hidden,generation_,view_==View::Reading&&!hide_annotations_};wanted.insert(key);
            if(tiles_.contains(key)||pending_.contains(key))continue;
            Rect clip{page.originX+x*step,page.originY+y*step,std::min(step,page.width-x*step),std::min(step,page.height-y*step)};
            if(clip.w<=0||clip.h<=0)continue;
            auto token=std::make_shared<std::atomic_bool>(false);pending_.emplace(key,token);
            const auto shown=ScreenRect(static_cast<int>(i),clip);
            const float ty=shown.y-absolute_.y+scroll_+shown.h*.5f,tx=shown.x-absolute_.x+shown.w*.5f;
            const bool onScreen=ty+shown.h*.5f>=scroll_&&ty-shown.h*.5f<=scroll_+absolute_.h;
            fresh.push_back({(onScreen?0.0f:1e6f)+std::abs(ty-centerY)+std::abs(tx-centerX)*.5f,TileRequest{key,scale,clip,generation_,token,hidden,tone_}});
        }
    }
    if(tool_==Tool::Select&&selecting_text_&&!text_selection_.spans.empty())RequestSpans();
    std::stable_sort(fresh.begin(),fresh.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    for(auto& item:fresh)request_tile(std::move(item.second));
    for(auto it=pending_.begin();it!=pending_.end();){if(!wanted.contains(it->first)){it->second->store(true);it=pending_.erase(it);}else ++it;}wanted_=wanted;
    TrimTiles();RequestLens();
}
void PdfCanvas::TrimTiles(){
    // 先丢不可见页面的多余瓦片；可见页面的旧版本瓦片仍在垫底，只有总量过大才放弃。
    if(tiles_.size()<=24)return;
    for(auto it=tiles_.begin();it!=tiles_.end()&&tiles_.size()>16;){
        const bool visible=std::find(visible_.begin(),visible_.end(),it->first.page)!=visible_.end();
        if(!wanted_.contains(it->first)&&!visible)it=tiles_.erase(it);else ++it;
    }
    for(auto it=tiles_.begin();it!=tiles_.end()&&tiles_.size()>48;){
        if(!wanted_.contains(it->first))it=tiles_.erase(it);else ++it;
    }
}
void PdfCanvas::AcceptTile(TileRequest request,Bitmap bitmap){
    if(request.generation!=generation_||(request.cancelled&&request.cancelled->load()))return;
    if(request.key.x<0){   // 放大镜的高分辨率局部图（x/y = -1，不进普通瓦片表）
        if(!lens_pending_||lens_pending_->cancelled!=request.cancelled||request.tone!=tone_)return;
        lens_pending_.reset();Tile tile;tile.request=request;tile.pixels=std::move(bitmap);lens_tile_=std::move(tile);Invalidate();return;
    }
    pending_.erase(request.key);
    if(request.tone!=tone_)return;Tile tile;tile.request=request;tile.pixels=std::move(bitmap);
    tiles_.insert_or_assign(request.key,std::move(tile));TrimTiles();
    // 落下后，新版本的完整瓦片把该页可见区域全部覆盖时，预览完成使命。
    if(settling_&&request.key.page==settling_->page&&request.key.hidden<0&&request.generation>settling_->generation){
        bool complete=true;
        for(const auto& k:wanted_)if(k.page==settling_->page&&!tiles_.contains(k)){complete=false;break;}
        if(complete)settling_.reset();
    }
    Invalidate();
}
void PdfCanvas::AcceptTextLayer(int page,uint64_t generation,const std::vector<Annotation>& values){
    if(generation!=generation_||page<0||page>=static_cast<int>(pages_.size()))return;
    auto found=text_pages_.find(page);if(found!=text_pages_.end()&&found->second.generation==generation)return;
    TextPage layer;layer.generation=generation;lumen::TextLayout measure;
    for(const auto& a:values){
        if(a.type!=Tool::Text||!a.lumenText||a.rotation!=0)continue;
        TextVisual item;item.annotation=a;item.annotation.bounds=FitTextBounds(a,pages_[page],measure);
        item.display=item.annotation.bounds;layer.items.push_back(std::move(item));
    }
    text_pages_.insert_or_assign(page,std::move(layer));
    Invalidate();
}
void PdfCanvas::PrepareTextLayers(lumen::Painter& painter){
    if(view_!=View::Reading)return;
    // 与 DrawTextLayer 相同：在未旋转的页面几何中排版（绘制时整体旋转）。
    struct Unrotated{bool& flag;explicit Unrotated(bool& f):flag(f){flag=true;}~Unrotated(){flag=false;}} unrotated(unrotated_);
    for(int page:visible_){
        auto found=text_pages_.find(page);if(found==text_pages_.end())continue;
        const float scale=layout_[page].scale;
        for(auto& item:found->second.items){
            if(draft_&&draft_page_==page&&draft_->id==item.annotation.id)continue;
            const Annotation* a=&item.annotation;
            if(moving_&&down_page_==page&&moving_->id==a->id)a=&*moving_;
            else if(page==annotations_page_){
                auto live=std::find_if(annotations_.begin(),annotations_.end(),[&](const auto& v){return v.id==a->id;});
                if(live!=annotations_.end())a=&*live;
            }
            item.display=a->bounds;
            const auto box=ScreenRect(page,item.display);
            item.layout.Layout(a->text,AnnotationTypography(*a,scale),std::max(.5f,(item.display.w-4.0f)*scale),true);
            item.layout.Prepare(painter,{box.x+2.0f*scale,box.y+2.0f*scale},C(ToneColor(tone_,a->textFormat.color),a->opacity),C(ToneColor(tone_,0xffffff)));
        }
    }
}
void PdfCanvas::DrawTextLayer(lumen::Painter& painter,int page){
    if(hide_annotations_)return;
    auto found=text_pages_.find(page);if(found==text_pages_.end())return;
    const auto& info=pages_[page];const float scale=layout_[page].scale;
    painter.PushClip(ScreenRect(page,{info.originX,info.originY,info.width,info.height}));
    for(auto& item:found->second.items){
        if(draft_&&draft_page_==page&&draft_->id==item.annotation.id)continue;
        const auto box=ScreenRect(page,item.display);
        item.layout.Draw(painter,{box.x+2.0f*scale,box.y+2.0f*scale},C(ToneColor(tone_,item.annotation.textFormat.color),item.annotation.opacity),C(ToneColor(tone_,0xffffff)));
    }
    painter.PopClip();
}

void PdfCanvas::FailTile(TileKey key){if(key.x<0){lens_pending_.reset();return;}pending_.erase(key);}
void PdfCanvas::AcceptSprite(SpriteRequest request,Sprite sprite){
    if(!ghost_pending_||ghost_pending_->id!=request.id||ghost_pending_->page!=request.page||request.generation!=generation_)return;
    ghost_pending_.reset();Ghost ghost;ghost.request=request;ghost.sprite=std::move(sprite);ghost_=std::move(ghost);Invalidate();
}
void PdfCanvas::BeginGhost(int page,const Annotation& a){
    if(a.type==Tool::Text&&a.lumenText)return;
    if(!request_sprite||page<0||page>=static_cast<int>(layout_.size()))return;
    const float scale=TileScale(page);
    // 同一文档版本内位图与 from 始终成对，可跨多次拖动复用；版本变化后重新请求。
    if(ghost_&&ghost_->request.id==a.id&&ghost_->request.page==page&&ghost_->request.generation==generation_&&std::abs(ghost_->request.scale-scale)<.001f)return;
    if(ghost_pending_&&ghost_pending_->id==a.id&&ghost_pending_->page==page&&ghost_pending_->generation==generation_&&std::abs(ghost_pending_->scale-scale)<.001f)return;
    ghost_.reset();ghost_pending_=SpriteRequest{page,a.id,scale,generation_,a.bounds,a.type};request_sprite(*ghost_pending_);
}
void PdfCanvas::Prepare(lumen::Painter& p){
    if(device_!=p.DeviceIdentity()){if(device_){tiles_.clear();DropLens();RequestVisible();}device_=p.DeviceIdentity();}
    if(std::abs(dpi_-p.Scale())>.01f){dpi_=p.Scale();RequestVisible();PlaceEditor();}
    for(auto& [key,t]:tiles_){
        (void)key;if(!t.bitmap&&!t.pixels.bgra.empty())
            {t.bitmap.Attach(p.CreateBitmapBgra(t.pixels.width,t.pixels.height,t.pixels.bgra.data(),t.pixels.stride));if(t.bitmap){t.pixels.bgra.clear();t.pixels.bgra.shrink_to_fit();}}
    }
    if(lens_tile_&&!lens_tile_->bitmap&&!lens_tile_->pixels.bgra.empty()){
        auto& t=*lens_tile_;t.bitmap.Attach(p.CreateBitmapBgra(t.pixels.width,t.pixels.height,t.pixels.bgra.data(),t.pixels.stride));
        if(t.bitmap){t.pixels.bgra.clear();t.pixels.bgra.shrink_to_fit();}
    }
    if(ghost_&&!ghost_->bitmap&&!ghost_->sprite.pixels.bgra.empty()){
        auto& s=ghost_->sprite.pixels;ghost_->bitmap.Attach(p.CreateBitmapBgra(s.width,s.height,s.bgra.data(),s.stride));
        if(ghost_->bitmap){s.bgra.clear();s.bgra.shrink_to_fit();}
    }
    if(device_!=p.DeviceIdentity()&&ghost_)ghost_->bitmap.Reset();
    PrepareTextLayers(p);
}
lumen::Point PdfCanvas::Local(int page,Point p)const{
    const auto& l=layout_[page];const auto& info=pages_[page];
    const float u=(p.x-info.originX)*l.scale,v=(p.y-info.originY)*l.scale,w=info.width*l.scale,h=info.height*l.scale;
    switch(Rot()){case 90:return {h-v,u};case 180:return {w-u,h-v};case 270:return {v,w-u};default:return {u,v};}
}
lumen::Rect PdfCanvas::ScreenRect(int page,Rect r)const{
    const auto& l=layout_[page];const auto& info=pages_[page];auto box=PageRect(page);
    const int rot=Rot();
    if(rot&&unrotated_){   // 已压入绕页面中心的旋转：按未旋转的页面框（同一中心）映射
        const float w=info.width*l.scale,h=info.height*l.scale;box={box.x+box.w*.5f-w*.5f,box.y+box.h*.5f-h*.5f,w,h};
    }
    if(!rot||unrotated_)return {absolute_.x+box.x+(r.x-info.originX)*l.scale,absolute_.y+box.y-scroll_+(r.y-info.originY)*l.scale,r.w*l.scale,r.h*l.scale};
    const auto a=Local(page,{r.x,r.y}),b=Local(page,{r.x+r.w,r.y+r.h});
    return {absolute_.x+box.x+std::min(a.x,b.x),absolute_.y+box.y-scroll_+std::min(a.y,b.y),std::abs(b.x-a.x),std::abs(b.y-a.y)};
}
void PdfCanvas::Draw(lumen::Painter& p,const lumen::Theme&){
    if(settling_&&GetTickCount64()>settling_->deadline){settling_.reset();const_cast<PdfCanvas*>(this)->RequestVisible();}
    p.FillRect(absolute_,C(presenting_&&view_==View::Reading?0x000000:view_==View::Thumbnails?0x101010:0x191919));p.PushClip(absolute_);
    bool loading=false;
    for(int page:visible_){
        if(Grid()&&Lifted(page))continue;
        const auto r=PageRect(page).Offset(absolute_.x,absolute_.y-scroll_);
        if(!presenting_)DrawPageFrame(p,r,page);
        p.FillRect(r,C(ToneColor(tone_,0xffffff)));
        // 视图旋转：页面内容（瓦片、文字层、拖动预览）在未旋转的几何中绘制，再绕页面中心整体旋转；瓦片无需重渲染。
        const int rot=Rot();
        if(rot){p.PushRotate({r.x+r.w*.5f,r.y+r.h*.5f},static_cast<float>(rot));unrotated_=true;}
        DrawPageTiles(p,page,loading);
        if(rot){unrotated_=false;p.PopTransform();}
        if(Grid())DrawGridChrome(p,page,r,false);
    }
    if(Grid()){
        // 拖动中：预览顺序里的空位用虚线框标出，松手后页面会飞入这里。
        if(page_drag_.active)for(int page:page_drag_.group){
            if(page<0||page>=static_cast<int>(layout_.size()))continue;
            const auto slot=layout_[page].rect.Offset(absolute_.x,absolute_.y-scroll_).Inset(-5,-5);
            if(slot.Bottom()<absolute_.y||slot.y>absolute_.Bottom())continue;
            p.FillRoundedRect(slot,6,C(0xffffff,.045f));
            p.StrokeDashedRoundedRect(slot,6,C(0xffffff,page==page_drag_.grabbed?.55f:.28f),1.25f);
        }
        // 浮起层：其余选中页叠在下面，抓住的那页在最上。放大、阴影随 lift_ 与位移同一缓动进出。
        std::vector<int> stack;
        for(auto it=lifted_.rbegin();it!=lifted_.rend();++it)if(*it!=page_drag_.grabbed)stack.push_back(*it);
        if(std::find(lifted_.begin(),lifted_.end(),page_drag_.grabbed)!=lifted_.end())stack.push_back(page_drag_.grabbed);
        for(int page:stack){
            if(page<0||page>=static_cast<int>(layout_.size()))continue;
            const auto r=PageRect(page).Offset(absolute_.x,absolute_.y-scroll_);
            const float k=1.0f+.035f*lift_;
            p.PushScale({r.x+r.w*.5f,r.y+r.h*.5f},k,k);
            p.DrawGlow(r,3,C(0x000000,.75f*lift_),1.1f);
            DrawPageFrame(p,r,page);
            p.FillRect(r,C(ToneColor(tone_,0xffffff)));
            DrawPageTiles(p,page,loading);
            DrawGridChrome(p,page,r,true);
            p.PopTransform();
        }
        if(page_drag_.active&&page_drag_.group.size()>1&&page_drag_.grabbed>=0&&page_drag_.grabbed<static_cast<int>(layout_.size())){
            const auto r=PageRect(page_drag_.grabbed).Offset(absolute_.x,absolute_.y-scroll_);
            const std::wstring count=std::to_wstring(page_drag_.group.size());
            const float w=std::max(24.0f,10.0f+8.0f*static_cast<float>(count.size()));
            const lumen::Rect badge{r.Right()-w*.5f,r.y-11,w,22};
            p.FillRoundedRect(badge,11,C(0xf2f2f2));
            p.DrawText(count,badge,lumen::TextRole::CaptionStrong,C(0x111111),lumen::Align::Center);
        }
    }
    if(loading!=loading_){loading_=loading;if(loading_)Animate();}
    if(view_==View::Reading){
        // 全部命中淡黄色，当前命中加深并描边。结果按页有序，只画可见页。
        for(size_t i=0;i<search_hits_.size();++i){
            const auto& hit=search_hits_[i];
            if(hit.page<0||hit.page>=static_cast<int>(layout_.size())||std::find(visible_.begin(),visible_.end(),hit.page)==visible_.end())continue;
            const auto box=ScreenRect(hit.page,hit.bounds).Inset(-1.5f,-1);
            if(static_cast<int>(i)==search_current_){p.FillRoundedRect(box,2,C(0xffb020,.45f));p.StrokeRoundedRect(box,2,C(0xe08a00),1.5f);}
            else p.FillRoundedRect(box,2,C(0xf5d443,.30f));
        }
        // 朗读跟读：当前句淡绿底 + 下划线。
        if(speech_page_>=0&&speech_page_<static_cast<int>(layout_.size())&&std::find(visible_.begin(),visible_.end(),speech_page_)!=visible_.end()){
            for(const auto& b:speech_boxes_){
                const auto box=ScreenRect(speech_page_,b).Inset(-2,-1.5f);
                p.FillRoundedRect(box,3,C(0x2fbf71,.22f));
                p.DrawLine({box.x+1,box.Bottom()},{box.Right()-1,box.Bottom()},C(0x1f9d57,.9f),2.0f);
            }
        }
        // 可填写的表单字段：淡蓝底；悬停加深描边；必填字段红色细边。
        if(highlight_fields_&&tool_==Tool::Select&&!presenting_){
            for(int page:visible_){
                auto found=links_.find(page);if(found==links_.end()||found->second.generation!=generation_)continue;
                for(const auto& f:found->second.fields){
                    const bool clickable=f.Fillable()||(f.type==FieldType::Signature&&!f.signedField&&!f.readOnly);
                    if(!clickable)continue;
                    const auto box=ScreenRect(page,f.bounds);
                    const bool hover=page==hover_field_page_&&f.id==hover_field_;
                    p.FillRect(box,C(0x4f8ff7,hover?.22f:.12f));
                    if(hover)p.StrokeRoundedRect(box.Inset(-.5f,-.5f),1.5f,C(0x3676dd,.9f),1.25f);
                    else if(f.required)p.StrokeRoundedRect(box.Inset(-.5f,-.5f),1.5f,C(0xd33445,.55f),1);
                }
            }
        }
        // 涂黑标记：半透明黑块 + 红框（预览，下方内容仍可辨认）；悬停加深并显示 ×（单击删除）。
        if(!presenting_&&(redact_mode_||!redact_marks_.empty())){
            for(size_t i=0;i<redact_marks_.size();++i){
                const auto& m=redact_marks_[i];
                if(std::find(visible_.begin(),visible_.end(),m.page)==visible_.end())continue;
                const auto box=ScreenRect(m.page,m.bounds);const bool hover=static_cast<int>(i)==redact_hover_;
                p.FillRect(box,C(0x000000,hover?.78f:.58f));
                p.StrokeRoundedRect(box,1,C(0xe0243a,hover?1.0f:.85f),hover?2.0f:1.25f);
                if(hover&&box.w>14&&box.h>14){
                    const float cx=box.x+box.w/2,cy=box.y+box.h/2,k=std::min(6.0f,std::min(box.w,box.h)/4);
                    p.DrawLine({cx-k,cy-k},{cx+k,cy+k},C(0xffffff,.9f),1.5f);p.DrawLine({cx-k,cy+k},{cx+k,cy-k},C(0xffffff,.9f),1.5f);
                }
            }
            if(redact_drag_){
                const auto& g=*redact_drag_;
                const Rect r{std::min(g.start.x,g.current.x),std::min(g.start.y,g.current.y),std::abs(g.current.x-g.start.x),std::abs(g.current.y-g.start.y)};
                const auto box=ScreenRect(g.page,r);
                p.FillRect(box,C(0x000000,.35f));p.StrokeRoundedRect(box,1,C(0xe0243a,.95f),1.25f);
            }
        }
        if(hover_link_>=0&&tool_==Tool::Select&&!dragging_){
            auto found=links_.find(hover_link_page_);
            if(found!=links_.end()&&hover_link_<static_cast<int>(found->second.links.size())){
                const auto box=ScreenRect(hover_link_page_,found->second.links[hover_link_].bounds).Inset(-2,-1);
                p.FillRoundedRect(box,3,C(0x3676dd,.10f));
                p.DrawLine({box.x+2,box.Bottom()},{box.Right()-2,box.Bottom()},C(0x3676dd,.75f),1.25f);
            }
        }
        for(const auto& span:text_selection_.spans){
            if(span.page<0||span.page>=static_cast<int>(layout_.size())||std::find(visible_.begin(),visible_.end(),span.page)==visible_.end())continue;
            for(const auto& q:span.quads)p.FillRect(ScreenRect(span.page,PointsRect(q.ul,q.lr)),C(0x3a86ff,.30f));
        }
        if(search_hits_.empty()&&search_&&search_->page>=0&&search_->page<static_cast<int>(layout_.size()))p.FillRect(ScreenRect(search_->page,search_->bounds),C(0xe5cb60,.35f));
        if(!hide_annotations_&&!draft_&&annotations_page_>=0&&annotations_page_<static_cast<int>(layout_.size())){
            for(const auto& a:annotations_){
                if(a.id==selected_annotation_)DrawSelection(p,moving_?*moving_:a);
                else if(a.id==hover_annotation_)DrawSelection(p,a,true);
            }
        }
        if(dragging_&&!moving_&&down_page_>=0){
            Annotation preview;preview.type=tool_;preview.bounds=Between(down_point_,last_point_);
            preview.style=drawing_style_;preview.opacity=drawing_opacity_;
            auto pageRect=ScreenRect(down_page_,{pages_[down_page_].originX,pages_[down_page_].originY,pages_[down_page_].width,pages_[down_page_].height});
            p.PushClip(pageRect);
            if(tool_==Tool::Ink){preview.points=SmoothInk(stroke_);DrawVector(p,down_page_,preview);}
            else if(IsLineTool(tool_)){preview.points={down_point_,last_point_};DrawVector(p,down_page_,preview);}
            else if(IsBoxShape(tool_))DrawVector(p,down_page_,preview);
            else if(IsMarkupTool(tool_)){
                if(area_highlight_){const auto& r=preview.bounds;preview.quads={{{r.x,r.y},{r.x+r.w,r.y},{r.x,r.y+r.h},{r.x+r.w,r.y+r.h}}};}
                else preview.quads=highlight_preview_;
                DrawVector(p,down_page_,preview);
            }else if(tool_!=Tool::Select||area_select_){
                auto r=ScreenRect(down_page_,preview.bounds);
                p.FillRect(r,C(0x3676dd,.08f));p.StrokeDashedRoundedRect(r,1,C(0x3676dd),1);
            }
            p.PopClip();
        }
    }
    if(draft_&&draft_page_>=0)DrawSelection(p,moving_?*moving_:*draft_);
    if(zoom_drag_&&view_==View::Reading){
        const auto r=zoom_drag_->Offset(absolute_.x,absolute_.y);
        p.FillRect(r,C(0x3676dd,.10f));p.StrokeDashedRoundedRect(r,1,C(0x5b9bff),1.5f);
        if(r.w>=8&&r.h>=8&&!layout_.empty()){
            const float target=std::clamp(ActualZoom()*std::min((absolute_.w-40)/r.w,(absolute_.h-40)/r.h),.1f,8.0f);
            const auto label=std::to_wstring(static_cast<int>(std::lround(target*100)))+L"%";
            const lumen::Rect tag{r.x,r.y-24>absolute_.y?r.y-24:r.Bottom()+4,64,20};
            p.FillRoundedRect(tag,4,C(0x3676dd,.92f));p.DrawText(label,tag,lumen::TextRole::CaptionStrong,C(0xffffff),lumen::Align::Center);
        }
    }
    DrawLens(p);
    if(content_height_>absolute_.h&&!presenting_){
        const auto thumb=ScrollThumb();
        if(scrollbar_hover_||scrollbar_drag_)p.FillRoundedRect({absolute_.Right()-16,absolute_.y+3,13,absolute_.h-6},6,C(0x292929,.95f));
        p.FillRoundedRect(thumb,thumb.w/2,C(scrollbar_drag_?0xe5e5e5:scrollbar_hover_?0xb5b5b5:0x777777));
    }
    p.PopClip();
}
void PdfCanvas::DrawPageTiles(lumen::Painter& p,int page,bool& loading){
    const int scaleKey=static_cast<int>(std::lround(TileScale(page)*100));int shown=Hidden(page);
    // 拖动预览要等“预览位图 + 隐藏原位内容的整套瓦片”都到齐才切换，之前继续显示原图，避免文字消失或重影。
    bool ready=false;
    if(shown>=0&&!draft_){
        ready=ghost_&&ghost_->bitmap&&ghost_->request.page==page&&ghost_->request.id==shown;
        for(const auto& k:wanted_)if(ready&&k.page==page&&k.hidden==shown&&!tiles_.contains(k))ready=false;
        if(!ready)shown=-1;
    }
    const int settled=settling_&&settling_->page==page?settling_->id:-1;
    // 落下后到新版本瓦片到达前，已隐藏该批注的瓦片 + 预览位图才是正确画面；本版本内 Hidden 也返回它。
    if(settled>=0&&shown<0)shown=settled;
    // 每个格子选一张：当前版本且隐藏状态一致的优先；落下后其次是隐藏了该批注的旧瓦片（配合预览位图），
    // 再次是隐藏状态一致的旧版本，最后才用任意旧瓦片垫底，避免闪白。
    // 当前比例一张瓦片都没有（缩放或布局刚变）时，用最接近比例的旧瓦片按新几何拉伸垫底，避免整页闪白。
    int useScale=scaleKey;int nearest=INT_MAX;
    for(const auto& [key,t]:tiles_){
        if(key.page!=page||key.textLayer!=(view_==View::Reading&&!hide_annotations_)||!t.bitmap)continue;
        if(key.scale==scaleKey){useScale=scaleKey;break;}
        if(std::abs(key.scale-scaleKey)<nearest){nearest=std::abs(key.scale-scaleKey);useScale=key.scale;}
    }
    std::map<std::pair<int,int>,std::pair<int,const Tile*>> chosen;
    for(const auto& [key,t]:tiles_){
        if(key.page!=page||key.textLayer!=(view_==View::Reading&&!hide_annotations_)||!t.bitmap||key.scale!=useScale)continue;
        const int rank=key.generation==generation_&&key.hidden==shown?0:key.hidden==shown?1:key.hidden==settled?2:3;
        auto [it,inserted]=chosen.try_emplace({key.x,key.y},rank,&t);
        if(!inserted&&rank<it->second.first)it->second={rank,&t};
    }
    for(const auto& [cell,pick]:chosen){(void)cell;p.DrawBitmap(pick.second->bitmap.Get(),ScreenRect(page,pick.second->request.clip),true);}
    // 该页一张瓦片都还没到：画加载指示，而不是一张空白纸。
    if(chosen.empty()){
        bool waiting=false;for(const auto& [key,token]:pending_){(void)token;if(key.page==page){waiting=true;break;}}
        if(waiting){DrawLoading(p,PageRect(page).Offset(absolute_.x,absolute_.y-scroll_));loading=true;}
    }
    // 落下后只有当画出的瓦片全部隐藏了该批注时才叠画预览；否则原位文字尚在，叠画会成重影。
    bool hiddenEverywhere=settled>=0&&!chosen.empty();
    for(const auto& [cell,pick]:chosen){(void)cell;if(pick.second->request.key.hidden!=settled)hiddenEverywhere=false;}
    if(ready||hiddenEverywhere)DrawGhost(p,page);
    if(view_==View::Reading)DrawTextLayer(p,page);
}
void PdfCanvas::DrawGridChrome(lumen::Painter& p,int page,const lumen::Rect& r,bool lifted)const{
    const bool selected=page==current_page_||std::find(selected_pages_.begin(),selected_pages_.end(),page)!=selected_pages_.end();
    const float hover=page<static_cast<int>(hover_mix_.size())?hover_mix_[page]:0.0f;
    if(selected||lifted)p.StrokeRoundedRect(r.Inset(-5,-5),4,C(0xf2f2f2),2.0f);
    else if(hover>.01f)p.StrokeRoundedRect(r.Inset(-5,-5),4,C(0xffffff,.35f*hover),1.0f);
    if(lifted)return;
    const lumen::Rect badge{r.x+r.w*.5f-18,r.Bottom()+7,36,20};
    if(selected)p.FillRoundedRect(badge,10,C(0xf2f2f2));
    p.DrawText(layout_[page].label,{r.x,r.Bottom()+7,r.w,20},lumen::TextRole::Caption,selected?C(0x111111):hover>.5f?C(0xeeeeee):C(0x9a9a9a),lumen::Align::Center);
}
void PdfCanvas::DrawPageFrame(lumen::Painter& p,const lumen::Rect& r,int page)const{
    // 纸张落在深色底上：柔和外晕 + 一圈细边，阅读页更醒目，当前页（阅读视图）略亮。
    const bool reading=view_==View::Reading;
    const float spread=reading?.55f:.3f;
    p.DrawGlow(r,1.5f,C(0xffffff,reading&&page==current_page_&&visible_.size()>1?.07f:.045f),spread);
    p.FillRect(r.Inset(-1,-1),C(0x000000,.55f));
}
void PdfCanvas::DrawLoading(lumen::Painter& p,const lumen::Rect& r)const{
    const float radius=std::clamp(std::min(r.w,r.h)*.06f,7.0f,18.0f);
    const float cy=std::clamp(absolute_.y+absolute_.h*.5f,r.y+radius*2,std::max(r.y+radius*2,r.Bottom()-radius*2));
    const lumen::Point center{r.x+r.w*.5f,std::min(cy,r.y+r.h*.5f)};
    const float width=std::max(2.0f,radius*.22f);
    const unsigned ink=tone_==PageTone::Night?0xffffff:0x000000;
    p.DrawArc(center,radius,0,360,C(ink,.08f),width);
    p.DrawArc(center,radius,spin_,100,C(ink,.38f),width);
}
void PdfCanvas::GoTo(int page){
    if(page<0||page>=static_cast<int>(pages_.size()))return;
    if(pending_view_){pending_view_->page=page;pending_view_->offset=0;current_page_=page;}
    if(page>=static_cast<int>(layout_.size()))return;
    if(view_==View::Thumbnails){
        // 侧栏只在目标页不完整可见时滚动最短距离：单击缩略图不再把它甩到顶部，拖动也不会被打断。
        // 近距离与其它滚动同一缓动；远距离（如打开文档恢复位置）直接到位，避免一路请求中间页。
        current_page_=page;const auto& r=layout_[page].rect;
        float target=target_scroll_;
        if(r.y-12<target)target=r.y-12;else if(r.Bottom()+36>target+absolute_.h)target=r.Bottom()+36-absolute_.h;
        target=std::clamp(target,0.0f,std::max(0.0f,content_height_-absolute_.h));
        if(std::abs(target-scroll_)>absolute_.h*2){scroll_=target_scroll_=target;RequestVisible();}
        else if(target!=target_scroll_){target_scroll_=target;Animate();}
        Invalidate();return;
    }
    current_page_=page;scroll_=std::clamp(layout_[page].rect.y-16,0.0f,std::max(0.0f,content_height_-absolute_.h));target_scroll_=scroll_;
    if(PagedActive()){float lo,hi;RowRange(layout_[page].row,lo,hi);scroll_=target_scroll_=lo;}
    RequestVisible();Invalidate();
}
float PdfCanvas::ActualZoom()const{
    return current_page_>=0&&current_page_<static_cast<int>(layout_.size())?layout_[current_page_].scale*(72.0f/96.0f):zoom_;
}
void PdfCanvas::ZoomTo(float actual){
    if(view_!=View::Reading||pages_.empty())return;
    const int page=draft_?draft_page_:current_page_;
    // 画布尚未排版（隐藏或宽高为 0 时 layout_ 为空）：只记下缩放，等排版时生效。曾导致偶发空指针崩溃。
    if(page<0||page>=static_cast<int>(layout_.size())){zoom_=std::clamp(actual,.1f,8.0f);fit_=Fit::None;LayoutPages();if(zoom_changed)zoom_changed(ActualZoom());return;}
    const Point anchor=draft_?Point{draft_->bounds.x+draft_->bounds.w*.5f,draft_->bounds.y}:PagePoint(page,{absolute_.w*.5f,absolute_.h*.5f});
    const auto before=ScreenRect(page,{anchor.x,anchor.y,0,0});
    zoom_=std::clamp(actual,.1f,8.0f);fit_=Fit::None;LayoutPages();
    const auto after=ScreenRect(page,{anchor.x,anchor.y,0,0});
    scroll_=target_scroll_=std::clamp(scroll_+after.y-before.y,0.0f,std::max(0.0f,content_height_-absolute_.h));
    UpdateViewport();if(draft_)FocusText(draft_page_,draft_->bounds);if(zoom_changed)zoom_changed(ActualZoom());
}
void PdfCanvas::Zoom(float factor){if(view_==View::Pages){GridZoom(factor);return;}ZoomTo(ActualZoom()*factor);}
void PdfCanvas::GridZoom(float factor){
    if(view_!=View::Pages||layout_.empty()||page_drag_.active)return;
    const float next=std::clamp(grid_zoom_*factor,.6f,2.5f);if(std::abs(next-grid_zoom_)<.001f)return;
    // 以视口上部的页面为锚点，缩放后它在屏幕上的位置不变。
    int anchor=-1;float offset=0;
    for(int i:visible_){const auto& r=layout_[i].rect;if(r.Bottom()>=scroll_+absolute_.h*.3f&&(anchor<0||r.y<layout_[anchor].rect.y||(r.y==layout_[anchor].rect.y&&r.x<layout_[anchor].rect.x))){anchor=i;offset=r.y-scroll_;}}
    grid_zoom_=next;motion_=false;lifted_.clear();lift_=0;LayoutPages();
    if(anchor>=0){scroll_=target_scroll_=std::clamp(layout_[anchor].rect.y-offset,0.0f,std::max(0.0f,content_height_-absolute_.h));UpdateViewport();}
}
void PdfCanvas::ActualSize(){ZoomTo(1.0f);}
void PdfCanvas::SpeechHighlight(int page,std::vector<Rect> boxes,bool follow){
    speech_page_=page<0||boxes.empty()||page>=static_cast<int>(pages_.size())?-1:page;
    speech_boxes_=speech_page_<0?std::vector<Rect>{}:std::move(boxes);Invalidate();
    if(!follow||speech_page_<0||view_!=View::Reading||speech_page_>=static_cast<int>(layout_.size())||absolute_.h<=1)return;
    float top=FLT_MAX,bottom=-FLT_MAX,first=FLT_MAX;
    for(const auto& b:speech_boxes_){const auto r=ScreenRect(speech_page_,b);top=std::min(top,r.y);bottom=std::max(bottom,r.Bottom());first=std::min(first,b.y);}
    if(top>=absolute_.y+8&&bottom<=absolute_.Bottom()-8)return;   // 已完整可见：不打扰阅读位置
    // 目标句放到视口上部（留出上一行的余量）；旋转 90/270° 时只能到页首。
    GoToPoint(speech_page_,first-pages_[speech_page_].height*.04f);
    if(fit_==Fit::Page||Rot()%180)GoTo(speech_page_);
    if(page_changed)page_changed(current_page_);
}
void PdfCanvas::ZoomBox(bool on){
    if(on&&(view_!=View::Reading||presenting_||pages_.empty()))on=false;
    if(!on)zoom_drag_.reset();
    if(zoom_box_==on)return;
    zoom_box_=on;Invalidate();if(zoom_box_changed)zoom_box_changed(on);
}
void PdfCanvas::ZoomToRect(lumen::Rect r){
    if(view_!=View::Reading||pages_.empty()||layout_.empty()||absolute_.w<=40||absolute_.h<=40)return;
    const lumen::Point center{r.x+r.w*.5f,r.y+r.h*.5f};
    int page=Hit(center);if(page<0)page=std::clamp(current_page_,0,static_cast<int>(layout_.size())-1);
    const Point anchor=PagePoint(page,center);
    zoom_back_.push_back({CurrentView(),horizontal_});if(zoom_back_.size()>20)zoom_back_.erase(zoom_back_.begin());
    // 小于 8 像素视为单击：以该点放大 2 倍；否则让框铺满视口（四周各留 20）。
    const float factor=r.w<8||r.h<8?2.0f:std::min((absolute_.w-40)/r.w,(absolute_.h-40)/r.h);
    zoom_=std::clamp(layout_[page].scale*(72.0f/96.0f)*factor,.1f,8.0f);fit_=Fit::None;LayoutPages();
    if(layout_.empty())return;
    // 先纵向居中（横向可滚范围取决于视口内的页面），再横向居中。映射经 ScreenRect，视图旋转时同样成立。
    auto at=[&]{return ScreenRect(page,{anchor.x,anchor.y,0,0});};
    scroll_=target_scroll_=std::clamp(scroll_+(at().y-absolute_.y)-absolute_.h*.5f,0.0f,std::max(0.0f,content_height_-absolute_.h));
    horizontal_=std::clamp(horizontal_+(at().x-absolute_.x)-absolute_.w*.5f,0.0f,HorizontalLimit());LayoutPages();
    current_page_=page;UpdateViewport();
    if(zoom_changed)zoom_changed(ActualZoom());
    if(box_zoomed)box_zoomed(ActualZoom());
}
bool PdfCanvas::ZoomBack(){
    if(zoom_back_.empty()||view_!=View::Reading||pages_.empty())return false;
    auto back=zoom_back_.back();zoom_back_.pop_back();
    back.view.rotation=rotation_;ApplyView(back.view);
    if(back.horizontal>0){horizontal_=std::clamp(back.horizontal,0.0f,HorizontalLimit());LayoutPages();UpdateViewport();}
    return true;
}
void PdfCanvas::Magnifier(bool on){
    if(lens_on_==on)return;
    lens_on_=on;if(!on){lens_inside_=false;DropLens();}else RequestLens();
    Invalidate();if(magnifier_changed)magnifier_changed(on);
}
void PdfCanvas::MagnifierFactor(float factor){
    factor=std::clamp(std::round(factor*2)/2,1.5f,6.0f);
    if(std::abs(factor-lens_factor_)<.01f)return;
    lens_factor_=factor;RequestLens();Invalidate();if(lens_on_&&magnifier_changed)magnifier_changed(true);
}
void PdfCanvas::DropLens(){
    if(lens_pending_){if(lens_pending_->cancelled)lens_pending_->cancelled->store(true);lens_pending_.reset();}
    lens_tile_.reset();
}
void PdfCanvas::RequestLens(){
    if(!lens_on_||!lens_inside_||!request_tile||view_!=View::Reading||presenting_||layout_.empty())return;
    const int page=Hit(lens_point_);if(page<0||page>=static_cast<int>(pages_.size()))return;
    const auto& info=pages_[page];
    // 放大镜显示的是指针周围 (镜框 / 倍率) 大小的区域；反算到页面坐标（旋转时横纵互换）并裁到页面内。
    const auto lens=LensRect();const float hw=lens.w*.5f/lens_factor_,hh=lens.h*.5f/lens_factor_;
    const auto a=PagePoint(page,{lens_point_.x-hw,lens_point_.y-hh}),b=PagePoint(page,{lens_point_.x+hw,lens_point_.y+hh});
    const float x0=std::max(info.originX,std::min(a.x,b.x)),x1=std::min(info.originX+info.width,std::max(a.x,b.x));
    const float y0=std::max(info.originY,std::min(a.y,b.y)),y1=std::min(info.originY+info.height,std::max(a.y,b.y));
    if(x1<=x0||y1<=y0)return;
    const float scale=std::min(24.0f,TileScale(page)*lens_factor_);
    const TileKey key{page,static_cast<int>(std::lround(scale*100)),-1,-1,Hidden(page),generation_,!hide_annotations_};
    auto covers=[&](const TileRequest& r){return r.key==key&&r.tone==tone_&&r.clip.x<=x0+.01f&&r.clip.y<=y0+.01f&&r.clip.x+r.clip.w>=x1-.01f&&r.clip.y+r.clip.h>=y1-.01f;};
    if((lens_tile_&&covers(lens_tile_->request))||(lens_pending_&&covers(*lens_pending_)))return;
    // 多渲染半个镜框的余量，小幅移动不必重新请求。
    const float mx=(x1-x0)*.5f,my=(y1-y0)*.5f;
    const float cx0=std::max(info.originX,x0-mx),cy0=std::max(info.originY,y0-my);
    const Rect clip{cx0,cy0,std::min(info.originX+info.width,x1+mx)-cx0,std::min(info.originY+info.height,y1+my)-cy0};
    if(lens_pending_&&lens_pending_->cancelled)lens_pending_->cancelled->store(true);
    lens_pending_=TileRequest{key,scale,clip,generation_,std::make_shared<std::atomic_bool>(false),Hidden(page),tone_};
    request_tile(*lens_pending_);
}
void PdfCanvas::DrawLens(lumen::Painter& p){
    if(!lens_on_||!lens_inside_||view_!=View::Reading||presenting_||layout_.empty())return;
    const auto frame=LensRect().Offset(absolute_.x,absolute_.y);
    const lumen::Point origin{absolute_.x+lens_point_.x,absolute_.y+lens_point_.y};
    const auto lensArea=LensRect();
    p.DrawGlow(frame,4,C(0x000000,.7f),1.0f);
    p.PushClip(frame);
    p.FillRect(frame,C(0x191919));
    // 以指针为中心整体放大：先画普通瓦片（立即可见，略糊），高分辨率局部图到达后叠在上面。
    p.PushScale(origin,lens_factor_,lens_factor_);
    bool loading=false;
    for(int page:visible_){
        const auto r=PageRect(page).Offset(absolute_.x,absolute_.y-scroll_);
        const auto local=PageRect(page).Offset(0,-scroll_);
        const float hw=lensArea.w*.5f/lens_factor_,hh=lensArea.h*.5f/lens_factor_;
        if(local.Right()<lens_point_.x-hw||local.x>lens_point_.x+hw||local.Bottom()<lens_point_.y-hh||local.y>lens_point_.y+hh)continue;
        p.FillRect(r,C(ToneColor(tone_,0xffffff)));
        const int rot=Rot();
        if(rot){p.PushRotate({r.x+r.w*.5f,r.y+r.h*.5f},static_cast<float>(rot));unrotated_=true;}
        DrawPageTiles(p,page,loading);
        if(lens_tile_&&lens_tile_->bitmap&&lens_tile_->request.key.page==page)p.DrawBitmap(lens_tile_->bitmap.Get(),ScreenRect(page,lens_tile_->request.clip),true);
        if(rot){unrotated_=false;p.PopTransform();}
    }
    p.PopTransform();
    p.PopClip();
    p.StrokeRoundedRect(frame,2,C(0xf2f2f2,.9f),1.5f);
    const auto label=L"放大镜 "+std::to_wstring(static_cast<int>(lens_factor_))+(lens_factor_-std::floor(lens_factor_)>.01f?L".5":L"")+L"×";
    const lumen::Rect tag{frame.x+6,frame.Bottom()-24,78,18};
    p.FillRoundedRect(tag,4,C(0x000000,.6f));p.DrawText(label,tag,lumen::TextRole::Caption,C(0xf2f2f2),lumen::Align::Center);
}
void PdfCanvas::FitPage(){horizontal_=0;fit_=Fit::Page;LayoutPages();GoTo(current_page_);if(zoom_changed)zoom_changed(ActualZoom());}
void PdfCanvas::FitWidth(){
    if(view_!=View::Reading||pages_.empty()){fit_=Fit::Width;return;}
    if(current_page_<0||current_page_>=static_cast<int>(layout_.size())){horizontal_=0;fit_=Fit::Width;LayoutPages();return;}   // 尚未排版
    // 保持当前页内的相对阅读位置，而不是跳回页首。
    const int page=current_page_;const auto& before=layout_[page].rect;
    const float within=before.h>0?std::clamp((scroll_-before.y)/before.h,0.0f,1.0f):0.0f;
    horizontal_=0;fit_=Fit::Width;LayoutPages();
    const auto& after=layout_[page].rect;
    scroll_=target_scroll_=std::clamp(after.y+within*after.h,0.0f,std::max(0.0f,content_height_-absolute_.h));
    UpdateViewport();if(zoom_changed)zoom_changed(ActualZoom());
}
void PdfCanvas::StepPage(int direction){
    if(pages_.empty()||layout_.empty())return;
    int page=current_page_;
    // 当前页顶部已滚出视口时，“上一页”先回到本页顶部。
    if(direction<0&&layout_[page].rect.y<scroll_-24)direction=0;
    if(spread_&&view_==View::Reading&&direction!=0){
        // 对开时按行翻：跳到相邻行的第一页。
        const int target=layout_[page].row+direction;int next=page;
        for(int k=0;k<static_cast<int>(layout_.size());++k)if(layout_[k].row==target){next=k;break;}
        page=next;
    }else page=std::clamp(page+direction,0,static_cast<int>(layout_.size())-1);
    const float target=std::clamp(layout_[page].rect.y-16,0.0f,std::max(0.0f,content_height_-absolute_.h));
    current_page_=page;
    // 翻页模式直接换页：动画经过相邻页会被当作“滚出本页”而改判当前页。
    if(PagedActive()){float lo,hi;RowRange(layout_[page].row,lo,hi);scroll_=target_scroll_=lo;UpdateViewport();}
    else{target_scroll_=target;Animate();}
    if(page_changed)page_changed(page);
}
void PdfCanvas::Selection(int page,std::vector<Annotation> annotations,int selected){
    annotations_page_=page;annotations_=std::move(annotations);selected_annotation_=selected;moving_.reset();
    if(page>=0&&page<static_cast<int>(pages_.size())){
        lumen::TextLayout measure;
        for(auto& a:annotations_)if(a.type==Tool::Text&&a.lumenText&&a.rotation==0)a.bounds=FitTextBounds(a,pages_[page],measure);
    }
    Invalidate();
}
void PdfCanvas::SearchHighlight(std::optional<SearchHit> hit){search_=hit;if(hit)GoTo(hit->page);Invalidate();}
void PdfCanvas::SearchResults(std::vector<SearchHit> hits,int current,bool reveal){
    search_hits_=std::move(hits);search_current_=current;
    if(current>=0&&current<static_cast<int>(search_hits_.size())){
        search_=search_hits_[current];const auto& hit=search_hits_[current];
        if(reveal&&hit.page>=0&&hit.page<static_cast<int>(layout_.size())){
            // 结果不在视口内才滚动，并把它放在视口上部三分之一处。
            const auto r=ScreenRect(hit.page,hit.bounds);
            if(r.y<absolute_.y+8||r.Bottom()>absolute_.Bottom()-8){
                current_page_=hit.page;
                scroll_=target_scroll_=std::clamp(r.y-absolute_.y+scroll_-absolute_.h*.3f,0.0f,std::max(0.0f,content_height_-absolute_.h));
                UpdateViewport();
            }
        }
    }else search_.reset();
    Invalidate();
}
lumen::Rect PdfCanvas::ScrollThumb()const{
    const float track=std::max(1.0f,absolute_.h-8);
    const float h=std::min(track,std::max(32.0f,track*absolute_.h/std::max(1.0f,content_height_)));
    const float y=scroll_/std::max(1.0f,content_height_-absolute_.h)*(track-h);
    const float w=scrollbar_hover_||scrollbar_drag_?8.0f:5.0f;
    return {absolute_.Right()-w-5,absolute_.y+4+y,w,h};
}
float PdfCanvas::HorizontalLimit()const{
    if(view_!=View::Reading)return std::max(0.0f,content_width_-absolute_.w);
    float left=0,right=0;bool any=false;
    for(const auto& l:layout_){
        if(l.rect.Bottom()<scroll_||l.rect.y>scroll_+absolute_.h)continue;
        left=any?std::min(left,l.rect.x):l.rect.x;right=any?std::max(right,l.rect.Right()):l.rect.Right();any=true;
    }
    return any?std::max(0.0f,right-left+24-absolute_.w):0.0f;
}
void PdfCanvas::ClampHorizontal(){
    // 从宽页滚到窄页后自动回正，避免窄页停在偏左的位置、右侧留出大片空白。
    if(view_!=View::Reading||horizontal_<=0)return;
    const float limit=HorizontalLimit();
    if(horizontal_>limit+.5f){horizontal_=limit;LayoutPages();}
}
void PdfCanvas::UpdateViewport(){
    ClampHorizontal();
    if(PagedActive()){
        // 翻页模式：滚动位置越出当前一屏（拖动滚动条等）时按视口位置改定当前页，再限制在该页范围内。
        float lo,hi;const int count=static_cast<int>(layout_.size());
        RowRange(layout_[std::clamp(current_page_,0,count-1)].row,lo,hi);
        if(scroll_<lo-1||scroll_>hi+1){
            int page=0;for(int i=0;i<count;++i)if(layout_[i].rect.y<scroll_+absolute_.h*.4f)page=i;
            for(int i=0;i<count;++i)if(layout_[i].row==layout_[page].row){page=i;break;}
            if(page!=current_page_){current_page_=page;if(page_changed)page_changed(page);}
        }
        PagedClamp();
    }
    RequestVisible();
    if(view_==View::Reading&&!visible_.empty()&&!PagedActive()){
        int page=visible_.front();
        for(int n:visible_)if(layout_[n].rect.y<scroll_+absolute_.h*.4f)page=n;
        if(page!=current_page_){current_page_=page;if(page_changed)page_changed(page);}
    }
    Invalidate();
}
void PdfCanvas::Scroll(float delta,bool smooth){
    if(PagedActive()&&delta!=0){
        float lo,hi;RowRange(layout_[std::clamp(current_page_,0,static_cast<int>(layout_.size())-1)].row,lo,hi);
        if((delta>0&&target_scroll_>=hi-.5f)||(delta<0&&target_scroll_<=lo+.5f)){
            // 已到页边：整页翻过。连续滚轮 / 触摸板惯性每 0.35 秒最多翻一页；页内刚滚到底时也先停一下。
            const auto now=GetTickCount64();
            if(now-flip_tick_>=350&&FlipRow(delta>0?1:-1,delta>0))flip_tick_=now;
            return;
        }
        if(delta*(target_scroll_-scroll_)<0)target_scroll_=scroll_;
        target_scroll_=std::clamp(target_scroll_+delta,lo,hi);flip_tick_=GetTickCount64();
        if(smooth)Animate();else{scroll_=target_scroll_;UpdateViewport();}
        return;
    }
    if(delta*(target_scroll_-scroll_)<0)target_scroll_=scroll_;
    target_scroll_=std::clamp(target_scroll_+delta,0.0f,std::max(0.0f,content_height_-absolute_.h));
    if(smooth)Animate();else{scroll_=target_scroll_;UpdateViewport();}
}
bool PdfCanvas::OnAnimate(float dt){
    const bool base=Control::OnAnimate(dt);
    const float before=scroll_;
    bool moving=lumen::EaseTo(scroll_,target_scroll_,dt,kMotion,.1f);
    if(page_drag_.active){
        // 拖到上下边缘自动滚动，越靠边越快；滚动期间预览顺序同步更新。
        const float zone=std::min(64.0f,absolute_.h*.2f),y=page_drag_.mouse.y;float push=0;
        if(y<zone)push=-std::min(1.5f,(zone-y)/zone);else if(y>absolute_.h-zone)push=std::min(1.5f,(y-(absolute_.h-zone))/zone);
        if(push!=0){scroll_=target_scroll_=std::clamp(scroll_+push*std::abs(push)*1100.0f*dt,0.0f,std::max(0.0f,content_height_-absolute_.h));}
        UpdatePageDrag();moving=true;
    }
    if(dragging_&&selecting_text_&&!area_select_&&tool_==Tool::Select&&view_==View::Reading&&!layout_.empty()){
        const float zone=std::min(40.0f,absolute_.h*.12f),y=select_mouse_.y;float push=0;
        if(y<zone)push=-std::min(1.5f,(zone-y)/zone);else if(y>absolute_.h-zone)push=std::min(1.5f,(y-(absolute_.h-zone))/zone);
        if(push!=0){
            scroll_=target_scroll_=std::clamp(scroll_+push*std::abs(push)*1100.0f*dt,0.0f,std::max(0.0f,content_height_-absolute_.h));
            PagedClamp();UpdateViewport();ExtendSelection(select_mouse_);moving=true;
        }
    }
    if(before!=scroll_)UpdateViewport();
    if(Grid()&&motion_&&shown_.size()==layout_.size()){
        bool settling=false;
        for(size_t i=0;i<shown_.size();++i){
            if(page_drag_.active&&InDragGroup(static_cast<int>(i)))continue;
            auto& v=shown_[i];const auto& t=layout_[i].rect;
            settling|=lumen::EaseTo(v.x,t.x,dt,kMotion,.15f);
            settling|=lumen::EaseTo(v.y,t.y,dt,kMotion,.15f);
            v.w=t.w;v.h=t.h;
        }
        settling|=lumen::EaseTo(lift_,page_drag_.active?1.0f:0.0f,dt,kMotion,.002f);
        if(!settling&&!page_drag_.active){motion_=false;lifted_.clear();lift_=0;}
        RequestVisible();Invalidate();
        moving=moving||motion_;
    }
    for(size_t i=0;i<hover_mix_.size();++i){
        const float target=static_cast<int>(i)==hover_page_&&!page_drag_.active?1.0f:0.0f;
        if(hover_mix_[i]!=target){if(lumen::EaseTo(hover_mix_[i],target,dt,kMotion,.01f))moving=true;Invalidate();}
    }
    if(loading_){spin_=std::fmod(spin_+dt*300.0f,360.0f);Invalidate();}
    return base||moving||loading_;
}
bool PdfCanvas::OnHWheel(float delta){
    if(presenting_)return true;
    horizontal_=std::clamp(horizontal_+delta*96.0f,0.0f,HorizontalLimit());
    LayoutPages();return true;
}
bool PdfCanvas::OnWheel(float delta){
    if(presenting_&&view_==View::Reading){
        const auto now=GetTickCount64();
        if(delta!=0&&now-flip_tick_>=350&&FlipRow(delta<0?1:-1,true))flip_tick_=now;
        return true;
    }
    if(lens_on_&&(GetKeyState(VK_MENU)&0x8000)&&view_==View::Reading){MagnifierFactor(lens_factor_+(delta>0?.5f:-.5f));return true;}
    if((GetKeyState(VK_CONTROL)&0x8000)&&view_==View::Reading){Zoom(std::pow(1.1f,delta));return true;}
    if((GetKeyState(VK_CONTROL)&0x8000)&&view_==View::Pages){GridZoom(std::pow(1.1f,delta));return true;}
    // 拖动页面时仍可用滚轮滚动（预览顺序在动画帧中跟随更新）。
    if((dragging_&&!page_drag_.active)||scrollbar_drag_)return true;
    if((GetKeyState(VK_SHIFT)&0x8000)&&view_==View::Reading)return OnHWheel(-delta);
    if((GetKeyState(VK_CONTROL)&0x8000)&&view_==View::Reading)Zoom(std::pow(1.1f,delta));
    else{
        UINT lines=3;SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
        const float step=lines==WHEEL_PAGESCROLL?absolute_.h*.85f:32.0f*lines;
        Scroll(-delta*step,std::abs(delta)>=.35f);
    }
    return true;
}
bool PdfCanvas::OnKey(uint32_t key){
    const bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0;
    const bool shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
    // 拖动页面时 Esc：取消本次拖动，页面平滑退回原位。
    if(key==VK_ESCAPE&&page_drag_.active){EndPageDrag(false);return true;}
    if(key==VK_ESCAPE&&zoom_box_){ZoomBox(false);return true;}
    if(key==VK_ESCAPE&&lens_on_&&!redact_mode_&&!presenting_&&!draft_&&text_selection_.page<0){Magnifier(false);return true;}
    if(redact_mode_){
        if(key==VK_ESCAPE){if(redact_drag_){redact_drag_.reset();Invalidate();}else if(redact_escape)redact_escape();return true;}
        if(key==VK_DELETE&&redact_hover_>=0&&redact_remove){const auto i=static_cast<size_t>(redact_hover_);redact_hover_=-1;redact_remove(i);return true;}
    }
    if(presenting_&&view_==View::Reading&&!pages_.empty()){
        switch(key){
        case VK_RIGHT:case VK_DOWN:case VK_NEXT:case VK_SPACE:case VK_RETURN:case 'N':FlipRow(1,true);return true;
        case VK_LEFT:case VK_UP:case VK_PRIOR:case VK_BACK:case 'P':FlipRow(-1,true);return true;
        case VK_HOME:GoTo(0);if(page_changed)page_changed(0);return true;
        case VK_END:GoTo(static_cast<int>(pages_.size())-1);if(page_changed)page_changed(current_page_);return true;
        case VK_ESCAPE:if(exit_presentation)exit_presentation();return true;
        default:return false;
        }
    }
    // 按住空格 = 临时手型工具（松开由窗口的 WM_KEYUP 通知）；吞掉自动重复，避免触发其它控件。
    if(key==VK_SPACE&&view_==View::Reading&&!draft_&&!ctrl){SpaceHeld(true);return true;}
    // Acrobat 翻页键：Enter / Shift+Enter 下一屏 / 上一屏；Ctrl+PageDown/PageUp 下一页 / 上一页；
    // Ctrl+Shift+PageUp/↑ 首页，Ctrl+Shift+PageDown/↓ 末页。
    if(view_==View::Reading&&!draft_&&!pages_.empty()){
        if(key==VK_RETURN&&!ctrl){Scroll(shift?-absolute_.h*.85f:absolute_.h*.85f);return true;}
        if(ctrl&&shift&&(key==VK_PRIOR||key==VK_UP)){GoTo(0);if(page_changed)page_changed(0);return true;}
        if(ctrl&&shift&&(key==VK_NEXT||key==VK_DOWN)){GoTo(static_cast<int>(pages_.size())-1);if(page_changed)page_changed(current_page_);return true;}
        if(ctrl&&!shift&&(key==VK_NEXT||key==VK_PRIOR)){StepPage(key==VK_NEXT?1:-1);return true;}
    }
    if((GetKeyState(VK_MENU)&0x8000)&&(key==VK_LEFT||key==VK_RIGHT)&&view_==View::Reading){if(navigate)navigate(key==VK_LEFT?-1:1);return true;}
    if(ctrl&&(key=='C'||key==VK_INSERT)&&text_selection_.page>=0&&!draft_){if(copy_selection)copy_selection(text_selection_);return true;}
    if(key==VK_ESCAPE&&text_selection_.page>=0&&!draft_){ClearTextSelection();return false;}
    if(key==VK_DELETE&&editable_&&!draft_&&selected_annotation_>=0){
        if(delete_annotation)delete_annotation(annotations_page_,selected_annotation_);return true;
    }
    if(view_==View::Pages&&!pages_.empty()){
        if(ctrl&&key=='A'){
            selected_pages_.clear();for(int n=0;n<static_cast<int>(pages_.size());++n)selected_pages_.push_back(n);
            anchor_page_=current_page_;if(pages_selected)pages_selected(selected_pages_.size());Invalidate();return true;
        }
        if(key==VK_DELETE){if(delete_pages)delete_pages();return true;}
        if(key==VK_ESCAPE&&selected_pages_.size()>1){selected_pages_={current_page_};anchor_page_=current_page_;if(pages_selected)pages_selected(1);Invalidate();return true;}
    }
    // 翻页模式：页内还有未显示的部分时按屏滚动，到页边再整页翻过（不受滚轮翻页的间隔限制）。
    if((key==VK_NEXT||key==VK_PRIOR)&&PagedActive()){
        const int dir=key==VK_NEXT?1:-1;float lo,hi;
        RowRange(layout_[std::clamp(current_page_,0,static_cast<int>(layout_.size())-1)].row,lo,hi);
        if(dir>0?target_scroll_<hi-.5f:target_scroll_>lo+.5f)Scroll(dir*absolute_.h*.85f);else FlipRow(dir,dir>0);
        return true;
    }
    // 整页适配时 PageUp/PageDown 按页翻；放大阅读时按屏滚动。左右方向键在无横向滚动时翻页。
    if(key==VK_NEXT){if(view_==View::Reading&&fit_==Fit::Page)StepPage(1);else Scroll(absolute_.h*.85f);return true;}
    if(key==VK_PRIOR){if(view_==View::Reading&&fit_==Fit::Page)StepPage(-1);else Scroll(-absolute_.h*.85f);return true;}
    if(view_==View::Reading&&content_width_<=absolute_.w+1){
        if(key==VK_RIGHT){StepPage(1);return true;}
        if(key==VK_LEFT){StepPage(-1);return true;}
    }
    if(key==VK_DOWN){Scroll(60);return true;}if(key==VK_UP){Scroll(-60);return true;}
    if(key==VK_HOME){GoTo(0);if(page_changed)page_changed(0);return true;}
    if(key==VK_END&&!pages_.empty()){GoTo(static_cast<int>(pages_.size())-1);if(page_changed)page_changed(current_page_);return true;}
    return false;
}
int PdfCanvas::Hit(lumen::Point p)const{
    const lumen::Point c{p.x,p.y+scroll_};
    if(Grid()){for(auto it=visible_.rbegin();it!=visible_.rend();++it)if(PageRect(*it).Contains(c))return *it;return -1;}
    for(int i:visible_)if(layout_[i].rect.Contains(c))return i;return -1;
}
Point PdfCanvas::PagePoint(int page,lumen::Point p)const{
    const auto& l=layout_[page];const auto& info=pages_[page];const auto box=PageRect(page);
    const float x=p.x-box.x,y=p.y+scroll_-box.y,w=info.width*l.scale,h=info.height*l.scale;float u=x,v=y;
    switch(Rot()){case 90:u=y;v=h-x;break;case 180:u=w-x;v=h-y;break;case 270:u=w-y;v=x;break;default:break;}
    return {info.originX+u/l.scale,info.originY+v/l.scale};
}
int PdfCanvas::PageHandle(int handle)const{
    // 8 个控制点在 3×3 网格上的位置（去掉中心）；按视图旋转反算到页面坐标系。
    static constexpr int col[]={0,1,2,0,2,0,1,2},row[]={0,0,0,1,1,2,2,2};
    const int rot=Rot();if(!rot||handle<0||handle>7)return handle;
    const int sx=col[handle],sy=row[handle];int c=sx,r=sy;
    if(rot==90){c=sy;r=2-sx;}else if(rot==180){c=2-sx;r=2-sy;}else{c=2-sy;r=sx;}
    for(int i=0;i<8;++i)if(col[i]==c&&row[i]==r)return i;
    return handle;
}
void PdfCanvas::ViewRotation(int degrees){
    degrees=((degrees%360)+360)%360;degrees=degrees/90*90;
    if(degrees==rotation_)return;
    const int page=current_page_;rotation_=degrees;
    if(view_!=View::Reading){Invalidate();return;}
    if(fit_==Fit::Width||fit_==Fit::None)horizontal_=0;
    LayoutPages();GoTo(page);if(zoom_changed)zoom_changed(ActualZoom());
}
bool PdfCanvas::HandActive()const{
    if(view_!=View::Reading||pages_.empty()||draft_)return false;
    return hand_tool_||(space_held_&&(GetKeyState(VK_SPACE)&0x8000)!=0);
}
void PdfCanvas::OnMouseDown(lumen::Point local,uint32_t buttons){
    const bool middle=(buttons&MK_MBUTTON)!=0;
    if(presenting_&&view_==View::Reading){
        // 演示：单击链接跳转，其它位置下一页。记下时间，指针输入随后补报的双击不再重复翻页。
        if(!(buttons&1))return;
        Focus();triple_tick_=GetTickCount64();
        const int page=Hit(local);
        if(page>=0)if(const Link* link=LinkAt(page,PagePoint(page,local))){if(activate_link)activate_link(*link);return;}
        FlipRow(1,true);return;
    }
    if(!(buttons&1)&&!middle)return;Focus();
    const bool onScrollbar=local.x>=absolute_.w-18&&content_height_>absolute_.h;
    if(zoom_box_&&(buttons&1)&&!onScrollbar&&view_==View::Reading){zoom_origin_=local;zoom_drag_=lumen::Rect{local.x,local.y,0,0};dragging_=false;Invalidate();return;}
    if(!onScrollbar&&(middle?view_==View::Reading&&!pages_.empty()&&!draft_:HandActive())){
        // 手型拖动：记录起点，之后按位移直接设置滚动位置（不走平滑动画，跟手）。
        panning_=true;pan_origin_=local;pan_scroll_=target_scroll_=scroll_;pan_horizontal_=horizontal_;
        dragging_=false;selecting_text_=false;press_link_.reset();Invalidate();return;
    }
    if(!(buttons&1))return;
    // 涂黑模式：页面上按下开始框选（滚动条仍按原逻辑处理）。
    if(redact_mode_&&view_==View::Reading&&!presenting_&&!(local.x>=absolute_.w-18&&content_height_>absolute_.h)){
        const int page=Hit(local);if(page<0)return;
        const auto pt=PagePoint(page,local);redact_drag_=RedactDrag{page,pt,pt};Invalidate();return;
    }
    if(!draft_&&local.x>=absolute_.w-18&&content_height_>absolute_.h){
        const auto thumb=ScrollThumb();scrollbar_drag_=true;target_scroll_=scroll_;
        const float y=local.y+absolute_.y;
        scroll_grab_=y>=thumb.y&&y<=thumb.Bottom()?y-thumb.y:thumb.h/2;
        OnMouseMove(local,buttons);return;
    }
    if(!editable_)return;
    if(draft_){
        down_page_=draft_page_;down_local_=local;down_point_=last_point_=PagePoint(down_page_,local);
        resize_handle_=HandleAt(*draft_,local);resizing_=resize_handle_>=0;
        auto r=ScreenRect(down_page_,draft_->bounds).Inset(-9,-9);
        if(resizing_||r.Contains({local.x+absolute_.x,local.y+absolute_.y})){
            moving_=*draft_;dragging_=true;
        }else{dragging_=false;moving_.reset();if(commit_text)commit_text();}
        return;
    }
    target_scroll_=scroll_;down_page_=Hit(local);if(down_page_<0)return;
    down_local_=local;current_page_=down_page_;down_point_=last_point_=PagePoint(down_page_,local);
    if(view_!=View::Reading){
        collapse_on_up_=false;
        const bool ctrlDown=(GetKeyState(VK_CONTROL)&0x8000)!=0,shiftDown=(GetKeyState(VK_SHIFT)&0x8000)!=0;
        const bool already=std::find(selected_pages_.begin(),selected_pages_.end(),down_page_)!=selected_pages_.end();
        if(shiftDown&&anchor_page_>=0&&anchor_page_<static_cast<int>(pages_.size())){
            // Shift：从锚点到当前页连续选择；Ctrl+Shift 在已有选择上追加这一段。
            if(!ctrlDown)selected_pages_.clear();
            const int from=std::min(anchor_page_,down_page_),to=std::max(anchor_page_,down_page_);
            for(int n=from;n<=to;++n)if(std::find(selected_pages_.begin(),selected_pages_.end(),n)==selected_pages_.end())selected_pages_.push_back(n);
            std::sort(selected_pages_.begin(),selected_pages_.end());
        }else if(ctrlDown){
            auto it=std::find(selected_pages_.begin(),selected_pages_.end(),down_page_);
            if(it==selected_pages_.end())selected_pages_.push_back(down_page_);else selected_pages_.erase(it);
            anchor_page_=down_page_;
        }else{
            // 按在已选中的页上时保留多选，方便整组拖动；否则单选。
            if(!already)selected_pages_={down_page_};
            collapse_on_up_=already&&selected_pages_.size()>1;
            anchor_page_=down_page_;
        }
        if(pages_selected)pages_selected(selected_pages_.size());
        // 按在选中的页上：超过拖动阈值后浮起（Ctrl 单击取消选中的那页不参与拖动）。
        page_drag_=PageDrag{};
        if(std::find(selected_pages_.begin(),selected_pages_.end(),down_page_)!=selected_pages_.end()){
            const auto box=PageRect(down_page_);page_drag_.grabbed=down_page_;
            page_drag_.grab={local.x-box.x,local.y+scroll_-box.y};page_drag_.mouse=local;
        }
        dragging_=true;if(page_changed)page_changed(down_page_);Invalidate();return;
    }
    if(page_changed)page_changed(down_page_);
    if(!editable_)return;
    moving_.reset();resizing_=false;resize_handle_=-1;
    if(tool_==Tool::Text&&Rot()){if(rotated_edit_blocked)rotated_edit_blocked();return;}
    if(tool_==Tool::Text&&annotations_page_==down_page_){
        for(auto it=annotations_.rbegin();it!=annotations_.rend();++it){
            if(it->type==Tool::Text&&it->bounds.Contains(down_point_)){
                const int id=it->id;
                dragging_=false;selected_annotation_=id;
                if(select_annotation)select_annotation(down_page_,id);
                Invalidate();return;
            }
        }
    }
    if(!hide_annotations_&&tool_==Tool::Select&&annotations_page_==down_page_){
        for(auto it=annotations_.rbegin();it!=annotations_.rend();++it){
            if(it->type==Tool::Select||it->readOnly)continue;
            const float pad=5/layout_[down_page_].scale;
            const int handle=it->id==selected_annotation_?HandleAt(*it,local):-1;
            if(handle>=0||HitAnnotation(*it,down_point_,pad)){
                selected_annotation_=it->id;moving_=*it;resize_handle_=handle;resizing_=handle>=0;break;
            }
        }
        if(!moving_)selected_annotation_=-1;
        else{
            // 按下即准备实时预览：请求该批注的外观位图，并让瓦片开始隐藏原位内容。
            settling_.reset();BeginGhost(down_page_,*moving_);RequestVisible();
        }
        if(select_annotation)select_annotation(down_page_,selected_annotation_);
    }
    if(tool_==Tool::Select){
        if(!moving_){
            // 空白处按下：可能是单击链接，也可能开始选择文字（Alt 拖动为区域复制）。
            area_select_=(GetKeyState(VK_MENU)&0x8000)!=0;
            // 双击后很快在原处再按一下：三击选段（系统双击时间与距离内）。
            const auto now=GetTickCount64();
            if(!area_select_&&double_tick_&&now-double_tick_<=GetDoubleClickTime()&&std::abs(local.x-double_local_.x)<=6&&std::abs(local.y-double_local_.y)<=6){
                double_tick_=0;triple_tick_=now;SnapSelect(down_page_,down_point_,2);return;
            }
            const FormField* field=FieldAt(down_page_,down_point_);
            press_field_=field?std::optional<std::pair<int,FormField>>({down_page_,*field}):std::nullopt;
            const Link* link=field?nullptr:LinkAt(down_page_,down_point_);
            press_link_=link?std::optional<Link>(*link):std::nullopt;
            selecting_text_=true;select_mouse_=local;
            text_selection_={down_page_,down_point_,down_point_,{},down_page_,{}};UpdateSpans();
        }else{text_selection_={};selecting_text_=false;press_link_.reset();press_field_.reset();}
    }
    dragging_=true;stroke_.clear();stroke_.push_back(down_point_);
    area_highlight_=(GetKeyState(VK_CONTROL)&0x8000)!=0;highlight_preview_.clear();
    if(IsMarkupTool(tool_))RequestHighlight();Invalidate();
}
void PdfCanvas::OnMouseMove(lumen::Point local,uint32_t){
    scrollbar_hover_=local.x>=absolute_.w-18;
    if(lens_on_){const bool inside=!scrollbar_hover_&&!presenting_;if(inside||lens_inside_){lens_point_=local;lens_inside_=inside;RequestLens();Invalidate();}}
    if(zoom_drag_){
        const float x=std::clamp(local.x,0.0f,absolute_.w),y=std::clamp(local.y,0.0f,absolute_.h);
        zoom_drag_=lumen::Rect{std::min(zoom_origin_.x,x),std::min(zoom_origin_.y,y),std::abs(x-zoom_origin_.x),std::abs(y-zoom_origin_.y)};Invalidate();return;
    }
    if(scrollbar_drag_){
        const float track=std::max(1.0f,absolute_.h-8-ScrollThumb().h);
        const float target=std::clamp((local.y-4-scroll_grab_)/track,0.0f,1.0f)*std::max(0.0f,content_height_-absolute_.h);
        scroll_=target_scroll_=target;UpdateViewport();return;
    }
    if(panning_){
        scroll_=target_scroll_=std::clamp(pan_scroll_-(local.y-pan_origin_.y),0.0f,std::max(0.0f,content_height_-absolute_.h));
        const float h=std::clamp(pan_horizontal_-(local.x-pan_origin_.x),0.0f,HorizontalLimit());
        if(h!=horizontal_){horizontal_=h;LayoutPages();}else UpdateViewport();
        return;
    }
    if(redact_mode_&&view_==View::Reading){
        if(redact_drag_){
            // 框选限制在起始页内。
            auto pt=PagePoint(redact_drag_->page,local);const auto& g=pages_[static_cast<size_t>(redact_drag_->page)];
            pt.x=std::clamp(pt.x,g.originX,g.originX+g.width);pt.y=std::clamp(pt.y,g.originY,g.originY+g.height);
            redact_drag_->current=pt;Invalidate();return;
        }
        const int page=Hit(local);const int hover=page>=0?RedactMarkAt(page,PagePoint(page,local)):-1;
        if(hover!=redact_hover_){redact_hover_=hover;Invalidate();}
        return;
    }
    if(!dragging_||down_page_<0){
        hover_annotation_=-1;int page=Hit(local);
        if(view_!=View::Reading&&page!=hover_page_){hover_page_=page;Animate();Invalidate();}
        if(!hide_annotations_&&tool_==Tool::Select&&page==annotations_page_&&page>=0){
            auto point=PagePoint(page,local);
            for(auto it=annotations_.rbegin();it!=annotations_.rend();++it)if(HitAnnotation(*it,point,4/layout_[page].scale)){hover_annotation_=it->id;break;}
        }
        int fieldPage=-1,fieldId=-1;
        if(view_==View::Reading&&tool_==Tool::Select&&hover_annotation_<0&&page>=0&&!presenting_)
            if(const FormField* f=FieldAt(page,PagePoint(page,local))){fieldPage=page;fieldId=f->id;}
        if(fieldPage!=hover_field_page_||fieldId!=hover_field_){hover_field_page_=fieldPage;hover_field_=fieldId;}
        int linkPage=-1,linkIndex=-1;
        if(view_==View::Reading&&tool_==Tool::Select&&hover_annotation_<0&&page>=0&&fieldId<0){
            if(const Link* link=LinkAt(page,PagePoint(page,local))){
                auto& list=links_[page].links;linkPage=page;linkIndex=static_cast<int>(link-list.data());
            }
        }
        if(linkPage!=hover_link_page_||linkIndex!=hover_link_){
            hover_link_page_=linkPage;hover_link_=linkIndex;
            if(link_hover)link_hover(linkIndex>=0?&links_[linkPage].links[static_cast<size_t>(linkIndex)]:nullptr);
        }
        Invalidate();return;
    }
    if(Grid()){
        page_drag_.mouse=local;
        if(!page_drag_.active&&page_drag_.grabbed>=0&&editable_&&(reorder_pages||reorder_page)&&std::abs(local.x-down_local_.x)+std::abs(local.y-down_local_.y)>5)BeginPageDrag();
        if(page_drag_.active){UpdatePageDrag();Animate();}
        return;
    }
    last_point_=ClampPoint(PagePoint(down_page_,local),pages_[down_page_]);
    if(!moving_)last_point_=ConstrainToPage(down_point_,PagePoint(down_page_,local),tool_,(GetKeyState(VK_SHIFT)&0x8000)!=0,pages_[down_page_]);
    if(moving_){
        const Annotation* originalAnnotation=nullptr;
        if(draft_)originalAnnotation=&*draft_;
        else{auto found=std::find_if(annotations_.begin(),annotations_.end(),[&](const auto& a){return a.id==moving_->id;});if(found!=annotations_.end())originalAnnotation=&*found;}
        if(originalAnnotation){const auto* it=originalAnnotation;
            if(resizing_&&IsLineTool(it->type)&&it->points.size()==2){
                *moving_=*it;
                const int endpoint=std::clamp(resize_handle_,0,1);
                moving_->points[endpoint]=ConstrainToPage(it->points[1-endpoint],PagePoint(down_page_,local),it->type,(GetKeyState(VK_SHIFT)&0x8000)!=0,pages_[down_page_]);
                moving_->bounds=PointsRect(moving_->points[0],moving_->points[1]);moving_->geometryEdited=true;
            }else if(resizing_){
                const float dx=last_point_.x-down_point_.x,dy=last_point_.y-down_point_.y;
                auto& r=moving_->bounds;const auto& original=it->bounds;
                const bool left=resize_handle_==0||resize_handle_==3||resize_handle_==5;
                const bool top=resize_handle_<=2;
                r.w=(resize_handle_==1||resize_handle_==6)?original.w:std::max(24.0f,original.w+(left?-dx:dx));
                r.h=(resize_handle_==3||resize_handle_==4)?original.h:std::max(18.0f,original.h+(top?-dy:dy));
                if(it->type==Tool::Image){
                    const float sx=r.w/std::max(1.0f,original.w),sy=r.h/std::max(1.0f,original.h);
                    float factor=std::max(sx,sy);
                    factor=std::min({factor,pages_[down_page_].width/std::max(1.0f,original.w),pages_[down_page_].height/std::max(1.0f,original.h)});
                    r.w=original.w*factor;r.h=original.h*factor;
                }
                r.x=left?original.x+original.w-r.w:original.x;
                r.y=top?original.y+original.h-r.h:original.y;
            }
            else{moving_->bounds.x=it->bounds.x+last_point_.x-down_point_.x;moving_->bounds.y=it->bounds.y+last_point_.y-down_point_.y;}
            auto& box=moving_->bounds;const auto& page=pages_[down_page_];
            box.w=std::min(box.w,page.width);box.h=std::min(box.h,page.height);
            box.x=std::clamp(box.x,page.originX,page.originX+page.width-box.w);
            box.y=std::clamp(box.y,page.originY,page.originY+page.height-box.h);
            if(!(resizing_&&IsLineTool(it->type)))*moving_=TransformAnnotation(*it,box);
            if(resizing_&&it->type==Tool::Text){
                moving_->fixedTextBox=true;moving_->fixedTextWidth=true;
                moving_->textSizing=(resize_handle_==3||resize_handle_==4)?TextSizing::Width:TextSizing::Fixed;
            }
        }
    }else if(tool_==Tool::Ink){
        if(stroke_.empty()||PointDistance(stroke_.back(),last_point_)*layout_[down_page_].scale>=.45f)stroke_.push_back(last_point_);
        if(stroke_.size()>8192){std::vector<Point> reduced;for(size_t i=0;i<stroke_.size();i+=2)reduced.push_back(stroke_[i]);reduced.push_back(stroke_.back());stroke_=std::move(reduced);}
    }else if(IsMarkupTool(tool_))RequestHighlight();
    else if(tool_==Tool::Select&&selecting_text_&&view_==View::Reading){
        select_mouse_=local;
        if(area_select_)text_selection_.end=last_point_;
        else{
            ExtendSelection(local);
            // 靠近上下边缘：动画帧里自动滚动并继续扩展选择。
            const float zone=std::min(40.0f,absolute_.h*.12f);
            if(local.y<zone||local.y>absolute_.h-zone)Animate();
        }
    }
    PlaceEditor();Invalidate();
}
void PdfCanvas::OnMouseUp(lumen::Point local,uint32_t){
    if(zoom_drag_){const auto r=*zoom_drag_;ZoomBox(false);ZoomToRect(r);(void)local;return;}
    if(scrollbar_drag_){scrollbar_drag_=false;Invalidate();return;}
    if(panning_){
        panning_=false;
        // 常驻手型工具下原地单击仍可打开链接（与 Acrobat 一致）。
        const bool still=std::abs(local.x-pan_origin_.x)+std::abs(local.y-pan_origin_.y)<=4;
        if(still&&hand_tool_&&activate_link){const int page=Hit(local);if(page>=0)if(const Link* link=LinkAt(page,PagePoint(page,local)))activate_link(*link);}
        Invalidate();return;
    }
    if(redact_drag_){
        OnMouseMove(local,0);const auto g=*redact_drag_;redact_drag_.reset();
        const Rect r{std::min(g.start.x,g.current.x),std::min(g.start.y,g.current.y),std::abs(g.current.x-g.start.x),std::abs(g.current.y-g.start.y)};
        const auto screen=ScreenRect(g.page,r);
        if(screen.w>=4&&screen.h>=4&&r.w>=1&&r.h>=1){if(redact_add)redact_add(g.page,r);}
        else if(const int hit=RedactMarkAt(g.page,g.start);hit>=0&&redact_remove)redact_remove(static_cast<size_t>(hit));
        Invalidate();return;
    }
    if(!dragging_||down_page_<0)return;
    // A final mouse-up position may arrive without an intervening move event.
    OnMouseMove(local,0);dragging_=false;
    if(highlight_cancel_)highlight_cancel_->store(true);
    const bool moved=std::abs(local.x-down_local_.x)+std::abs(local.y-down_local_.y)>4;
    if(Grid()){
        if(page_drag_.active)EndPageDrag(true);
        else if(!moved&&collapse_on_up_){selected_pages_={down_page_};if(pages_selected)pages_selected(1);}
        page_drag_.grabbed=-1;collapse_on_up_=false;Invalidate();return;
    }
    if(moving_){
        if(draft_){
            // Callbacks may replace the draft and invalidate moving_. Copy first.
            const auto bounds=moving_->bounds;const bool resized=resizing_&&moved;
            if(moved){draft_->bounds=bounds;if(resized)draft_->fixedTextBox=true;}
            moving_.reset();if(moved&&draft_resized)draft_resized(bounds,resized);
        }
        else if(moved&&update_annotation){
            // 落下：本地先按新位置显示（选框、命中区与预览），文档更新完成、新瓦片到达后预览自动退场。
            auto found=std::find_if(annotations_.begin(),annotations_.end(),[&](const auto& a){return a.id==moving_->id;});
            if(found!=annotations_.end()){
                if(ghost_&&ghost_->request.id==moving_->id)settling_=Settle{down_page_,moving_->id,moving_->bounds,generation_,GetTickCount64()+1500};
                *found=*moving_;
            }
            update_annotation(down_page_,*moving_);
        }
        moving_.reset();RequestVisible();PlaceEditor();Invalidate();return;
    }
    Rect r=Between(down_point_,last_point_);
    if(tool_==Tool::Select){
        const auto link=std::exchange(press_link_,std::nullopt);
        const auto field=std::exchange(press_field_,std::nullopt);
        if(!moved){
            ClearTextSelection();
            if(field&&activate_field){activate_field(field->first,field->second);Invalidate();return;}
            if(link&&activate_link)activate_link(*link);
        }else if(area_select_){
            text_selection_={};selecting_text_=false;
            if(text_selection)text_selection(down_page_,r);
        }else if(selecting_text_){
            // 最终范围再请求一次（不因松手而取消），结果到达后通知界面。终点已由上面的 OnMouseMove 更新（可能在其它页）。
            RequestHighlight();
            if(selection_changed)selection_changed(text_selection_);
        }
    }
    else if(create_annotation){
        if(tool_==Tool::Text&&(!moved||r.w<30))r={down_point_.x,down_point_.y,20,20};
        if(tool_==Tool::Note)r={down_point_.x,down_point_.y,24,24};
        if(tool_==Tool::Image&&(!moved||r.w<30))r={down_point_.x,down_point_.y,160,100};
        if(tool_==Tool::Stamp&&(!moved||r.w<30||r.h<16))r={down_point_.x,down_point_.y,2,2};   // 默认尺寸由印章文字决定
        if(IsLineTool(tool_)&&!moved){stroke_.clear();Invalidate();return;}
        if(tool_==Tool::Ink){
            stroke_.push_back(last_point_);
            float x0=down_point_.x,x1=x0,y0=down_point_.y,y1=y0;
            for(const auto& point:stroke_){x0=std::min(x0,point.x);x1=std::max(x1,point.x);y0=std::min(y0,point.y);y1=std::max(y1,point.y);}
            r={x0,y0,std::max(2.0f,x1-x0),std::max(2.0f,y1-y0)};
        }
        if(IsLineTool(tool_)){stroke_={down_point_,last_point_};r.w=std::max(r.w,2.0f);r.h=std::max(r.h,2.0f);}
        if(IsMarkupTool(tool_)){
            if(!moved){stroke_.clear();Invalidate();return;}
            if(area_highlight_)stroke_.clear();else stroke_={down_point_,last_point_};
            r.w=std::max(2.0f,r.w);r.h=std::max(2.0f,r.h);
        }
        if(tool_==Tool::Text){if(moved&&r.w>=30)stroke_={down_point_,last_point_};else stroke_.clear();}
        if(r.w>=2&&r.h>=2)create_annotation(down_page_,tool_,r,stroke_);
    }
    stroke_.clear();Invalidate();
}
void PdfCanvas::OnMouseLeave(){scrollbar_hover_=false;lens_inside_=false;hover_annotation_=-1;hover_page_=-1;Animate();Invalidate();}
void PdfCanvas::FocusText(int page,Rect bounds){
    if(page<0||page>=static_cast<int>(layout_.size()))return;
    current_page_=page;const auto r=ScreenRect(page,bounds);
    // Entering a visible frame must not jump or zoom the page. A frame selected
    // before scrolling still needs to be revealed when editing from the panel.
    const bool topVisible=r.y>=absolute_.y+12&&r.y<std::max(absolute_.y+12,absolute_.Bottom()-48);
    const bool leftVisible=r.x>=absolute_.x+12&&r.x<std::max(absolute_.x+12,absolute_.Right()-48);
    if(topVisible&&leftVisible)return;
    const auto& l=layout_[page];
    (void)l;if(!topVisible)scroll_=target_scroll_=std::clamp(r.y-absolute_.y+scroll_-std::min(140.0f,absolute_.h*.3f),0.0f,std::max(0.0f,content_height_-absolute_.h));
    if(!leftVisible){
        horizontal_=std::clamp(horizontal_+r.x-(absolute_.x+24),0.0f,std::max(0.0f,content_width_-absolute_.w));LayoutPages();
    }
    UpdateViewport();
}
void PdfCanvas::Draft(std::optional<Annotation> value,int page){
    const int oldHidden=draft_?draft_->id:-1,newHidden=value?value->id:-1;
    draft_=std::move(value);draft_page_=page;moving_.reset();dragging_=false;
    if(oldHidden!=newHidden){
        for(auto& [key,token]:pending_){(void)key;token->store(true);}pending_.clear();RequestVisible();
    }
    target_scroll_=scroll_;PlaceEditor();Invalidate();
}
void PdfCanvas::DrawGhost(lumen::Painter& p,int page)const{
    if(!ghost_||!ghost_->bitmap||ghost_->request.page!=page)return;
    int id=-1;Rect to{};
    if(moving_&&!draft_&&down_page_==page){id=moving_->id;to=moving_->bounds;}
    else if(settling_&&settling_->page==page){id=settling_->id;to=settling_->to;}
    if(id!=ghost_->request.id)return;
    const auto& s=ghost_->sprite.bounds;const Rect& from=ghost_->request.from;
    const auto type=ghost_->request.type;
    if(IsLineTool(type)||IsBoxShape(type)||type==Tool::Ink||IsMarkupTool(type)){
        auto it=std::find_if(annotations_.begin(),annotations_.end(),[id](const auto& a){return a.id==id;});
        if(moving_){DrawVector(p,page,*moving_);return;}
        if(it!=annotations_.end()){DrawVector(p,page,*it);return;}
    }
    const bool scaled=(ghost_->request.type==Tool::Image||ghost_->request.type==Tool::Stamp)&&from.w>0&&from.h>0;   // 图片随框缩放，文字保持原字号
    const Rect dest=scaled?Rect{to.x+(s.x-from.x)*to.w/from.w,to.y+(s.y-from.y)*to.h/from.h,s.w*to.w/from.w,s.h*to.h/from.h}
                          :Rect{s.x+to.x-from.x,s.y+to.y-from.y,s.w,s.h};
    const bool clip=!scaled&&(std::abs(to.w-from.w)>.01f||std::abs(to.h-from.h)>.01f);
    if(clip)p.PushClip(ScreenRect(page,{to.x-2,to.y-2,to.w+4,to.h+4}));
    p.DrawBitmap(ghost_->bitmap.Get(),ScreenRect(page,dest),true);
    if(clip)p.PopClip();
}
void PdfCanvas::PlaceEditor(){
    if(!draft_||!editor_layout||draft_page_<0||draft_page_>=static_cast<int>(layout_.size()))return;
    const auto& value=moving_?*moving_:*draft_;
    editor_layout(ScreenRect(draft_page_,value.bounds),absolute_,layout_[draft_page_].scale,dpi_);
    for(size_t i=0;i<ChildCount();++i)if(ChildVisible(i))ArrangeChildAt(i);
}
int PdfCanvas::HandleAt(const Annotation& a,lumen::Point local)const{
    if(a.type==Tool::Note||a.readOnly||(IsMarkupTool(a.type)&&!a.areaHighlight))return -1;
    if(IsLineTool(a.type)&&a.points.size()==2){
        for(int i=0;i<2;++i){auto r=ScreenRect(annotations_page_,{a.points[i].x,a.points[i].y,0,0});if(std::hypot(local.x+absolute_.x-r.x,local.y+absolute_.y-r.y)<8)return i;}
        return -1;
    }
    const auto r=ScreenRect(draft_?draft_page_:annotations_page_,a.bounds).Inset(-6,-6);
    const lumen::Point point{local.x+absolute_.x,local.y+absolute_.y};
    const lumen::Point handles[]={{r.x,r.y},{r.x+r.w/2,r.y},{r.Right(),r.y},
        {r.x,r.y+r.h/2},{r.Right(),r.y+r.h/2},{r.x,r.Bottom()},{r.x+r.w/2,r.Bottom()},{r.Right(),r.Bottom()}};
    for(int i=0;i<8;++i)if(!(a.type==Tool::Image&&(i==1||i==3||i==4||i==6))&&std::abs(point.x-handles[i].x)<7&&std::abs(point.y-handles[i].y)<7)return PageHandle(i);
    return -1;
}
lumen::CursorShape PdfCanvas::CursorAt(lumen::Point local)const{
    using Shape=lumen::CursorShape;
    if(panning_||page_drag_.active)return Shape::SizeAll;
    if(presenting_)return hover_link_>=0?Shape::Hand:Shape::Arrow;
    if(zoom_box_&&view_==View::Reading&&local.x<absolute_.w-18)return Shape::Cross;
    if(HandActive()&&local.x<absolute_.w-18)return Shape::Hand;
    if(view_!=View::Reading||!editable_)return Shape::Arrow;
    const bool captured=moving_&&dragging_;
    if(!captured&&(scrollbar_drag_||local.x>=absolute_.w-18))return Shape::Arrow;
    if(redact_mode_&&!(local.x>=absolute_.w-18))return redact_hover_>=0&&!redact_drag_?Shape::Hand:Shape::Cross;
    if(!captured&&!draft_&&tool_==Tool::Select&&hover_field_>=0)return Shape::Hand;
    const Annotation* selected=nullptr;int handle=-1;
    if(captured){selected=&*moving_;if(resizing_)handle=resize_handle_;else return Shape::SizeAll;}
    else if(draft_){selected=&*draft_;handle=HandleAt(*draft_,local);}
    else if(tool_==Tool::Select){for(const auto& a:annotations_)if(a.id==selected_annotation_){selected=&a;handle=HandleAt(a,local);break;}}
    if(handle>=0){
        if(selected&&IsLineTool(selected->type))return Shape::Cross;
        const bool swap=Rot()%180!=0;   // 旋转 90 / 270° 时，页面上的竖边显示为横边
        return (handle==1||handle==6)?(swap?Shape::SizeWE:Shape::SizeNS):(handle==3||handle==4)?(swap?Shape::SizeNS:Shape::SizeWE):
            ((handle==0||handle==7)!=swap)?Shape::SizeNWSE:Shape::SizeNESW;
    }
    if(draft_)return ScreenRect(draft_page_,draft_->bounds).Inset(-9,-9).Contains({local.x+absolute_.x,local.y+absolute_.y})?Shape::SizeAll:Shape::Arrow;
    if(tool_==Tool::Text||(IsMarkupTool(tool_)&&!area_highlight_))return Shape::IBeam;
    return hover_annotation_>=0||(tool_==Tool::Select&&hover_link_>=0)?Shape::Hand:Shape::Arrow;
}
void PdfCanvas::DrawSelection(lumen::Painter& p,const Annotation& a,bool hover)const{
    const int page=draft_?draft_page_:annotations_page_;
    auto handle=[&](float x,float y){
        p.FillRoundedRect({x-4.5f,y-4.5f,9,9},4.5f,C(0xffffff));
        p.StrokeRoundedRect({x-4.5f,y-4.5f,9,9},4.5f,C(0x3676dd),1.25f);
    };
    if(IsLineTool(a.type)&&a.points.size()==2){
        auto first=ScreenRect(page,{a.points[0].x,a.points[0].y,0,0}),last=ScreenRect(page,{a.points[1].x,a.points[1].y,0,0});
        if(hover)p.DrawDashedLine({first.x,first.y},{last.x,last.y},C(0x3676dd,.7f),1);
        else{handle(first.x,first.y);handle(last.x,last.y);}return;
    }
    if(IsMarkupTool(a.type)&&!a.areaHighlight){
        for(const auto& q:a.quads){auto r=ScreenRect(page,PointsRect(q.ul,q.lr));p.StrokeRoundedRect(r,1,C(0x3676dd,hover?.5f:.9f),1);}
        return;
    }
    const auto r=ScreenRect(page,a.bounds).Inset(-6,-6);
    p.StrokeRoundedRect(r,2,C(0xffffff,hover?.5f:.95f),3);
    p.StrokeRoundedRect(r,2,C(0x3676dd,hover?.6f:1),1);
    if(hover||a.type==Tool::Note||a.readOnly)return;
    handle(r.x,r.y);handle(r.Right(),r.y);handle(r.x,r.Bottom());handle(r.Right(),r.Bottom());
    if(a.type!=Tool::Image){handle(r.x+r.w/2,r.y);handle(r.x,r.y+r.h/2);handle(r.Right(),r.y+r.h/2);handle(r.x+r.w/2,r.Bottom());}
}
void PdfCanvas::OnMouseDoubleClick(lumen::Point p){
    if(view_==View::Pages){if(!page_drag_.active&&open_page){const int page=Hit(p);if(page>=0)open_page(page);}return;}
    // 三击的第三下（或演示中刚处理过的单击）：指针输入会紧接着再报一次双击，忽略。
    if(GetTickCount64()-triple_tick_<=50)return;
    if(presenting_&&view_==View::Reading){FlipRow(1,true);return;}
    if(!editable_||view_!=View::Reading||draft_)return;
    const bool textTool=tool_==Tool::Select||tool_==Tool::Text;
    const bool noteTool=tool_==Tool::Select||tool_==Tool::Note;
    if(!textTool&&!noteTool)return;
    const int page=Hit(p);if(page<0)return;
    if(!hide_annotations_&&page==annotations_page_){const auto point=PagePoint(page,p);
        for(auto it=annotations_.rbegin();it!=annotations_.rend();++it){
            const bool editableText=it->type==Tool::Text&&textTool;
            const bool editableNote=(it->type==Tool::Note&&noteTool)||(it->type==Tool::Stamp&&tool_==Tool::Select);
            if((editableText||editableNote)&&!it->readOnly&&it->bounds.Contains(point)){
                if(editableText&&Rot()){if(rotated_edit_blocked)rotated_edit_blocked();return;}
                dragging_=false;resizing_=false;resize_handle_=-1;moving_.reset();
                RequestVisible();if(edit_text)edit_text(page,it->id,point);return;
            }
        }
    }
    // 空白文字处双击：选中单词（中文按词）；随后在原处再按一下为三击选段。
    if(tool_==Tool::Select&&!HandActive()){double_tick_=GetTickCount64();double_local_=p;SnapSelect(page,PagePoint(page,p),1);}
}
void PdfCanvas::ClearTextSelection(){
    if(highlight_cancel_&&tool_==Tool::Select)highlight_cancel_->store(true);
    const bool had=text_selection_.page>=0;
    text_selection_={};selecting_text_=false;span_requested_.clear();
    if(had&&selection_changed)selection_changed(text_selection_);
    Invalidate();
}
bool PdfCanvas::HasTextSelection()const{
    if(text_selection_.page<0)return false;
    for(const auto& s:text_selection_.spans)if(!s.quads.empty())return true;
    return false;
}
int PdfCanvas::PageNear(lumen::Point local)const{
    const int hit=Hit(local);if(hit>=0)return hit;
    // 页间空白或视口外：取最近的页，纵向距离优先（拖到两页之间时归上下最近的那页）。
    const lumen::Point c{local.x,local.y+scroll_};int best=-1;float bestDistance=1e30f;
    for(int i:visible_){
        const auto& r=layout_[i].rect;
        const float dx=c.x<r.x?r.x-c.x:c.x>r.Right()?c.x-r.Right():0.0f;
        const float dy=c.y<r.y?r.y-c.y:c.y>r.Bottom()?c.y-r.Bottom():0.0f;
        if(dy*4+dx<bestDistance){bestDistance=dy*4+dx;best=i;}
    }
    return best;
}
void PdfCanvas::UpdateSpans(){
    auto& t=text_selection_;std::vector<TextSpan> next;
    if(t.page>=0&&t.endPage>=0&&t.page<static_cast<int>(pages_.size())&&t.endPage<static_cast<int>(pages_.size())){
        int a=t.page,b=t.endPage;Point pa=t.start,pb=t.end;
        if(a>b){std::swap(a,b);std::swap(pa,pb);}
        if(a==b){TextSpan s;s.page=a;s.start=pa;s.end=pb;next.push_back(std::move(s));}
        else for(int p=a;p<=b;++p){TextSpan s;s.page=p;s.start=p==a?pa:Point{};s.end=p==b?pb:Point{};s.fromStart=p!=a;s.toEnd=p!=b;next.push_back(std::move(s));}
    }
    // 范围未变的页保留已有高亮（拖动时中间页不重复请求）。
    auto same=[](Point x,Point y){return x.x==y.x&&x.y==y.y;};
    for(auto& s:next)for(const auto& old:t.spans)
        if(old.page==s.page&&same(old.start,s.start)&&same(old.end,s.end)&&old.fromStart==s.fromStart&&old.toEnd==s.toEnd){s.quads=old.quads;s.ready=old.ready;break;}
    t.spans=std::move(next);t.quads.clear();
    for(const auto& s:t.spans)if(s.page==t.page)t.quads=s.quads;
}
void PdfCanvas::RequestSpans(){
    if(!request_highlight||!highlight_cancel_||highlight_cancel_->load())return;
    for(const auto& s:text_selection_.spans){
        // 只为可见页请求高亮，滚到别处时由 RequestVisible 补请求；复制按起止点取文字，不依赖高亮。
        if(s.ready||span_requested_.contains(s.page)||std::find(visible_.begin(),visible_.end(),s.page)==visible_.end())continue;
        span_requested_.insert(s.page);
        HighlightRequest q{s.page,s.start,s.end,generation_,highlight_cancel_};q.fromStart=s.fromStart;q.toEnd=s.toEnd;
        request_highlight(q);
    }
}
void PdfCanvas::ExtendSelection(lumen::Point local){
    const int page=PageNear(local);if(page<0||text_selection_.page<0)return;
    const Point point=ClampPoint(PagePoint(page,local),pages_[page]);
    auto& t=text_selection_;
    if(t.endPage==page&&t.end.x==point.x&&t.end.y==point.y)return;
    t.endPage=page;t.end=point;UpdateSpans();RequestHighlight();
}
void PdfCanvas::SnapSelect(int page,Point p,int unit){
    if(!request_highlight||page<0||page>=static_cast<int>(pages_.size()))return;
    if(highlight_cancel_)highlight_cancel_->store(true);
    dragging_=false;press_link_.reset();area_select_=false;selecting_text_=true;
    text_selection_={page,p,p,{},page,{}};UpdateSpans();
    highlight_cancel_=std::make_shared<std::atomic_bool>(false);span_requested_={page};
    HighlightRequest q{page,p,p,generation_,highlight_cancel_};q.snap=unit;request_highlight(q);
    Invalidate();
}
void PdfCanvas::RowRange(int row,float& lo,float& hi)const{
    float top=1e30f,bottom=-1e30f;
    for(const auto& l:layout_)if(l.row==row){top=std::min(top,l.rect.y);bottom=std::max(bottom,l.rect.Bottom());}
    if(top>bottom){lo=hi=scroll_;return;}
    const float view=absolute_.h,limit=std::max(0.0f,content_height_-view);
    if(presenting_&&bottom-top<=view)lo=hi=top-(view-(bottom-top))*.5f;
    else{const float margin=presenting_?0.0f:16.0f;lo=top-margin;hi=std::max(lo,bottom+margin-view);}
    lo=std::clamp(lo,0.0f,limit);hi=std::clamp(hi,lo,limit);
}
void PdfCanvas::PagedClamp(){
    if(!PagedActive())return;
    float lo,hi;RowRange(layout_[std::clamp(current_page_,0,static_cast<int>(layout_.size())-1)].row,lo,hi);
    scroll_=std::clamp(scroll_,lo,hi);target_scroll_=std::clamp(target_scroll_,lo,hi);
}
bool PdfCanvas::FlipRow(int direction,bool toTop){
    if(!PagedActive()||direction==0)return false;
    const int target=layout_[std::clamp(current_page_,0,static_cast<int>(layout_.size())-1)].row+direction;
    int page=-1;for(int i=0;i<static_cast<int>(layout_.size());++i)if(layout_[i].row==target){page=i;break;}
    if(page<0)return false;
    float lo,hi;RowRange(target,lo,hi);
    current_page_=page;scroll_=target_scroll_=toTop?lo:hi;
    UpdateViewport();if(page_changed)page_changed(page);Invalidate();return true;
}
void PdfCanvas::Paged(bool on){
    if(paged_==on)return;
    paged_=on;PagedClamp();RequestVisible();Invalidate();
}
void PdfCanvas::Presenting(bool on){
    if(presenting_==on||view_!=View::Reading)return;
    if(on){
        ZoomBox(false);
        ClearTextSelection();before_present_=CurrentView();
        const int page=current_page_;presenting_=true;hover_link_=-1;hover_annotation_=-1;horizontal_=0;fit_=Fit::Page;
        LayoutPages();GoTo(page);
    }else{
        presenting_=false;
        ViewState state=before_present_.value_or(ViewState{});state.page=current_page_;state.offset=0;before_present_.reset();
        if(!layout_.empty()||!pages_.empty())ApplyView(state);else LayoutPages();
    }
    Invalidate();
}
void PdfCanvas::AcceptLinks(int page,uint64_t generation,std::vector<Link> links,std::vector<FormField> fields){
    if(generation!=generation_||page<0||page>=static_cast<int>(pages_.size()))return;
    links_pending_.erase(page);links_.insert_or_assign(page,LinkPage{generation,std::move(links),std::move(fields)});
    Invalidate();
}
int PdfCanvas::RedactMarkAt(int page,Point p)const{
    for(int i=static_cast<int>(redact_marks_.size())-1;i>=0;--i){const auto& m=redact_marks_[static_cast<size_t>(i)];if(m.page==page&&m.bounds.Contains(p))return i;}
    return -1;
}
const FormField* PdfCanvas::FieldAt(int page,Point p)const{
    auto found=links_.find(page);if(found==links_.end()||found->second.generation!=generation_)return nullptr;
    const auto& list=found->second.fields;
    for(auto it=list.rbegin();it!=list.rend();++it){
        const bool clickable=it->Fillable()||(it->type==FieldType::Signature&&!it->signedField&&!it->readOnly);
        if(clickable&&it->bounds.Contains(p))return &*it;
    }
    return nullptr;
}
void PdfCanvas::RevealRect(int page,Rect bounds){
    if(page<0||page>=static_cast<int>(layout_.size()))return;
    const auto r=ScreenRect(page,bounds);
    if(r.y<absolute_.y+8||r.Bottom()>absolute_.Bottom()-8){
        current_page_=page;
        scroll_=target_scroll_=std::clamp(r.y-absolute_.y+scroll_-absolute_.h*.3f,0.0f,std::max(0.0f,content_height_-absolute_.h));
        UpdateViewport();
    }
    Invalidate();
}
const Link* PdfCanvas::LinkAt(int page,Point p)const{
    auto found=links_.find(page);if(found==links_.end()||found->second.generation!=generation_)return nullptr;
    // 后出现的链接在上层。
    const auto& list=found->second.links;
    for(auto it=list.rbegin();it!=list.rend();++it)if(it->bounds.Contains(p))return &*it;
    return nullptr;
}
bool PdfCanvas::ShowContextMenu(lumen::Point window_dip){
    if(pages_.empty())return false;
    if(presenting_&&view_==View::Reading){FlipRow(-1,true);return true;}
    if(view_==View::Reading){if(!context_menu)return false;context_menu(window_dip);return true;}
    if(!page_menu)return false;
    const int page=Hit({window_dip.x-absolute_.x,window_dip.y-absolute_.y});
    if(page>=0&&std::find(selected_pages_.begin(),selected_pages_.end(),page)==selected_pages_.end()){
        selected_pages_={page};current_page_=page;anchor_page_=page;if(page_changed)page_changed(page);if(pages_selected)pages_selected(1);Invalidate();
    }
    page_menu(window_dip,page);return true;
}
void PdfCanvas::Tone(PageTone tone){
    if(tone==tone_)return;
    tone_=tone;tiles_.clear();DropLens();
    for(auto& [key,token]:pending_){(void)key;token->store(true);}pending_.clear();
    ghost_.reset();ghost_pending_.reset();RequestVisible();Invalidate();
}
PdfCanvas::ViewState PdfCanvas::CurrentView()const{
    ViewState state;state.fit=fit_;state.zoom=zoom_;state.rotation=rotation_;
    if(pending_view_)return *pending_view_;
    if(layout_.empty()||current_page_<0||current_page_>=static_cast<int>(layout_.size())){state.page=current_page_;return state;}
    state.page=current_page_;
    const auto& l=layout_[current_page_];
    state.offset=fit_==Fit::Page?0.0f:std::max(0.0f,(scroll_+16-l.rect.y)/std::max(.01f,l.scale));
    return state;
}
void PdfCanvas::RestoreView(ViewState state){
    zoom_back_.clear();ZoomBox(false);
    if(layout_.empty()||absolute_.w<=1||absolute_.h<=1||view_!=View::Reading){pending_view_=state;current_page_=std::clamp(state.page,0,std::max(0,static_cast<int>(pages_.size())-1));return;}
    pending_view_.reset();ApplyView(state);
}
void PdfCanvas::ApplyView(const ViewState& state){
    if(pages_.empty())return;
    fit_=state.fit;zoom_=std::clamp(std::isfinite(state.zoom)?state.zoom:1.0f,.1f,8.0f);horizontal_=0;rotation_=((state.rotation%360)+360)%360/90*90;LayoutPages();
    if(layout_.empty()){pending_view_=state;return;}
    const int page=std::clamp(state.page,0,static_cast<int>(layout_.size())-1);
    const auto& l=layout_[page];
    const float offset=std::isfinite(state.offset)?std::clamp(state.offset,0.0f,DisplayHeight(pages_[page])):0.0f;
    current_page_=page;
    scroll_=target_scroll_=std::clamp(l.rect.y-16+offset*l.scale,0.0f,std::max(0.0f,content_height_-absolute_.h));
    UpdateViewport();if(zoom_changed)zoom_changed(ActualZoom());
}
void PdfCanvas::GoToPoint(int page,float y){
    if(page<0||page>=static_cast<int>(layout_.size()))return;
    if(!std::isfinite(y)||fit_==Fit::Page){GoTo(page);return;}
    if(Rot()%180){GoTo(page);return;}   // 旋转 90 / 270°：页内纵坐标变成横向，只到页首
    const auto& l=layout_[page];
    const float offset=std::clamp(y-pages_[page].originY,0.0f,pages_[page].height);
    current_page_=page;
    // 旋转 180°：目标在显示框中自下而上，放到视口下部。
    const float target=Rot()==180?l.rect.y+(pages_[page].height-offset)*l.scale-absolute_.h+24:l.rect.y+offset*l.scale-24;
    scroll_=target_scroll_=std::clamp(target,0.0f,std::max(0.0f,content_height_-absolute_.h));
    UpdateViewport();
}
void PdfCanvas::OnFileDrop(std::vector<std::wstring> paths){std::vector<fs::path> files;for(auto& p:paths)files.emplace_back(std::move(p));if(files_dropped)files_dropped(std::move(files));}
// —— 网格拖动重排 ——
lumen::Rect PdfCanvas::PageRect(int page)const{
    if(Grid()&&page>=0&&page<static_cast<int>(shown_.size()))return shown_[page];
    return layout_[page].rect;
}
bool PdfCanvas::InDragGroup(int page)const{return std::find(page_drag_.group.begin(),page_drag_.group.end(),page)!=page_drag_.group.end();}
bool PdfCanvas::Lifted(int page)const{return std::find(lifted_.begin(),lifted_.end(),page)!=lifted_.end();}
std::vector<int> PdfCanvas::DragOrder(int insert)const{
    std::vector<int> order;order.reserve(pages_.size());
    for(int i=0;i<static_cast<int>(pages_.size());++i)if(!InDragGroup(i))order.push_back(i);
    insert=std::clamp(insert,0,static_cast<int>(order.size()));
    order.insert(order.begin()+insert,page_drag_.group.begin(),page_drag_.group.end());
    return order;
}
int PdfCanvas::DropIndex(lumen::Point c)const{
    // 光标所在格子：左半（单列时上半）插到它前面，右半（下半）插到它后面；落在拖动组自己的空位上保持不变。
    const int n=static_cast<int>(layout_.size());
    if(grid_rows_.empty()||n==0)return page_drag_.insert;
    std::vector<int> order=order_;if(static_cast<int>(order.size())!=n){order.resize(n);std::iota(order.begin(),order.end(),0);}
    const int rows=static_cast<int>(grid_rows_.size());int row=rows-1;
    for(int i=0;i+1<rows;++i)if(c.y<grid_rows_[i+1]-12){row=i;break;}
    const int columns=std::max(1,grid_columns_);const float cell=std::max(1.0f,grid_cell_);
    const int col=std::clamp(static_cast<int>(std::floor((c.x-12)/cell)),0,columns-1);
    int pos=row*columns+col;bool after=false;
    if(pos>=n){pos=n-1;after=true;}
    else if(columns==1){const auto& r=layout_[order[pos]].rect;after=c.y>r.y+r.h*.5f;}
    else after=c.x>12+col*cell+cell*.5f;
    if(InDragGroup(order[pos]))return page_drag_.insert;
    int k=0;for(int i=0;i<pos;++i)if(!InDragGroup(order[i]))++k;
    return k+(after?1:0);
}
void PdfCanvas::BeginPageDrag(){
    const int n=static_cast<int>(pages_.size());const int grabbed=page_drag_.grabbed;
    if(grabbed<0||grabbed>=n||n<2||static_cast<int>(layout_.size())!=n)return;
    std::vector<int> group;
    if(std::find(selected_pages_.begin(),selected_pages_.end(),grabbed)!=selected_pages_.end())
        for(int p:selected_pages_)if(p>=0&&p<n)group.push_back(p);
    if(group.empty())group={grabbed};
    std::sort(group.begin(),group.end());group.erase(std::unique(group.begin(),group.end()),group.end());
    if(static_cast<int>(group.size())>=n)return;   // 全部选中时拖动不改变顺序
    page_drag_.group=std::move(group);page_drag_.active=true;collapse_on_up_=false;hover_page_=-1;
    // 初始插入位置 = 组内第一页的原位置；不连续的多选会先平滑聚拢到这里。
    page_drag_.insert=page_drag_.group.front();
    order_=DragOrder(page_drag_.insert);lifted_=page_drag_.group;motion_=true;
    LayoutPages();UpdatePageDrag();Animate();
}
void PdfCanvas::UpdatePageDrag(){
    if(!page_drag_.active||shown_.size()!=layout_.size())return;
    const lumen::Point content{page_drag_.mouse.x,page_drag_.mouse.y+scroll_};
    const int insert=DropIndex(content);
    if(insert!=page_drag_.insert){page_drag_.insert=insert;order_=DragOrder(insert);LayoutPages();}
    // 抓住的页面跟手（不缓动，避免拖拽发飘）；同组其它页在它身后错开叠放。
    const int grabbed=page_drag_.grabbed;const auto& t=layout_[grabbed].rect;
    const lumen::Rect card{content.x-std::clamp(page_drag_.grab.x,0.0f,t.w),content.y-std::clamp(page_drag_.grab.y,0.0f,t.h),t.w,t.h};
    shown_[grabbed]=card;int rank=0;
    for(int page:page_drag_.group){
        if(page==grabbed)continue;
        const auto& o=layout_[page].rect;const float d=5.0f*static_cast<float>(std::min(++rank,3));
        shown_[page]={card.x+(card.w-o.w)*.5f+d,card.y+(card.h-o.h)*.5f+d,o.w,o.h};
    }
    Invalidate();
}
void PdfCanvas::EndPageDrag(bool commit){
    if(!page_drag_.active)return;
    const auto order=order_.size()==pages_.size()?order_:DragOrder(page_drag_.insert);
    const auto group=page_drag_.group;const int grabbed=page_drag_.grabbed;
    page_drag_.active=false;page_drag_.grabbed=-1;order_.clear();
    bool changed=false;for(size_t i=0;i<order.size();++i)if(order[i]!=static_cast<int>(i)){changed=true;break;}
    bool accepted=false;
    if(commit&&changed){
        std::vector<int> to(order.size());for(size_t pos=0;pos<order.size();++pos)to[order[pos]]=static_cast<int>(pos);
        std::vector<int> selected;for(int p:group)selected.push_back(to[p]);std::sort(selected.begin(),selected.end());
        const int current=to[grabbed];
        if(reorder_pages)accepted=reorder_pages(order,selected,current);
        else if(reorder_page&&group.size()==1){reorder_page(grabbed,current);accepted=true;}
        if(accepted){
            // 本地先按新顺序重排（位置、瓦片、选择一起换号），浮起的页面从松手处飞入新位置；文档更新到达后无缝衔接。
            PreviewOrder(order);selected_pages_=selected;current_page_=current;anchor_page_=current;
            if(pages_selected)pages_selected(selected_pages_.size());
        }
    }
    page_drag_.group.clear();
    if(!accepted)LayoutPages();   // 取消或未接受：回到原顺序，页面沿同一缓动退回
    motion_=true;Animate();Invalidate();
}
void PdfCanvas::PreviewOrder(const std::vector<int>& order){
    const int n=static_cast<int>(pages_.size());if(static_cast<int>(order.size())!=n||n==0)return;
    std::vector<int> to(n,-1);
    for(int pos=0;pos<n;++pos){const int p=order[pos];if(p<0||p>=n||to[p]>=0)return;to[p]=pos;}
    auto remap=[&](int p){return p>=0&&p<n?to[p]:p;};
    const bool reading=view_==View::Reading&&!layout_.empty();
    ViewState state;if(reading)state=CurrentView();
    {std::vector<PageInfo> pages(n);for(int pos=0;pos<n;++pos)pages[pos]=pages_[order[pos]];pages_=std::move(pages);}
    if(static_cast<int>(shown_.size())==n){std::vector<lumen::Rect> v(n);for(int pos=0;pos<n;++pos)v[pos]=shown_[order[pos]];shown_=std::move(v);}
    if(static_cast<int>(hover_mix_.size())==n){std::vector<float> v(n);for(int pos=0;pos<n;++pos)v[pos]=hover_mix_[order[pos]];hover_mix_=std::move(v);}
    DropLens();{std::map<TileKey,Tile> tiles;for(auto& [key,tile]:tiles_){TileKey k=key;k.page=remap(k.page);tile.request.key.page=k.page;tiles.insert_or_assign(k,std::move(tile));}tiles_=std::move(tiles);}
    for(auto& [key,token]:pending_){(void)key;token->store(true);}pending_.clear();wanted_.clear();
    {std::map<int,TextPage> layers;for(auto& [p,layer]:text_pages_)layers.insert_or_assign(remap(p),std::move(layer));text_pages_=std::move(layers);}
    {std::map<int,LinkPage> links;for(auto& [p,list]:links_)links.insert_or_assign(remap(p),std::move(list));links_=std::move(links);}
    links_pending_.clear();hover_link_=-1;hover_link_page_=-1;press_link_.reset();hover_field_=-1;hover_field_page_=-1;press_field_.reset();
    for(auto& p:selected_pages_)p=remap(p);std::sort(selected_pages_.begin(),selected_pages_.end());
    current_page_=remap(current_page_);anchor_page_=remap(anchor_page_);down_page_=remap(down_page_);
    annotations_page_=remap(annotations_page_);draft_page_=remap(draft_page_);hover_page_=-1;
    for(auto& p:lifted_)p=remap(p);for(auto& p:page_drag_.group)p=remap(p);page_drag_.grabbed=remap(page_drag_.grabbed);
    for(auto& h:search_hits_)h.page=remap(h.page);if(search_)search_->page=remap(search_->page);
    text_selection_={};selecting_text_=false;ghost_.reset();ghost_pending_.reset();settling_.reset();order_.clear();
    if(reading){state.page=remap(state.page);ApplyView(state);}else LayoutPages();
    Invalidate();
}
void PdfCanvas::ResetTiles(){
    tiles_.clear();for(auto& [key,token]:pending_){(void)key;token->store(true);}pending_.clear();
    RequestVisible();Invalidate();
}
void PdfCanvas::HideAnnotations(bool hide){
    if(hide_annotations_==hide)return;hide_annotations_=hide;selected_annotation_=-1;hover_annotation_=-1;settling_.reset();ghost_.reset();ResetTiles();
}
}