#include "app/canvas.h"
#include "core/annotation_geometry.h"
#include <lumen/App.h>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace lpdf {
static int canvasAssertions=0;
static void Check(bool value,const char* what){++canvasAssertions;if(!value)throw std::runtime_error(what);}
struct CanvasTestAccess{
static void Run(){
    PdfCanvas canvas;
    canvas.request_tile=[](TileRequest){};
    canvas.DocumentPages(std::vector<PageInfo>(1000,{595,842,0,0}),1);
    canvas.Arrange({0,0,900,700});
    UINT lines=3;SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
    const float step=lines==WHEEL_PAGESCROLL?595.0f:32.0f*lines;
    canvas.OnWheel(-1);
    Check(std::abs(canvas.target_scroll_-step)<.1f,"wheel notch must use logical steps, not subpixel distance");
    Check(canvas.scroll_==0,"discrete wheel should animate");
    for(int i=0;i<60;++i)canvas.OnAnimate(1.0f/120);
    Check(std::abs(canvas.scroll_-step)<.2f,"wheel animation did not settle");
    for(int i=0;i<12;++i)canvas.OnWheel(-1);
    for(int i=0;i<120;++i)canvas.OnAnimate(1.0f/120);
    Check(std::abs(canvas.scroll_-13*step)<.2f,"rapid wheel input lost distance");
    if(step>0)Check(canvas.current_page_>0,"long document never advances a page");
    canvas.GoTo(0);canvas.OnWheel(-.1f);
    Check(std::abs(canvas.scroll_-step*.1f)<.1f,"precision wheel delta discarded");
    canvas.OnMouseDown({895,350},1);
    canvas.OnMouseMove({895,695},1);canvas.OnMouseUp({895,695},0);
    Check(canvas.scroll_>canvas.content_height_-1500,"scrollbar cannot reach long document end");
    canvas.OnMouseDown({895,5},1);canvas.OnMouseMove({895,0},1);canvas.OnMouseUp({895,0},0);
    Check(canvas.scroll_==0,"scrollbar cannot return to top");
    canvas.Scroll(-1000);for(int i=0;i<60;++i)canvas.OnAnimate(1.0f/60);
    Check(canvas.scroll_==0,"scroll beyond top");
    // 手型工具：拖动跟手平移（不走动画），松开不留文字选择；中键拖动同样平移；Enter 下一屏（Acrobat）。
    canvas.HandTool(true);
    Check(canvas.CursorAt({400,300})==lumen::CursorShape::Hand,"hand tool must show the hand cursor");
    canvas.OnMouseDown({400,500},1);canvas.OnMouseMove({400,200},1);
    Check(std::abs(canvas.scroll_-300)<.1f&&canvas.CursorAt({400,200})==lumen::CursorShape::SizeAll,"hand drag must scroll with the pointer");
    canvas.OnMouseUp({400,200},0);
    Check(!canvas.panning_&&!canvas.HasTextSelection(),"hand drag left a selection or capture");
    canvas.HandTool(false);
    Check(canvas.CursorAt({400,300})!=lumen::CursorShape::Hand,"hand cursor must end with the hand tool");
    canvas.OnMouseDown({400,200},MK_MBUTTON);canvas.OnMouseMove({400,400},MK_MBUTTON);canvas.OnMouseUp({400,400},0);
    Check(std::abs(canvas.scroll_-100)<.1f&&!canvas.panning_,"middle-button drag must pan");
    canvas.GoTo(0);canvas.OnKey(VK_RETURN);
    Check(std::abs(canvas.target_scroll_-595)<1,"Enter must advance one screen");
    canvas.GoTo(0);
    {
        // 横向平移只按屏幕上的页面计算：宽页可左右拖，窄页不能被拖进右侧空白；离开宽页后回正。
        PdfCanvas wide;wide.request_tile=[](TileRequest){};
        std::vector<PageInfo> mixed(20,{595,842,0,0});mixed[0]={2400,842,0,0};
        wide.DocumentPages(mixed,1);wide.Arrange({0,0,900,700});wide.ActualSize();wide.GoTo(0);wide.HandTool(true);
        wide.OnMouseDown({400,300},1);wide.OnMouseMove({100,300},1);wide.OnMouseUp({100,300},0);
        Check(std::abs(wide.horizontal_-300)<.5f,"wide page must pan horizontally");
        wide.GoTo(10);wide.UpdateViewport();
        Check(wide.horizontal_==0,"narrow pages must re-center after leaving the wide page");
        wide.OnMouseDown({400,300},1);wide.OnMouseMove({100,300},1);wide.OnMouseUp({100,300},0);
        Check(wide.horizontal_==0,"narrow page must not be dragged into the empty margin");
        wide.OnHWheel(3);
        Check(wide.horizontal_==0,"horizontal wheel must not push a fitting page aside");
    }
    // Fit must constrain both portrait and landscape pages and react to viewport changes.
    canvas.DocumentPages({{595,842,0,0},{1200,400,0,0}},2);canvas.FitPage();
    for(const auto& l:canvas.layout_)Check(l.rect.w<=848.1f&&l.rect.h<=660.1f,"fit overflow");
    canvas.Arrange({0,0,500,400});
    for(const auto& l:canvas.layout_)Check(l.rect.w<=448.1f&&l.rect.h<=360.1f,"resized fit overflow");
    canvas.Arrange({0,0,900,700});canvas.GoTo(0);
    Annotation a;a.id=9;a.type=Tool::Text;a.bounds={100,100,180,50};
    canvas.Selection(0,{a},9);
    auto r=canvas.ScreenRect(0,a.bounds).Inset(-6,-6);
    const lumen::Point handles[]={{r.x,r.y},{r.x+r.w/2,r.y},{r.Right(),r.y},
        {r.x,r.y+r.h/2},{r.Right(),r.y+r.h/2},{r.x,r.Bottom()},{r.x+r.w/2,r.Bottom()},{r.Right(),r.Bottom()}};
    for(int i=0;i<8;++i)Check(canvas.HandleAt(a,handles[i])==i,"eight text handles missing");
    canvas.OnMouseDown(handles[0],1);
    Check(canvas.resizing_,"top-left text handle missing");
    canvas.OnMouseMove({r.x-30,r.y-15},1);
    Check(canvas.moving_&&canvas.moving_->bounds.w>a.bounds.w,"text frame did not resize");
    canvas.OnMouseUp({r.x-30,r.y-15},0);
    // 拖动实时预览：按下即请求批注外观位图；预览就绪前瓦片仍显示原位文字；就绪后瓦片改为隐藏该批注并由预览跟随。
    std::vector<TileRequest> tiles;std::vector<SpriteRequest> sprites;
    canvas.request_tile=[&](TileRequest t){tiles.push_back(t);};
    canvas.request_sprite=[&](SpriteRequest s){sprites.push_back(s);};
    canvas.Selection(0,{a},9);canvas.GoTo(0);
    r=canvas.ScreenRect(0,a.bounds);
    canvas.OnMouseDown({r.x+20,r.y+20},1);
    Check(sprites.size()==1&&sprites[0].id==9&&sprites[0].page==0,"press on text must request a drag sprite");
    Check(std::any_of(tiles.begin(),tiles.end(),[](const TileRequest& t){return t.key.page==0&&t.hiddenAnnotation==9;}),"press must request tiles with the dragged text hidden");
    canvas.OnMouseMove({r.x+60,r.y+50},1);
    Check(canvas.moving_&&std::abs(canvas.moving_->bounds.x-(a.bounds.x+40/canvas.layout_[0].scale))<.5f,"drag preview did not follow pointer");
    Sprite sprite;sprite.bounds={a.bounds.x-2,a.bounds.y-2,a.bounds.w+4,a.bounds.h+4};sprite.pixels.width=4;sprite.pixels.height=4;sprite.pixels.stride=16;sprite.pixels.bgra.assign(64,0);
    canvas.AcceptSprite(sprites[0],sprite);
    Check(canvas.ghost_&&canvas.ghost_->request.id==9,"sprite was not accepted for the dragged text");
    int updated=-1;Rect dropped{};canvas.update_annotation=[&](int,Annotation v){updated=v.id;dropped=v.bounds;};
    canvas.OnMouseUp({r.x+60,r.y+50},0);
    Check(updated==9&&std::abs(dropped.x-(a.bounds.x+40/canvas.layout_[0].scale))<.5f,"drop did not commit the moved bounds");
    Check(canvas.settling_&&canvas.settling_->id==9,"drop must keep the preview until fresh tiles arrive");
    Check(!canvas.moving_,"drag state must end on drop");
    // 文档更新后（新版本），全部完整瓦片到达时预览退场。
    tiles.clear();canvas.DocumentPages({{595,842,0,0},{1200,400,0,0}},3);canvas.Selection(0,{a},9);canvas.GoTo(0);
    Check(canvas.settling_.has_value(),"preview must survive the document refresh until new tiles arrive");
    Check(!tiles.empty()&&std::all_of(tiles.begin(),tiles.end(),[](const TileRequest& t){return t.hiddenAnnotation<0;}),"new generation must request full tiles");
    for(const auto& t:tiles){Bitmap b;b.width=b.height=1;b.stride=4;b.bgra.assign(4,255);canvas.AcceptTile(t,b);}
    Check(!canvas.settling_,"preview did not retire after fresh tiles arrived");
    canvas.request_tile=[](TileRequest){};canvas.request_sprite={};
    bool edited=false;canvas.edit_text=[&](int page,int id,Point){edited=page==0&&id==9;};
    r=canvas.ScreenRect(0,a.bounds);canvas.OnMouseDoubleClick({r.x+10,r.y+10});
    Check(edited,"double click did not enter text edit");
    canvas.Draft(a,0);const auto before=canvas.scroll_;canvas.OnWheel(-1);
    for(int i=0;i<30;++i)canvas.OnAnimate(1.0f/60);
    Check(canvas.scroll_!=before,"page scrolling was blocked while editing text");
    canvas.Draft({});

    // The six restored tools share selection, move/resize, cancellation and deletion.
    canvas.DocumentPages({{595,842,0,0}},4);canvas.Arrange({0,0,900,700});canvas.GoTo(0);canvas.EditingTool(Tool::Select);
    auto screen=[&](Point p){auto r=canvas.ScreenRect(0,{p.x,p.y,0,0});return lumen::Point{r.x,r.y};};
    Annotation shape;shape.id=20;shape.type=Tool::Rectangle;shape.bounds={100,100,150,70};
    canvas.Selection(0,{shape},-1);
    canvas.OnMouseDown(screen({175,135}),1);Check(!canvas.moving_,"empty rectangle captured its interior");canvas.OnMouseUp(screen({175,135}),0);
    canvas.OnMouseDown(screen({101,135}),1);Check(canvas.moving_&&canvas.moving_->type==Tool::Rectangle,"rectangle outline cannot be selected");
    canvas.OnMouseMove(screen({121,145}),1);Check(std::abs(canvas.moving_->bounds.x-120)<.1f,"rectangle did not follow the pointer");canvas.OnMouseUp(screen({121,145}),0);
    shape.type=Tool::Arrow;shape.id=21;shape.bounds={100,200,150,60};shape.points={{100,260},{250,200}};
    canvas.Selection(0,{shape},21);
    Check(canvas.HandleAt(shape,screen(shape.points[0]))==0&&canvas.HandleAt(shape,screen(shape.points[1]))==1,"arrow endpoint handles missing");
    Annotation committed;canvas.update_annotation=[&](int,Annotation a){committed=a;};
    canvas.OnMouseDown(screen(shape.points[1]),1);canvas.OnMouseMove(screen({275,220}),1);canvas.OnMouseUp(screen({275,220}),0);
    Check(committed.geometryEdited&&PointDistance(committed.points[0],shape.points[0])<.1f&&PointDistance(committed.points[1],{275,220})<.1f,"arrow endpoint edit moved the opposite endpoint");
    shape.id=22;shape.type=Tool::Image;shape.bounds={100,350,160,80};shape.points.clear();canvas.Selection(0,{shape},22);
    r=canvas.ScreenRect(0,shape.bounds).Inset(-6,-6);
    Check(canvas.HandleAt(shape,{r.Right(),r.y+r.h/2})==-1,"image should not have stretching edge handles");
    canvas.OnMouseDown({r.Right(),r.Bottom()},1);canvas.OnMouseMove({r.Right()+40,r.Bottom()+25},1);
    Check(canvas.moving_&&std::abs(canvas.moving_->bounds.w/canvas.moving_->bounds.h-2)<.01f,"image corner distorted its aspect ratio");canvas.OnMouseUp({r.Right()+40,r.Bottom()+25},0);
    shape.id=23;shape.type=Tool::Note;shape.bounds={350,100,24,24};canvas.Selection(0,{shape},23);
    Check(canvas.HandleAt(shape,screen({350,100}))==-1,"note icon must not expose resize handles");
    bool noteEdit=false,deleted=false;canvas.edit_text=[&](int,int id,Point){noteEdit=id==23;};canvas.delete_annotation=[&](int,int id){deleted=id==23;};
    canvas.OnMouseDoubleClick(screen({360,110}));Check(noteEdit,"double-click note did not open its editor");canvas.OnKey(VK_DELETE);Check(deleted,"Delete key did not delete selected annotation");
    int created=0;Tool createdTool=Tool::Select;std::vector<Point> createdPoints;
    canvas.create_annotation=[&](int,Tool t,Rect,std::vector<Point> points){++created;createdTool=t;createdPoints=std::move(points);};
    canvas.Selection(0,{});canvas.EditingTool(Tool::Ink);
    canvas.OnMouseDown(screen({60,450}),1);canvas.OnMouseUp(screen({60,450}),0);
    Check(created==1&&createdTool==Tool::Ink&&!createdPoints.empty(),"a pen tap must produce an editable dot");
    canvas.OnMouseDown(screen({60,470}),1);canvas.OnMouseMove(screen({240,470}),1);canvas.OnMouseUp(screen({240,470}),0);
    Check(created==2&&createdPoints.back().x>239,"horizontal freehand stroke was discarded");
    canvas.EditingTool(Tool::Rectangle);canvas.OnMouseDown(screen({300,400}),1);canvas.OnMouseMove(screen({430,490}),1);
    canvas.EditingTool(Tool::Select);canvas.OnMouseUp(screen({430,490}),0);Check(created==2,"cancelled drawing mutated the PDF");
    std::vector<HighlightRequest> requests;canvas.request_highlight=[&](HighlightRequest q){requests.push_back(q);};canvas.EditingTool(Tool::Highlight);
    canvas.OnMouseDown(screen({60,50}),1);canvas.OnMouseMove(screen({270,70}),1);
    Check(requests.size()>=2&&requests.front().cancelled->load(),"obsolete highlight work not cancelled");
    const std::vector<Quad> marks{{{60,50},{150,50},{60,64},{150,64}},{{60,70},{250,70},{60,84},{250,84}}};
    canvas.AcceptHighlight(requests.back(),marks);Check(canvas.highlight_preview_.size()==2,"multi-line preview did not arrive");
    canvas.OnMouseUp(screen({270,70}),0);Check(createdTool==Tool::Highlight&&createdPoints.size()==2,"highlight lost selection endpoints");
    canvas.AcceptHighlight(requests.back(),marks);Check(!canvas.dragging_,"late preview restarted a finished gesture");
    canvas.OnMouseDown(screen({50,500}),1);canvas.area_highlight_=true;canvas.OnMouseMove(screen({180,560}),1);canvas.OnMouseUp(screen({180,560}),0);
    Check(createdTool==Tool::Highlight&&createdPoints.empty(),"area highlight was incorrectly sent as text selection");

    // Eight independent axes/anchors, not just eight drawn handles.
    canvas.Draft({});canvas.DocumentPages({{595,842,0,0}},8);canvas.Arrange({0,0,900,700});canvas.GoTo(0);canvas.EditingTool(Tool::Select);
    Annotation text;text.id=31;text.type=Tool::Text;text.bounds={140,240,180,75};text.fontSize=18;
    auto isClose=[](float a,float b){return std::abs(a-b)<.12f;};
    for(int i=0;i<8;++i){
        canvas.Selection(0,{text},text.id);const auto frame=canvas.ScreenRect(0,text.bounds).Inset(-6,-6);
        const lumen::Point grips[]={{frame.x,frame.y},{frame.x+frame.w/2,frame.y},{frame.Right(),frame.y},
            {frame.x,frame.y+frame.h/2},{frame.Right(),frame.y+frame.h/2},{frame.x,frame.Bottom()},{frame.x+frame.w/2,frame.Bottom()},{frame.Right(),frame.Bottom()}};
        const bool left=i==0||i==3||i==5,top=i<=2,vertical=i==1||i==6,horizontal=i==3||i==4;
        auto expectedCursor=vertical?lumen::CursorShape::SizeNS:horizontal?lumen::CursorShape::SizeWE:(i==0||i==7)?lumen::CursorShape::SizeNWSE:lumen::CursorShape::SizeNESW;
        Check(canvas.CursorAt(grips[i])==expectedCursor,"native eight-way cursor is wrong");
        if(vertical)Check(canvas.CursorAt(grips[i])==lumen::CursorShape::SizeNS,"vertical fallback cursor must not be horizontal");
        const float scale=canvas.layout_[0].scale;
        const lumen::Point to{grips[i].x+(vertical?0:left?-24:24)*scale,grips[i].y+(horizontal?0:top?-18:18)*scale};
        committed={};canvas.update_annotation=[&](int,Annotation value){committed=value;};
        canvas.OnMouseDown(grips[i],1);
        Check(canvas.CursorAt({-20,-20})==expectedCursor,"resize cursor changed while captured outside canvas");
        canvas.OnMouseUp(to,0);
        Check(isClose(committed.bounds.w,text.bounds.w+(vertical?0:24)),"resize changed the wrong width axis");
        Check(isClose(committed.bounds.h,text.bounds.h+(horizontal?0:18)),"resize changed the wrong height axis");
        Check(isClose(committed.bounds.x,text.bounds.x-(left?24:0)),"resize moved opposite horizontal anchor");
        Check(isClose(committed.bounds.y,text.bounds.y-(top?18:0)),"resize moved opposite vertical anchor");
        Check(committed.fixedTextBox&&isClose(committed.fontSize,18),"text resize must lock frame without scaling font size");
    }
    // Even very small old frames retain the unaffected dimension.
    text.bounds.w=16;text.bounds.h=12;canvas.Selection(0,{text},text.id);
    auto tiny=canvas.ScreenRect(0,text.bounds).Inset(-6,-6);auto bottom=lumen::Point{tiny.x+tiny.w/2,tiny.Bottom()};
    canvas.OnMouseDown(bottom,1);canvas.OnMouseUp({bottom.x,bottom.y+40},0);
    Check(isClose(committed.bounds.w,16),"top/bottom handle widened a narrow legacy frame");
    text.bounds={140,240,180,75};canvas.Draft(text,0);
    bool draftResized=false;int draftChanges=0;
    canvas.draft_resized=[&](Rect bounds,bool resized){++draftChanges;draftResized=resized;auto next=text;next.bounds=bounds;next.fixedTextBox=resized;canvas.Draft(next,0);};
    auto draftFrame=canvas.ScreenRect(0,text.bounds).Inset(-6,-6);bottom={draftFrame.x+draftFrame.w/2,draftFrame.Bottom()};
    canvas.OnMouseDown(bottom,1);canvas.OnMouseUp({bottom.x,bottom.y+35},0);
    Check(draftChanges==1&&draftResized&&canvas.draft_->bounds.h>text.bounds.h,"draft vertical resize not committed safely");
    canvas.Draft(text,0);auto inner=canvas.ScreenRect(0,text.bounds);lumen::Point border{inner.x+inner.w*.25f,inner.y-6};
    canvas.OnMouseDown(border,1);canvas.OnMouseUp({border.x+20,border.y+10},0);
    Check(draftChanges==2&&!draftResized&&!canvas.draft_->fixedTextBox,"moving draft incorrectly locked its height");
    canvas.Draft({});canvas.EditingTool(Tool::Text);canvas.Selection(0,{text},-1);
    int directEdits=0,newTexts=0,commits=0;Point caret{};
    canvas.create_annotation=[&](int,Tool,Rect,std::vector<Point>){++newTexts;};
    canvas.edit_text=[&](int page,int id,Point point){Check(page==0&&id==text.id,"wrong direct-edit target");++directEdits;caret=point;canvas.Draft(text,0);};
    inner=canvas.ScreenRect(0,text.bounds);lumen::Point click{inner.x+10,inner.y+10};
    canvas.OnMouseDown(click,1);canvas.OnMouseUp(click,0);
    Check(directEdits==0&&newTexts==0,"single click must select existing text without editing or overlay creation");
    canvas.OnMouseDoubleClick(click);
    Check(directEdits==1&&newTexts==0,"double click did not enter the existing text draft");
    Check(caret.x>text.bounds.x&&caret.y>text.bounds.y,"double click did not retain caret position");
    canvas.commit_text=[&]{++commits;canvas.Draft({});};
    canvas.OnMouseDown(screen({500,700}),1);canvas.OnMouseUp(screen({500,700}),0);
    Check(commits==1&&newTexts==0,"blank click did not commit exactly once");
    // Both user tool states enter the same page editor. Other tools, hidden
    // views and disabled/locked annotations must not leak into that path.
    canvas.Draft({});text.readOnly=false;
    for(auto tool:{Tool::Select,Tool::Text}){
        canvas.EditingTool(tool);canvas.Selection(0,{text},text.id);
        const int count=directEdits;const float zoom=canvas.ActualZoom(),scroll=canvas.scroll_;
        canvas.OnMouseDoubleClick(click);
        Check(directEdits==count+1&&canvas.draft_.has_value(),"Select/Text double-click paths disagree");
        Check(canvas.tool_==tool&&canvas.ActualZoom()==zoom&&canvas.scroll_==scroll,"double-click entry reset tool, zoom or scroll");
        canvas.Draft({});
    }
    const int beforeIgnored=directEdits;
    canvas.EditingTool(Tool::Rectangle);canvas.OnMouseDoubleClick(click);
    canvas.EditingTool(Tool::Select);canvas.Editable(false);canvas.OnMouseDoubleClick(click);canvas.Editable(true);
    Check(directEdits==beforeIgnored,"unrelated or disabled tool entered text editing");
    canvas.EditingTool(Tool::Text);
    text.readOnly=true;canvas.Selection(0,{text},-1);canvas.OnMouseDown(click,1);canvas.OnMouseUp(click,0);
    Check(directEdits==beforeIgnored&&newTexts==0,"locked text was edited or covered by a new box");

    canvas.Draft({});canvas.EditingTool(Tool::Select);canvas.FitPage();canvas.GoTo(0);
    const float entryZoom=canvas.zoom_,entryScroll=canvas.scroll_;
    canvas.FocusText(0,{140,240,180,75});
    Check(canvas.zoom_==entryZoom&&canvas.scroll_==entryScroll,"visible text edit must not change zoom or scroll");
    canvas.Zoom(2);canvas.Scroll(500,false);const float scrolledZoom=canvas.zoom_;
    canvas.FocusText(0,{60,70,160,60});const auto revealed=canvas.ScreenRect(0,{60,70,160,60});
    Check(canvas.zoom_==scrolledZoom&&revealed.y>=canvas.absolute_.y&&revealed.y<canvas.absolute_.Bottom(),"offscreen text was not revealed without a zoom change");

    // 适合宽度：页面宽度贴合视口（留边距），不受高度约束；之后 Ctrl+0 回到整页适配。
    PdfCanvas reader;std::vector<TileRequest> order;int changed=-1;
    reader.request_tile=[&](TileRequest t){order.push_back(t);};reader.page_changed=[&](int p){changed=p;};
    reader.DocumentPages(std::vector<PageInfo>(20,{595,842,0,0}),3);reader.Arrange({0,0,900,700});
    reader.FitWidth();
    Check(reader.FitMode()==PdfCanvas::Fit::Width&&std::abs(reader.layout_[0].rect.w-848)<1,"fit width does not fill the viewport");
    Check(reader.layout_[0].rect.h>700,"fit width still constrained by page height");
    reader.FitPage();Check(reader.layout_[0].rect.h<=660.1f,"fit page after fit width overflowed");
    // 整页适配时 PageDown/PageUp 按页翻，到末页不越界。
    reader.GoTo(0);reader.OnKey(VK_NEXT);for(int i=0;i<120;++i)reader.OnAnimate(1.0f/60);
    Check(changed==1&&std::abs(reader.scroll_-(reader.layout_[1].rect.y-16))<1,"PageDown did not flip to the next page");
    reader.OnKey(VK_PRIOR);for(int i=0;i<120;++i)reader.OnAnimate(1.0f/60);
    Check(changed==0&&reader.scroll_<1,"PageUp did not flip back");
    reader.GoTo(19);reader.OnKey(VK_NEXT);Check(reader.current_page_==19,"page flip passed the last page");
    // 瓦片优先级：屏幕内的瓦片先于预取边距提交。
    reader.ActualSize();reader.Zoom(3);order.clear();reader.tiles_.clear();
    for(auto& [key,token]:reader.pending_){(void)key;token->store(true);}reader.pending_.clear();reader.RequestVisible();
    Check(order.size()>1,"zoomed page produced too few tiles");
    auto onScreen=[&](const TileRequest& t){const auto r=reader.ScreenRect(t.key.page,t.clip);return r.Bottom()>=reader.absolute_.y&&r.y<=reader.absolute_.Bottom();};
    bool sawOff=false,ordered=true;for(const auto& t:order){if(!onScreen(t))sawOff=true;else if(sawOff)ordered=false;}
    Check(ordered,"prefetch tiles were requested before visible tiles");
    // 搜索：全部结果保存，当前结果不在视口时才滚动。
    reader.FitPage();reader.GoTo(0);
    reader.SearchResults({{0,{50,50,40,12}},{5,{80,300,40,12}},{12,{80,700,40,12}}},1);
    Check(reader.search_hits_.size()==3&&reader.search_current_==1&&reader.search_&&reader.search_->page==5,"search results not stored");
    const auto shown=reader.ScreenRect(5,{80,300,40,12});
    Check(shown.y>=reader.absolute_.y&&shown.Bottom()<=reader.absolute_.Bottom(),"current search hit not revealed");
    const float kept=reader.scroll_;reader.SearchResults(reader.search_hits_,1);Check(reader.scroll_==kept,"visible search hit scrolled again");
    reader.DocumentPages(std::vector<PageInfo>(20,{595,842,0,0}),4);Check(reader.search_hits_.empty(),"stale search hits survived a document change");

    // 文字选择：拖动按文字流请求高亮，松手后保留（最终请求不被取消），单击空白清除；Ctrl+C 复制选中内容。
    {
        PdfCanvas sel;std::vector<HighlightRequest> asks;std::vector<PdfCanvas::TextSelection> copies;std::vector<Rect> areas;int changes=0;
        sel.request_tile=[](TileRequest){};sel.request_highlight=[&](HighlightRequest q){asks.push_back(q);};
        sel.copy_selection=[&](PdfCanvas::TextSelection t){copies.push_back(t);};sel.text_selection=[&](int,Rect r){areas.push_back(r);};
        sel.selection_changed=[&](PdfCanvas::TextSelection){++changes;};
        sel.DocumentPages({{595,842,0,0}},9);sel.Arrange({0,0,900,700});sel.GoTo(0);sel.EditingTool(Tool::Select);
        auto at=[&](Point p){auto r=sel.ScreenRect(0,{p.x,p.y,0,0});return lumen::Point{r.x,r.y};};
        sel.OnMouseDown(at({80,100}),1);sel.OnMouseMove(at({300,140}),1);
        Check(!asks.empty()&&asks.back().page==0,"text drag must request flow highlight");
        sel.OnMouseUp(at({300,140}),0);
        Check(!asks.back().cancelled->load(),"final selection request must survive mouse-up");
        sel.AcceptHighlight(asks.back(),{{{80,100},{300,100},{80,112},{300,112}}});
        Check(sel.HasTextSelection()&&sel.Selected().quads.size()==1,"selection must persist after release");
        Check(changes>0,"selection change was not reported");
        Check(areas.empty(),"flow selection must not fall back to area copy");
        sel.copy_selection(sel.Selected());Check(copies.size()==1&&copies[0].page==0,"copy callback");
        sel.OnMouseDown(at({500,700}),1);sel.OnMouseUp(at({500,700}),0);
        Check(!sel.HasTextSelection(),"click on empty space must clear the selection");
        // 过期请求（文档变更后）被忽略。
        sel.OnMouseDown(at({80,100}),1);sel.OnMouseMove(at({300,140}),1);sel.OnMouseUp(at({300,140}),0);
        const auto stale=asks.back();sel.DocumentPages({{595,842,0,0}},10);sel.AcceptHighlight(stale,{{{1,1},{2,1},{1,2},{2,2}}});
        Check(!sel.HasTextSelection(),"stale highlight reply applied after document change");
    }
    // 链接：可见页请求链接；悬停变手形并报告目标；单击（不拖动）激活，拖动不激活。
    {
        PdfCanvas lk;std::vector<std::pair<int,uint64_t>> linkAsks;std::vector<Link> activated;const Link* hovered=nullptr;int hovers=0;
        lk.request_tile=[](TileRequest){};lk.request_links=[&](int p,uint64_t g){linkAsks.push_back({p,g});};
        lk.activate_link=[&](Link l){activated.push_back(l);};lk.link_hover=[&](const Link* l){hovered=l;++hovers;};
        lk.request_highlight=[](HighlightRequest){};
        lk.DocumentPages(std::vector<PageInfo>(5,{612,792,0,0}),21);lk.Arrange({0,0,900,700});lk.GoTo(0);lk.EditingTool(Tool::Select);
        Check(!linkAsks.empty()&&linkAsks.front().first==0&&linkAsks.front().second==21,"visible page links not requested");
        const size_t asked=linkAsks.size();lk.RequestVisible();Check(linkAsks.size()==asked,"links requested twice for the same page");
        Link innerLink;innerLink.bounds={70,118,190,20};innerLink.page=2;innerLink.targetY=372;
        lk.AcceptLinks(0,21,{innerLink});
        auto at=[&](Point p){auto r=lk.ScreenRect(0,{p.x,p.y,0,0});return lumen::Point{r.x-lk.absolute_.x,r.y-lk.absolute_.y};};
        lk.OnMouseMove(at({100,128}),0);
        Check(hovered&&hovered->page==2&&hovers==1,"link hover not reported");
        Check(lk.CursorAt(at({100,128}))==lumen::CursorShape::Hand,"link cursor must be a hand");
        lk.OnMouseMove(at({400,400}),0);Check(hovered==nullptr&&hovers==2,"leaving a link must clear the hover");
        lk.OnMouseDown(at({100,128}),1);lk.OnMouseUp(at({100,128}),0);
        Check(activated.size()==1&&activated[0].page==2,"link click not activated");
        lk.OnMouseDown(at({100,128}),1);lk.OnMouseMove(at({240,160}),1);lk.OnMouseUp(at({240,160}),0);
        Check(activated.size()==1,"dragging from a link must select text, not follow it");
        lk.AcceptLinks(0,20,{innerLink});lk.DocumentPages(std::vector<PageInfo>(5,{612,792,0,0}),22);
        Check(lk.LinkAt(0,{100,128})==nullptr,"links from an old document version survived");
        // 跳转到目标纵坐标（适合宽度时生效）。
        lk.FitWidth();lk.GoToPoint(2,372);
        const auto target=lk.ScreenRect(2,{0,372,0,0});
        Check(lk.current_page_==2&&target.y>=lk.absolute_.y&&target.y<lk.absolute_.y+80,"link target position not placed near the top");
    }
    // 页面配色：白纸映射到纸色/深色，黑字映射到深字/浅字；切换配色丢弃旧瓦片并按新配色重新请求。
    {
        Check(ToneColor(PageTone::Normal,0xffffff)==0xffffff,"normal tone is identity");
        Check(ToneColor(PageTone::Sepia,0xffffff)==kSepiaPaper&&ToneColor(PageTone::Sepia,0)==0,"sepia maps paper and keeps ink");
        const auto night=ToneColor(PageTone::Night,0xffffff),ink=ToneColor(PageTone::Night,0);
        Check((night&0xff)==kNightPaper&&(ink&0xff)==kNightInk,"night inverts within a soft range");
        const auto red=ToneColor(PageTone::Night,0xd33445);
        Check(((red>>16)&0xff)>((red>>8)&0xff)+60&&((red>>16)&0xff)>(red&0xff)+60,"night must keep hue (red stays red)");
        std::vector<unsigned char> px{255,255,255,255,0,0,0,255};ApplyTone(PageTone::Night,px,2,1,8);
        Check(px[0]==kNightPaper&&px[4]==kNightInk&&px[3]==255&&px[7]==255,"bitmap tone keeps alpha");
        PdfCanvas tc;std::vector<TileRequest> asks;tc.request_tile=[&](TileRequest t){asks.push_back(t);};
        tc.DocumentPages({{595,842,0,0}},41);tc.Arrange({0,0,900,700});
        Check(!asks.empty()&&asks.back().tone==PageTone::Normal,"tiles default to normal tone");
        const auto old=asks.back();asks.clear();tc.Tone(PageTone::Night);
        Check(!asks.empty()&&asks.back().tone==PageTone::Night&&old.cancelled->load(),"tone change must cancel and re-request");
        tc.AcceptTile(old,{});Check(tc.tiles_.empty(),"tile rendered with the previous tone was accepted");
    }
    // 双页：封面单独放右侧，其后两两成行；同一行的两页顶部对齐、互不重叠；翻页按行跳。
    {
        PdfCanvas sp;sp.request_tile=[](TileRequest){};
        sp.DocumentPages(std::vector<PageInfo>(5,{595,842,0,0}),61);sp.Arrange({0,0,1200,800});
        sp.Spread(true,true);
        const auto& L=sp.layout_;
        Check(L.size()==5&&L[0].row==0&&L[1].row==1&&L[2].row==1&&L[3].row==2&&L[4].row==2,"cover spread rows");
        Check(L[0].rect.x>600&&std::abs(L[1].rect.y-L[2].rect.y)<.5f&&L[1].rect.Right()<=L[2].rect.x,"spread pages side by side");
        Check(L[1].rect.h<=800-40+1,"fit page applies to the spread height");
        sp.GoTo(0);sp.StepPage(1);Check(sp.CurrentPage()==1,"step from cover to first spread");
        sp.StepPage(1);Check(sp.CurrentPage()==3,"step moves one spread");
        sp.Spread(true,false);
        Check(sp.layout_[0].row==0&&sp.layout_[1].row==0&&sp.layout_[4].row==2&&sp.layout_[4].rect.x<600,"no-cover spread and trailing single page on the left");
        sp.Spread(false);Check(sp.layout_[1].rect.y>sp.layout_[0].rect.Bottom(),"back to single column");
    }
    // 页面视图右键：先选中光标下的页面，再弹出菜单。
    {
        PdfCanvas pv(PdfCanvas::View::Pages);pv.request_tile=[](TileRequest){};int menuPage=-2;
        pv.page_menu=[&](lumen::Point,int p){menuPage=p;};
        pv.DocumentPages(std::vector<PageInfo>(6,{595,842,0,0}),51);pv.Arrange({0,0,900,700});
        const auto cell=pv.layout_[2].rect;
        Check(pv.ShowContextMenu({cell.x+cell.w/2,cell.y+cell.h/2}),"page menu handled");
        Check(menuPage==2&&pv.SelectedPages()==std::vector<int>{2},"right-click must select the page under the cursor");
        pv.SelectedPages({1,2});pv.ShowContextMenu({cell.x+cell.w/2,cell.y+cell.h/2});
        Check(pv.SelectedPages().size()==2,"right-click inside a multi-selection keeps it");
    }
    // 网格拖动重排：超过阈值才浮起；浮起页跟手、其余页按预览顺序缓动让位；松手提交并本地换号，瓦片随页走；Esc / 被拒绝时退回。
    {
        PdfCanvas pv(PdfCanvas::View::Pages);pv.request_tile=[](TileRequest){};
        std::vector<int> got,gotSelected;int gotCurrent=-1,calls=0;bool accept=true;
        pv.reorder_pages=[&](std::vector<int> order,std::vector<int> selected,int current){++calls;got=order;gotSelected=selected;gotCurrent=current;return accept;};
        pv.DocumentPages(std::vector<PageInfo>(8,{595,842,0,0}),71);pv.Arrange({0,0,900,700});
        Check(pv.grid_columns_==4&&pv.grid_rows_.size()==2,"grid columns and rows");
        auto center=[&](int page,float fx=.5f){const auto r=pv.layout_[page].rect;return lumen::Point{r.x+r.w*fx,r.y+r.h*.5f};};
        const auto first=pv.layout_[0].rect;
        PdfCanvas::Tile tile;tile.request.key={0,28,0,0,-1,71,false};pv.tiles_.emplace(tile.request.key,tile);
        pv.OnMouseDown(center(0),1);pv.OnMouseMove({center(0).x+2,center(0).y},1);
        Check(!pv.page_drag_.active&&pv.order_.empty(),"a tiny movement is still a click");
        const auto target=center(2,.75f);pv.OnMouseMove(target,1);
        Check(pv.page_drag_.active&&pv.order_==std::vector<int>{1,2,0,3,4,5,6,7},"preview order follows the cursor");
        Check(std::abs(pv.layout_[1].rect.x-first.x)<.5f,"neighbour target moves into the vacated cell");
        Check(std::abs(pv.shown_[1].x-pv.layout_[1].rect.x)>10,"neighbours ease instead of jumping");
        Check(std::abs(pv.shown_[0].x+pv.shown_[0].w*.5f-target.x)<1&&std::abs(pv.shown_[0].y+pv.shown_[0].h*.5f-target.y)<1,"lifted card stays under the cursor");
        Check(pv.layout_[0].label==L"3"&&pv.layout_[1].label==L"1","labels preview the new numbering");
        pv.OnMouseUp(target,0);
        Check(calls==1&&got==std::vector<int>{1,2,0,3,4,5,6,7}&&gotSelected==std::vector<int>{2}&&gotCurrent==2,"drop commits the new order");
        Check(pv.SelectedPages()==std::vector<int>{2}&&pv.CurrentPage()==2&&!pv.page_drag_.active,"moved page stays selected");
        Check(pv.tiles_.size()==1&&pv.tiles_.begin()->first.page==2&&pv.tiles_.begin()->second.request.key.page==2,"tiles follow their page");
        Check(pv.motion_&&pv.Lifted(2),"dropped page flies into its slot");
        for(int i=0;i<180;++i)pv.OnAnimate(1.0f/60);
        Check(!pv.motion_&&pv.lifted_.empty()&&pv.lift_==0,"drop animation settles");
        for(int i=0;i<8;++i)Check(std::abs(pv.shown_[i].x-pv.layout_[i].rect.x)<.5f&&std::abs(pv.shown_[i].y-pv.layout_[i].rect.y)<.5f,"every page rests in its cell");
        // 多选整组拖动：不连续的选中页聚拢成组，插到目标之后，保持原相对顺序。
        pv.SelectedPages({0,5});pv.OnMouseDown(center(5),1);
        Check(pv.SelectedPages().size()==2,"pressing a selected page keeps the multi-selection");
        pv.OnMouseMove({center(5).x+20,center(5).y},1);Check(pv.page_drag_.active&&pv.page_drag_.group==std::vector<int>{0,5},"group drag");
        pv.OnMouseMove(center(7,.8f),1);
        Check(pv.order_==std::vector<int>{1,2,3,4,6,7,0,5},"group lands after the target");
        pv.OnMouseUp(center(7,.8f),0);
        Check(got==std::vector<int>{1,2,3,4,6,7,0,5}&&gotSelected==std::vector<int>{6,7},"group order committed");
        for(int i=0;i<180;++i)pv.OnAnimate(1.0f/60);
        // Esc 取消：不提交，顺序复原，页面退回。
        const int callsBefore=calls;pv.SelectedPages({3});pv.OnMouseDown(center(3),1);pv.OnMouseMove(center(0,.2f),1);
        Check(pv.page_drag_.active&&!pv.order_.empty(),"drag before cancel");
        Check(pv.OnKey(VK_ESCAPE)&&!pv.page_drag_.active&&pv.order_.empty(),"Esc cancels the drag");
        pv.OnMouseMove(center(1),1);pv.OnMouseUp(center(1),0);
        Check(calls==callsBefore&&!pv.page_drag_.active,"cancelled drag never commits or restarts");
        for(int i=0;i<180;++i)pv.OnAnimate(1.0f/60);
        Check(std::abs(pv.shown_[3].x-pv.layout_[3].rect.x)<.5f,"cancelled page returns home");
        pv.SelectedPages({3});pv.OnMouseDown(center(3),1);pv.OnMouseMove(center(0,.2f),1);pv.CancelPageDrag();
        Check(!pv.page_drag_.active&&pv.order_.empty()&&calls==callsBefore,"window-level Esc cancels the drag");
        pv.OnMouseUp(center(0,.2f),0);pv.SelectedPages({3});pv.OnMouseDown(center(3),1);pv.OnMouseMove(center(0,.2f),1);pv.EditingTool(Tool::Select);
        Check(!pv.page_drag_.active&&pv.order_.empty(),"tool changes never strand a lifted page");
        pv.OnMouseUp(center(0,.2f),0);for(int i=0;i<180;++i)pv.OnAnimate(1.0f/60);
        // 被拒绝（例如正忙）：不本地换号。
        accept=false;pv.SelectedPages({1});pv.OnMouseDown(center(1),1);pv.OnMouseMove(center(4,.8f),1);pv.OnMouseUp(center(4,.8f),0);
        Check(calls==callsBefore+1&&pv.order_.empty()&&pv.layout_[1].label==L"2","refused reorder leaves the order intact");
        // 另一视图的预排：页面几何、选择与瓦片同步换号。
        PdfCanvas side(PdfCanvas::View::Thumbnails);side.request_tile=[](TileRequest){};
        std::vector<PageInfo> mixed(4,{595,842,0,0});mixed[3]={1190,842,0,0};
        side.DocumentPages(mixed,72);side.Arrange({0,0,200,700});side.SelectedPages({3});
        side.PreviewOrder({3,0,1,2});
        Check(side.pages_[0].width==1190&&side.SelectedPages()==std::vector<int>{0},"preview order remaps geometry and selection");
        Check(side.grid_columns_==1,"sidebar is a single column");
        side.PreviewOrder({0,0,1,2});Check(side.pages_[0].width==1190,"invalid permutations are ignored");
    }
    // 侧栏缩略图：单击可见的缩略图不滚动；不在视口内时只滚最短距离。
    {
        PdfCanvas side(PdfCanvas::View::Thumbnails);side.request_tile=[](TileRequest){};
        side.DocumentPages(std::vector<PageInfo>(40,{595,842,0,0}),73);side.Arrange({0,0,200,700});
        side.GoTo(1);Check(side.target_scroll_==0,"visible thumbnail does not scroll");
        side.GoTo(5);const float need=side.layout_[5].rect.Bottom()+36-700;
        Check(std::abs(side.target_scroll_-need)<.5f,"minimal scroll brings the thumbnail into view");
        for(int i=0;i<120;++i)side.OnAnimate(1.0f/60);Check(std::abs(side.scroll_-need)<.5f,"sidebar scroll eases into place");
        side.GoTo(39);Check(side.scroll_==side.target_scroll_&&side.scroll_>need,"far jumps land immediately");
    }
    // 阅读位置：记录页码 + 页内偏移 + 缩放方式；未布局时暂存，布局后恢复。
    {
        PdfCanvas va;va.request_tile=[](TileRequest){};
        va.DocumentPages(std::vector<PageInfo>(30,{595,842,0,0}),31);va.Arrange({0,0,900,700});
        va.FitWidth();va.GoToPoint(12,300);const auto saved=va.CurrentView();
        Check(saved.page==12&&saved.fit==PdfCanvas::Fit::Width&&std::abs(saved.offset-(300-8/va.layout_[12].scale))<1,"current view offset");
        PdfCanvas b;b.request_tile=[](TileRequest){};
        b.DocumentPages(std::vector<PageInfo>(30,{595,842,0,0}),32);b.RestoreView(saved);
        Check(b.CurrentView().page==12,"pending view must be reported before layout");
        b.Arrange({0,0,900,700});
        const auto restored=b.CurrentView();
        Check(restored.page==12&&restored.fit==PdfCanvas::Fit::Width&&std::abs(restored.offset-saved.offset)<1,"view not restored after layout");
        PdfCanvas::ViewState custom{7,120,PdfCanvas::Fit::None,1.5f};b.RestoreView(custom);
        Check(std::abs(b.ActualZoom()-1.5f)<.01f&&b.current_page_==7&&std::abs(b.CurrentView().offset-120)<1,"custom zoom view");
        PdfCanvas::ViewState bogus{999,-5,PdfCanvas::Fit::Page,NAN};b.RestoreView(bogus);
        Check(b.current_page_==29,"out-of-range page must clamp");
    }

    // 表单字段：单击触发 activate_field 且优先于同位置的链接；拖动不触发；只读字段不可点；新文档版本清除字段缓存。
    {
        PdfCanvas f;f.request_tile=[](TileRequest){};
        f.DocumentPages({{595,842,0,0}},50);f.Arrange({0,0,900,700});f.GoTo(0);f.EditingTool(Tool::Select);
        auto at=[&](Point p){auto r=f.ScreenRect(0,{p.x,p.y,0,0});return lumen::Point{r.x,r.y};};
        FormField fill;fill.id=31;fill.page=0;fill.type=FieldType::Text;fill.name=L"name";fill.bounds={100,300,200,20};
        FormField locked=fill;locked.id=32;locked.readOnly=true;locked.bounds={100,340,200,20};
        FormField sig;sig.id=33;sig.page=0;sig.type=FieldType::Signature;sig.bounds={100,400,200,40};
        Link link;link.bounds={100,300,200,60};link.page=0;
        int activated=-1,followed=0;
        f.activate_field=[&](int page,FormField field){if(page==0)activated=field.id;};
        f.activate_link=[&](Link){++followed;};
        f.AcceptLinks(0,50,{link},{fill,locked,sig});
        f.OnMouseMove(at({150,310}),0);
        Check(f.hover_field_==31,"hovering a fillable field");
        f.OnMouseDown(at({150,310}),1);f.OnMouseUp(at({150,310}),0);
        Check(activated==31&&followed==0,"click must fill the field, not follow the overlapping link");
        activated=-1;f.OnMouseDown(at({150,310}),1);f.OnMouseMove(at({260,500}),1);f.OnMouseUp(at({260,500}),0);
        Check(activated==-1,"a drag must not activate the field");
        f.OnMouseMove(at({150,350}),0);Check(f.hover_field_<0,"read-only field is not clickable");
        f.OnMouseDown(at({150,350}),1);f.OnMouseUp(at({150,350}),0);
        Check(activated==-1&&followed==1,"read-only field falls through to the link");
        f.OnMouseDown(at({150,420}),1);f.OnMouseUp(at({150,420}),0);
        Check(activated==33,"unsigned signature field is clickable");
        const auto box=f.FieldScreenRect(0,fill.bounds);
        Check(box.w>100&&box.h>5,"field screen rectangle");
        f.DocumentPages({{595,842,0,0}},51);
        f.OnMouseMove(at({150,310}),0);activated=-1;
        Check(f.hover_field_<0,"stale fields must not survive a new document generation");
        f.OnMouseDown(at({150,310}),1);f.OnMouseUp(at({150,310}),0);
        Check(activated==-1,"stale field must not be activated");
        f.HighlightFields(false);Check(!f.HighlightFields(),"highlight toggle");
    }
    // 涂黑模式：拖动新增标记、单击标记删除、Delete 删除悬停标记、Esc 先取消拖动再退出；涂黑模式下不触发表单字段。
    {
        PdfCanvas g;g.request_tile=[](TileRequest){};
        g.DocumentPages({{595,842,0,0}},60);g.Arrange({0,0,900,700});g.GoTo(0);g.EditingTool(Tool::Select);
        auto at=[&](Point p){auto r=g.ScreenRect(0,{p.x,p.y,0,0});return lumen::Point{r.x,r.y};};
        std::vector<std::pair<int,Rect>> added;std::vector<size_t> removed;int escapes=0,fields=0;
        g.redact_add=[&](int page,Rect r){added.push_back({page,r});};
        g.redact_remove=[&](size_t i){removed.push_back(i);};
        g.redact_escape=[&]{++escapes;};
        g.activate_field=[&](int,FormField){++fields;};
        FormField fill;fill.id=41;fill.page=0;fill.type=FieldType::Text;fill.bounds={100,100,200,40};
        g.AcceptLinks(0,60,{},{fill});
        g.RedactMode(true);Check(g.RedactMode(),"redaction mode on");
        g.OnMouseDown(at({110,110}),1);g.OnMouseMove(at({250,130}),1);g.OnMouseUp(at({250,130}),0);
        Check(added.size()==1&&added[0].first==0,"drag adds a redaction mark");
        Check(std::abs(added[0].second.x-110)<2&&std::abs(added[0].second.w-140)<2&&std::abs(added[0].second.h-20)<2,"mark rectangle follows the drag");
        Check(fields==0,"redaction mode must not fill form fields");
        g.OnMouseDown(at({250,130}),1);g.OnMouseMove(at({110,110}),1);g.OnMouseUp(at({110,110}),0);
        Check(added.size()==2&&std::abs(added[1].second.x-110)<2,"reverse drag is normalized");
        g.OnMouseDown(at({100,300}),1);g.OnMouseMove(at({180,5000}),1);g.OnMouseUp(at({180,5000}),0);
        Check(added.size()==3&&added[2].second.y+added[2].second.h<=842.5f,"drag is clamped to the page");
        g.RedactMarks({{0,{300,300,100,50}},{0,{320,320,20,20}}});
        g.OnMouseMove(at({330,330}),0);Check(g.redact_hover_==1,"topmost mark is hovered");
        g.OnMouseDown(at({330,330}),1);g.OnMouseUp(at({330,330}),0);
        Check(removed.size()==1&&removed[0]==1,"click removes the hovered mark");
        g.OnMouseMove(at({305,340}),0);Check(g.redact_hover_==0,"hover the remaining mark");
        g.OnKey(VK_DELETE);Check(removed.size()==2&&removed[1]==0,"Delete removes the hovered mark");
        g.OnMouseDown(at({50,50}),1);g.OnMouseMove(at({90,90}),1);g.OnKey(VK_ESCAPE);
        Check(!g.redact_drag_&&escapes==0,"Esc cancels the drag first");
        g.OnMouseUp(at({90,90}),0);Check(added.size()==3,"cancelled drag adds nothing");
        g.OnKey(VK_ESCAPE);Check(escapes==1,"second Esc asks to leave redaction mode");
        g.RedactMode(false);
        g.OnMouseMove(at({150,120}),0);g.OnMouseDown(at({150,120}),1);g.OnMouseUp(at({150,120}),0);
        Check(fields==1&&added.size()==3,"normal clicks resume after redaction mode");
    }
    RunRotation();
    RunZoomTools();
    RunSpeechHighlight();
}
// 临时视图旋转：布局尺寸、坐标往返、瓦片覆盖、控制点方向、拖动 / 缩放在页面坐标中正确、文字编辑被拦截、视图状态。
// 朗读跟读高亮：保存 / 清除、不在视口时滚过去、已可见时不动、换文档清除。
static void RunSpeechHighlight(){
    PdfCanvas c;c.request_tile=[](TileRequest){};int changed=-1;c.page_changed=[&](int p){changed=p;};
    c.DocumentPages({{600,800,0,0},{600,800,0,0},{600,800,0,0}},21);c.Arrange({0,0,900,700});c.FitWidth();
    const float top=c.scroll_;
    c.SpeechHighlight(0,{{72,90,300,14}},true);
    Check(c.SpeechPage()==0&&c.speech_boxes_.size()==1&&std::abs(c.scroll_-top)<.5f,"visible sentence: highlighted without scrolling");
    c.SpeechHighlight(1,{{72,600,300,14},{72,616,200,14}},false);
    Check(c.SpeechPage()==1&&std::abs(c.scroll_-top)<.5f,"follow off: no scrolling");
    c.SpeechHighlight(2,{{72,500,300,14},{72,516,200,14}},true);
    {float y0=1e9f,y1=-1e9f;for(const auto& b:c.speech_boxes_){const auto r=c.ScreenRect(2,b);y0=std::min(y0,r.y);y1=std::max(y1,r.Bottom());}
     Check(y0>=c.absolute_.y&&y1<=c.absolute_.Bottom(),"follow: off-screen sentence scrolled into view");}
    Check(changed==2&&c.current_page_==2,"follow updates the current page");
    c.SpeechHighlight(-1,{},true);Check(c.SpeechPage()<0&&c.speech_boxes_.empty(),"cleared");
    c.SpeechHighlight(1,{},true);Check(c.SpeechPage()<0,"no boxes means no highlight");
    c.SpeechHighlight(9,{{0,0,10,10}},true);Check(c.SpeechPage()<0,"out-of-range page ignored");
    c.SpeechHighlight(1,{{72,90,300,14}},false);c.DocumentPages({{600,800,0,0}},22);
    Check(c.SpeechPage()<0,"a different document clears the highlight");
}
// 框选放大与放大镜：拖框放大到铺满并居中、单击放大 2 倍、Esc 取消、返回之前的缩放、旋转视图、上限；放大镜的高分辨率局部图请求与复用。
static void RunZoomTools(){
    PdfCanvas c;std::vector<TileRequest> tiles;c.request_tile=[&](TileRequest t){tiles.push_back(t);};
    c.DocumentPages({{600,800,0,0},{600,800,0,0}},11);c.Arrange({0,0,900,700});c.FitPage();c.EditingTool(Tool::Select);
    std::vector<bool> boxEvents;std::vector<float> zoomed;c.zoom_box_changed=[&](bool on){boxEvents.push_back(on);};c.box_zoomed=[&](float z){zoomed.push_back(z);};
    const float start=c.ActualZoom();const float startScroll=c.scroll_;
    c.ZoomBox(true);
    Check(c.ZoomBoxActive()&&boxEvents.size()==1&&boxEvents[0],"Z enters zoom-box mode");
    Check(!c.CanPan()&&c.PrefersDragOverPan(),"zoom-box drags are not touch pans");
    Check(c.CursorAt({450,350})==lumen::CursorShape::Cross,"zoom-box cursor is a crosshair");
    const Point anchor=c.PagePoint(0,{360,245});
    c.OnMouseDown({300,200},1);c.OnMouseMove({420,290},1);
    Check(c.zoom_drag_&&std::abs(c.zoom_drag_->w-120)<.01f&&std::abs(c.zoom_drag_->h-90)<.01f,"drag draws the zoom box");
    c.OnMouseUp({420,290},0);
    Check(!c.ZoomBoxActive()&&boxEvents.size()==2&&!boxEvents[1],"zoom box is one-shot");
    const float expected=std::min(start*std::min(860.0f/120,660.0f/90),8.0f);
    Check(std::abs(c.ActualZoom()-expected)<.01f&&zoomed.size()==1&&std::abs(zoomed[0]-c.ActualZoom())<.001f,"box zoom fills the viewport");
    {const auto at=c.ScreenRect(0,{anchor.x,anchor.y,0,0});Check(std::abs(at.x-450)<2&&std::abs(at.y-350)<2,"box centre ends up at the viewport centre");}
    Check(c.CanZoomBack(),"box zoom can be undone");
    Check(c.ZoomBack()&&std::abs(c.ActualZoom()-start)<.001f&&std::abs(c.scroll_-startScroll)<.5f&&c.fit_==PdfCanvas::Fit::Page&&!c.CanZoomBack(),"Shift+Z returns to the previous zoom and position");
    Check(!c.ZoomBack(),"nothing more to return to");
    // 单击（不拖动）= 以该点放大 2 倍
    c.ZoomBox(true);const Point clickAt=c.PagePoint(0,{460,380});c.OnMouseDown({460,380},1);c.OnMouseUp({461,381},0);
    Check(std::abs(c.ActualZoom()-start*2)<.01f,"click in zoom-box mode zooms 2x");
    {const auto at=c.ScreenRect(0,{clickAt.x,clickAt.y,0,0});Check(std::abs(at.x-450)<2&&std::abs(at.y-350)<2,"clicked point is centred");}
    c.ZoomBack();
    // Esc 取消，不改变缩放
    c.ZoomBox(true);c.OnMouseDown({300,200},1);c.OnMouseMove({400,300},1);
    Check(c.OnKey(VK_ESCAPE)&&!c.ZoomBoxActive()&&!c.zoom_drag_,"Esc cancels the zoom box");
    c.OnMouseUp({400,300},0);Check(std::abs(c.ActualZoom()-start)<.001f,"cancelled box does not zoom");
    // 上限 800%
    c.ZoomToRect({440,340,10,10});Check(std::abs(c.ActualZoom()-8.0f)<.001f,"box zoom clamps at 800%");c.ZoomBack();
    // 旋转视图中同样居中
    c.ViewRotation(90);c.FitPage();
    {const Point a=c.PagePoint(0,{420,300});c.ZoomToRect({380,260,80,80});const auto at=c.ScreenRect(0,{a.x,a.y,0,0});
     Check(std::abs(at.x-450)<2&&std::abs(at.y-350)<2&&c.ViewRotation()==90,"box zoom centres correctly in a rotated view");
     c.ZoomBack();Check(c.ViewRotation()==90,"returning keeps the view rotation");}
    c.ViewRotation(0);c.FitPage();
    // 页面视图不进入框选
    c.Display(PdfCanvas::View::Pages);c.ZoomBox(true);Check(!c.ZoomBoxActive(),"no zoom box in the Pages grid");
    c.Display(PdfCanvas::View::Reading);c.DocumentPages({{600,800,0,0},{600,800,0,0}},11);c.FitPage();
    // —— 放大镜 ——
    std::vector<bool> lensEvents;c.magnifier_changed=[&](bool on){lensEvents.push_back(on);};
    tiles.clear();c.Magnifier(true);Check(c.MagnifierActive()&&lensEvents.size()==1&&lensEvents[0],"L turns the magnifier on");
    Check(tiles.empty(),"no lens render before the pointer is over a page");
    const Point under=c.PagePoint(0,{450,350});
    c.MagnifierAt({450,350});
    std::vector<TileRequest> lens;for(const auto& t:tiles)if(t.key.x<0)lens.push_back(t);
    Check(lens.size()==1,"lens requests one high-resolution render");
    Check(lens.size()==1&&std::abs(lens[0].scale-c.TileScale(0)*2.5f)<.02f,"lens render at 2.5x the tile scale");
    Check(lens.size()==1&&lens[0].clip.x<=under.x&&lens[0].clip.y<=under.y&&lens[0].clip.x+lens[0].clip.w>=under.x&&lens[0].clip.y+lens[0].clip.h>=under.y,"lens render covers the point under the pointer");
    {const float pageW=300/2.5f/c.layout_[0].scale;Check(lens.size()==1&&lens[0].clip.w>=pageW-.5f,"lens render covers the whole lens area");}
    tiles.clear();c.MagnifierAt({456,352});Check(tiles.empty(),"small moves reuse the lens render");
    tiles.clear();c.MagnifierAt({300,120});
    Check(tiles.size()==1&&tiles[0].key.x<0&&lens[0].cancelled->load(),"a far move cancels the old lens render and requests a new one");
    Bitmap pixels;pixels.width=2;pixels.height=2;pixels.stride=8;pixels.bgra.assign(16,255);
    c.AcceptTile(lens[0],pixels);Check(!c.lens_tile_,"stale lens render is ignored");
    const size_t normal=c.tiles_.size();c.AcceptTile(tiles[0],pixels);
    Check(c.lens_tile_&&c.tiles_.size()==normal&&!c.lens_pending_,"lens render is kept apart from the page tiles");
    tiles.clear();c.MagnifierFactor(4);Check(c.MagnifierFactor()==4&&tiles.size()==1&&std::abs(tiles[0].scale-c.TileScale(0)*4)<.02f,"changing the magnification re-renders the lens");
    c.MagnifierFactor(20);Check(c.MagnifierFactor()==6,"magnification capped at 6x");
    c.MagnifierFactor(1);Check(c.MagnifierFactor()==1.5f,"magnification at least 1.5x");
    c.OnMouseLeave();tiles.clear();c.RequestVisible();
    bool lensAgain=false;for(const auto& t:tiles)if(t.key.x<0)lensAgain=true;
    Check(!lensAgain,"no lens render while the pointer is outside");
    for(const auto& t:tiles)if(t.key.x<0)Check(false,"page tiles never use the lens key");
    c.MagnifierAt({450,350});const auto pending=c.lens_pending_;
    Check(c.OnKey(VK_ESCAPE)&&!c.MagnifierActive()&&lensEvents.back()==false,"Esc turns the magnifier off");
    Check(!c.lens_tile_&&!c.lens_pending_&&(!pending||pending->cancelled->load()),"turning off drops the lens render");
}
static void RunRotation(){
    PdfCanvas c;std::vector<TileRequest> tiles;c.request_tile=[&](TileRequest t){tiles.push_back(t);};
    c.DocumentPages({{600,800,0,0},{600,800,0,0}},7);c.Arrange({0,0,900,700});c.FitPage();c.EditingTool(Tool::Select);
    const auto before=c.layout_[0].rect;
    c.ViewRotation(90);
    Check(c.ViewRotation()==90&&c.layout_[0].rect.w>c.layout_[0].rect.h,"90 deg: portrait page displayed landscape");
    Check(std::abs(c.layout_[0].rect.w/c.layout_[0].rect.h-800.0f/600)<.01f,"90 deg: aspect ratio swapped exactly");
    for(int rot:{0,90,180,270}){
        c.ViewRotation(rot);
        for(Point p:{Point{10,20},Point{590,40},Point{300,780},Point{123.5f,456.25f}}){
            const auto r=c.ScreenRect(0,{p.x,p.y,0,0});const auto back=c.PagePoint(0,{r.x,r.y});
            Check(PointDistance(back,p)<.05f,"rotation round trip page -> screen -> page");
        }
        const auto box=c.ScreenRect(0,{0,0,600,800});const auto frame=c.layout_[0].rect;
        Check(std::abs(box.x-frame.x)<.05f&&std::abs(box.y-(frame.y-c.scroll_))<.05f&&std::abs(box.w-frame.w)<.05f&&std::abs(box.h-frame.h)<.05f,"whole page maps onto its display frame");
    }
    c.ViewRotation(90);
    {   // 页面左上角显示在右上角（顺时针 90°）
        const auto frame=c.layout_[0].rect;const auto tl=c.ScreenRect(0,{0,0,0,0});
        Check(std::abs(tl.x-frame.Right())<.05f&&std::abs(tl.y-(frame.y-c.scroll_))<.05f,"90 deg is clockwise: page top-left at display top-right");
    }
    c.ViewRotation(-90);Check(c.ViewRotation()==270,"negative rotation normalised");
    c.ViewRotation(450);Check(c.ViewRotation()==90,"rotation beyond 360 normalised");
    // 瓦片：旋转后请求的瓦片覆盖整个可见页面（按页面坐标）。
    tiles.clear();c.ViewRotation(180);c.ViewRotation(90);c.RequestVisible();
    float area=0;for(const auto& t:tiles)if(t.key.page==0)area+=t.clip.w*t.clip.h;
    Check(area>=600.0f*800-1,"rotated view requests tiles for the whole visible page");
    // 控制点：旋转 90° 时，屏幕左上角的控制点是页面左下角（序号 5）。
    Annotation shape;shape.id=31;shape.type=Tool::Rectangle;shape.bounds={100,100,200,100};c.Selection(0,{shape},31);
    const auto sr=c.ScreenRect(0,shape.bounds).Inset(-6,-6);
    Check(c.HandleAt(shape,{sr.x,sr.y})==5,"90 deg: screen top-left handle is page bottom-left");
    Check(c.HandleAt(shape,{sr.Right(),sr.Bottom()})==2,"90 deg: screen bottom-right handle is page top-right");
    Check(c.CursorAt({sr.x+sr.w/2,sr.y})==lumen::CursorShape::SizeNS,"90 deg: handle on the screen top edge shows a vertical cursor");
    // 拖动：屏幕向右 30 → 页面向上 30（90° 下屏幕 x 对应页面 -y）。
    Annotation moved;c.update_annotation=[&](int,Annotation a){moved=a;};
    const auto edge=c.ScreenRect(0,{shape.bounds.x+1,shape.bounds.y+50,0,0});
    c.OnMouseDown({edge.x,edge.y},1);Check(c.moving_.has_value(),"rectangle outline grabbed in rotated view");
    const float k=c.layout_[0].scale;
    c.OnMouseMove({edge.x+30*k,edge.y},1);c.OnMouseUp({edge.x+30*k,edge.y},0);
    Check(std::abs(moved.bounds.y-(shape.bounds.y-30))<.2f&&std::abs(moved.bounds.x-shape.bounds.x)<.2f,"drag in rotated view moves along page axes");
    // 缩放：拖屏幕左上角控制点（页面左下角）向左 20 → 页面高度增加 20，顶部不动。
    c.Selection(0,{shape},31);moved={};
    c.OnMouseDown({sr.x,sr.y},1);c.OnMouseMove({sr.x-20*k,sr.y},1);c.OnMouseUp({sr.x-20*k,sr.y},0);
    Check(std::abs(moved.bounds.h-120)<.3f&&std::abs(moved.bounds.y-100)<.3f&&std::abs(moved.bounds.w-200)<.3f,"resize in rotated view changes the matching page edge");
    // 旋转视图中文字工具：不开始编辑，给出提示。
    int blocked=0;c.rotated_edit_blocked=[&]{++blocked;};c.EditingTool(Tool::Text);
    const auto spot=c.ScreenRect(0,{400,600,0,0});c.OnMouseDown({spot.x,spot.y},1);c.OnMouseUp({spot.x,spot.y},0);
    Check(blocked==1&&!c.dragging_,"text tool blocked while rotated");c.EditingTool(Tool::Select);
    // 视图状态随标签页保存 / 恢复；页面视图不旋转。
    auto state=c.CurrentView();Check(state.rotation==90,"view state records rotation");
    c.ViewRotation(0);c.RestoreView(state);Check(c.ViewRotation()==90&&c.layout_[0].rect.w>c.layout_[0].rect.h,"view state restores rotation");
    c.Display(PdfCanvas::View::Pages);Check(c.layout_[0].rect.h>c.layout_[0].rect.w,"page grid shows true orientation");
    c.Display(PdfCanvas::View::Reading);Check(c.layout_[0].rect.w>c.layout_[0].rect.h,"reading view keeps rotation after grid");
    c.ViewRotation(0);Check(std::abs(c.layout_[0].rect.w-before.w)<.5f&&std::abs(c.layout_[0].rect.h-before.h)<.5f,"rotation back to 0 restores the original layout");
    // 未排版（宽高为 0）的画布上缩放 / 适合宽度不能访问空布局（document_workspace 偶发崩溃的根因）。
    PdfCanvas hidden;hidden.request_tile=[](TileRequest){};hidden.DocumentPages({{600,800,0,0}},9);hidden.Arrange({0,0,0,0});
    Check(hidden.layout_.empty(),"zero-size canvas has no layout");
    hidden.ZoomTo(1.5f);hidden.Zoom(1.2f);hidden.FitWidth();Check(hidden.fit_==PdfCanvas::Fit::Width,"zoom on an unlaid canvas is deferred, not a crash");
    hidden.Arrange({0,0,900,700});Check(!hidden.layout_.empty(),"layout appears once the canvas has a size");
}
};
}
int main(){
    try{lumen::App app;lpdf::CanvasTestAccess::Run();std::cout<<"PASS canvas scroll, tools, direct editing, eight-way resize, view rotation, zoom box, magnifier and read-along highlight: "<<lpdf::canvasAssertions<<" assertions\n";}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}