#pragma once
#include "core/document.h"
#include "page_tone.h"
#include <lumen/Panel.h>
#include <lumen/TextLayout.h>
#include <lumen/Animate.h>
#include <lumen/Painter.h>
#include <d2d1_3.h>
#include <wrl/client.h>
#include <map>
#include <set>
#include <tuple>
namespace lpdf {
struct TileKey {
    int page{}, scale{}, x{}, y{};
    int hidden{-1};          // 渲染时隐藏的批注 id（-1 为不隐藏）
    uint64_t generation{};   // 文档版本；旧版本瓦片在新瓦片到达前继续作为底图
    bool textLayer{};
    auto operator<=>(const TileKey&) const = default;
};
struct TileRequest { TileKey key; float scale{}; Rect clip; uint64_t generation{}; Cancel cancelled; int hiddenAnnotation{-1}; PageTone tone{PageTone::Normal}; };
// 拖动预览：请求单个批注的外观位图。from/type 记录发起时的原始几何，避免落下后本地乐观更新造成偏移。
struct SpriteRequest { int page{-1}, id{-1}; float scale{}; uint64_t generation{}; Rect from; Tool type{}; };
// fromStart / toEnd：跨页选择的整页端点；snap：1 双击选词、2 三击选段（引擎先吸附，再按吸附后的起止点取高亮）。
struct HighlightRequest { int page{-1}; Point start,end; uint64_t generation{}; Cancel cancelled; bool fromStart{},toEnd{}; int snap{}; };
// 在引擎线程上解析高亮请求；吸附时改写 request 的起止点，回到界面后成为选择本身。
inline std::vector<Quad> ResolveHighlight(Document& document,HighlightRequest& request){
    if(request.snap){
        const auto range=document.SnapSelection(request.page,request.start,request.snap==2?SnapUnit::Paragraph:SnapUnit::Word);
        if(!range)return {};
        request.start=range->first;request.end=range->second;
    }
    return document.HighlightQuads(request.page,request.start,request.end,request.fromStart,request.toEnd);
}
class PdfCanvas final : public lumen::Panel {
public:
    enum class View { Reading, Thumbnails, Pages };
    explicit PdfCanvas(View view=View::Reading);
    void DocumentPages(std::vector<PageInfo> pages,uint64_t generation);
    void Display(View view);
    void EditingTool(Tool tool);
    void DrawingStyle(AnnotationStyle style,float opacity){drawing_style_=style;drawing_opacity_=opacity;Invalidate();}
    void AcceptHighlight(HighlightRequest,std::vector<Quad>);
    std::function<void(HighlightRequest)> request_highlight;
    std::function<void(int,int)> delete_annotation;
    void AcceptTile(TileRequest request,Bitmap bitmap);
    void FailTile(TileKey key);
    void AcceptTextLayer(int page,uint64_t generation,const std::vector<Annotation>& annotations);
    void AcceptSprite(SpriteRequest request,Sprite sprite);
    void GoTo(int page);
    void Zoom(float multiplier);
    void FitPage();
    // 视图中央对应的页面坐标（页面可能不在中央：调用方自行限制到页面范围内）。
    Point ViewCenter(int page)const{return PagePoint(page,{absolute_.w*.5f,absolute_.h*.5f});}
    void FitWidth();
    void ActualSize();
    // 翻页：整页适配时一次一页，其它缩放下跳到上一页/下一页顶部。
    void StepPage(int direction);
    enum class Fit { None, Page, Width };
    Fit FitMode()const noexcept{return fit_;}
    float ActualZoom()const;
    std::function<void(float)> zoom_changed;
    void FocusText(int page,Rect bounds);
    void RepositionEditor(){PlaceEditor();}
    void Draft(std::optional<Annotation> value,int page=-1);
    std::function<void(int,int,Point)> edit_text;
    std::function<void()> commit_text;
    lumen::Point ClientPoint(int page,Point p)const{auto r=ScreenRect(page,{p.x,p.y,0,0});return {r.x,r.y};}
    std::function<void(lumen::Rect,lumen::Rect,float,float)> editor_layout;
    std::function<void(Rect,bool)> draft_resized;
    void Selection(int page,std::vector<Annotation> annotations,int selected=-1);
    void SearchHighlight(std::optional<SearchHit> hit);
    // 全部搜索结果淡色标出，current 为当前结果（加深并描边）。
    void SearchResults(std::vector<SearchHit> hits,int current,bool reveal=true);
    void SelectedPages(std::vector<int> pages);
    std::vector<int> SelectedPages()const{return selected_pages_;}
    int CurrentPage()const{return current_page_;}
    float ZoomValue()const{return ActualZoom();}
    void Editable(bool value){editable_=value;}
    std::function<void(TileRequest)> request_tile;
    std::function<void(SpriteRequest)> request_sprite;
    std::function<void(int)> page_changed;
    // 页面视图：选中集合变化（单击 / Ctrl / Shift / Ctrl+A）与 Delete 删除选中页。
    std::function<void(size_t)> pages_selected;
    std::function<void()> delete_pages;
    std::function<void(int,Tool,Rect,std::vector<Point>)> create_annotation;
    std::function<void(int,Annotation)> update_annotation;
    std::function<void(int,int)> select_annotation;
    std::function<void(int,int)> reorder_page;
    // 网格拖动重排（页面视图与侧栏缩略图）：order[新位置]=原页码；selected / current 为移动后的选中页与当前页。
    // 返回 false 表示未接受（例如正忙），页面平滑退回原位。
    std::function<bool(std::vector<int>,std::vector<int>,int)> reorder_pages;
    // 另一视图发起重排时，本视图先按新顺序重映射页面、瓦片与选择：文档更新到达前不闪旧内容。
    void PreviewOrder(const std::vector<int>& order);
    void ResetTiles();
    void HideAnnotations(bool hide);
    bool AnnotationsHidden()const{return hide_annotations_;}
    void AutoScroll(float pixels){if(view_==View::Reading)Scroll(pixels,false);}
    bool PageDragging()const noexcept{return page_drag_.active;}
    void CancelPageDrag(){EndPageDrag(false);}
    // 页面视图：双击缩略图在阅读视图中打开。
    std::function<void(int)> open_page;
    // 页面视图缩略图大小（Ctrl+滚轮）。
    void GridZoom(float factor);
    std::function<void(std::vector<fs::path>)> files_dropped;
    std::function<void(int,Rect)> text_selection;
    // 阅读视图的文字选择：拖动时按文字流高亮，松手后保留；Ctrl+C 或右键复制。Alt 拖动为区域复制。
    // 可以跨页拖动（拖到上下边缘自动滚动）；双击选词，三击选段。
    // page/start 为起点（按下处），endPage/end 为终点；spans 按页序列出每页的范围：
    // 跨页时首页取到页尾、末页从页首取、中间页整页。quads 为起点页的高亮（单页选择时即全部）。
    struct TextSpan {int page{-1};Point start,end;bool fromStart{},toEnd{},ready{};std::vector<Quad> quads;};
    struct TextSelection {int page{-1};Point start,end;std::vector<Quad> quads;int endPage{-1};std::vector<TextSpan> spans;};
    const TextSelection& Selected()const{return text_selection_;}
    bool HasTextSelection()const;
    void ClearTextSelection();
    std::function<void(TextSelection)> copy_selection;
    std::function<void(TextSelection)> selection_changed;
    // 页面链接：可见页按需请求；悬停显示目标，单击跳转或打开外部地址。
    std::function<void(int,uint64_t)> request_links;
    void AcceptLinks(int page,uint64_t generation,std::vector<Link> links,std::vector<FormField> fields={});
    // 表单：单击可填写字段（或未签名的签名域）时回调；字段随链接一起按页请求、随文档代号失效。
    std::function<void(int,FormField)> activate_field;
    // 涂黑模式：拖动框选新增标记（redact_add），单击 / Delete 删除标记（redact_remove），Esc 退出（redact_escape）。
    // 标记由应用层持有并通过 RedactMarks 同步；画布只负责显示与命中。
    std::function<void(int,Rect)> redact_add;
    std::function<void(size_t)> redact_remove;
    std::function<void()> redact_escape;
    void RedactMode(bool on){redact_mode_=on;redact_drag_.reset();redact_hover_=-1;Invalidate();}
    bool RedactMode()const{return redact_mode_;}
    void RedactMarks(std::vector<RedactionMark> marks){redact_marks_=std::move(marks);redact_hover_=-1;Invalidate();}
    void HighlightFields(bool on){highlight_fields_=on;Invalidate();}
    bool HighlightFields()const{return highlight_fields_;}
    lumen::Rect FieldScreenRect(int page,Rect r)const{return page>=0&&page<static_cast<int>(layout_.size())?ScreenRect(page,r):lumen::Rect{};}
    // 把页内区域滚动到视口上部（已可见时不动）。
    void RevealRect(int page,Rect r);
    std::function<void(const Link*)> link_hover;
    std::function<void(Link)> activate_link;
    std::function<void(lumen::Point)> context_menu;
    std::function<void(int)> navigate;   // Alt+←/→：返回/前进
    // 页面配色（显示层）。切换后重新请求瓦片，旧配色的瓦片不再作为底图。
    // 双页（对开）阅读：cover=true 时首页单独成行（书籍排版）。只影响阅读视图。
    void Spread(bool on,bool cover=true);
    bool Spread()const noexcept{return spread_;}
    bool SpreadCover()const noexcept{return spread_cover_;}
    void Tone(PageTone tone);
    PageTone Tone()const noexcept{return tone_;}
    // 右键：阅读视图为文字/链接菜单；页面与缩略图视图先选中光标下的页面。第二个参数为光标下页码（-1 为空白）。
    std::function<void(lumen::Point,int)> page_menu;
    // 阅读位置：页码 + 页内偏移（页面坐标）+ 缩放方式，缩放变化后仍指向同一位置。
    struct ViewState {int page{};float offset{};Fit fit{Fit::Page};float zoom{1};int rotation{};};
    // 临时视图旋转（顺时针 0 / 90 / 180 / 270°）：只改变阅读视图的显示，不改文件；页面视图与缩略图不受影响。
    void ViewRotation(int degrees);
    int ViewRotation()const{return rotation_;}
    std::function<void()> rotated_edit_blocked;   // 旋转视图中尝试编辑文字（行内编辑器无法旋转）
    // 框选放大（Acrobat 的 Z）：进入后在页面上拖一个框，松开即放大到该区域；单击 = 以该点放大 2 倍；Esc 取消。一次性。
    void ZoomBox(bool on);
    bool ZoomBoxActive()const noexcept{return zoom_box_;}
    std::function<void(bool)> zoom_box_changed;
    std::function<void(float)> box_zoomed;        // 框选放大完成后的实际缩放
    void ZoomToRect(lumen::Rect local);          // 把画布局部坐标中的矩形放大到铺满视口并居中（记入可返回的历史）
    bool ZoomBack();                              // 回到最近一次框选前的缩放与位置
    bool CanZoomBack()const noexcept{return !zoom_back_.empty();}
    // 放大镜：跟随指针的局部放大，高分辨率单独渲染；不影响任何工具操作。Alt+滚轮调整倍率。
    void Magnifier(bool on);
    bool MagnifierActive()const noexcept{return lens_on_;}
    float MagnifierFactor()const noexcept{return lens_factor_;}
    void MagnifierFactor(float factor);
    void MagnifierAt(lumen::Point local){lens_point_=local;lens_inside_=true;RequestLens();Invalidate();}   // 自动化/冒烟用
    std::function<void(bool)> magnifier_changed;
    bool MagnifierRendered()const noexcept{return lens_tile_.has_value();}
    // 朗读跟读：高亮当前句（页面坐标的逐行框）；follow 时句子不在视口内就滚过去。page<0 清除。
    void SpeechHighlight(int page,std::vector<Rect> boxes,bool follow);
    int SpeechPage()const noexcept{return speech_page_;}
    ViewState CurrentView()const;
    void RestoreView(ViewState state);
    void GoToPoint(int page,float y);
    // 手型工具（与 Acrobat 相同）：H 常驻，按住空格临时启用，中键拖动也可平移。拖动同时平移纵向与横向。
    void HandTool(bool on){hand_tool_=on;if(!on)panning_=false;Invalidate();}
    // 翻页模式（非连续）：一次只显示当前一页（双页时一对），滚动到页边后整页翻过，PageDown 不会出现半页。
    void Paged(bool on);
    bool Paged()const noexcept{return paged_;}
    // 演示模式：黑底、整页居中、无边框与滚动条；单击 / 滚轮 / 方向键 / 空格翻页，右键上一页。
    void Presenting(bool on);
    bool Presenting()const noexcept{return presenting_;}
    std::function<void()> exit_presentation;
    bool HandTool()const noexcept{return hand_tool_;}
    void SpaceHeld(bool down){if(space_held_!=down){space_held_=down;Invalidate();}}
    bool Panning()const noexcept{return panning_;}
protected:
    lumen::Size Measure(lumen::Size available,const lumen::Theme&) override;
    void Arrange(const lumen::Rect&) override;
    void Prepare(lumen::Painter&) override;
    void Draw(lumen::Painter&,const lumen::Theme&) override;
    bool OnAnimate(float dt) override;
    bool OnWheel(float delta) override;
    void OnMouseLeave() override;
    lumen::CursorShape CursorAt(lumen::Point) const override;
    bool CanPan() const noexcept override{return !draft_&&!scrollbar_drag_&&!moving_&&!page_drag_.active&&tool_==Tool::Select&&view_!=View::Pages&&!zoom_box_;}
    void PanBy(float,float dy) override{dragging_=false;Scroll(-dy,false);}
    void PanFling(float,float vy) override{Scroll(-vy*.20f);}

    bool OnHWheel(float delta) override;
    bool OnKey(uint32_t key) override;
    void OnMouseDown(lumen::Point,uint32_t) override;
    void OnMouseMove(lumen::Point,uint32_t) override;
    void OnMouseUp(lumen::Point,uint32_t) override;
    void OnMouseDoubleClick(lumen::Point) override;
    bool Focusable()const noexcept override{return true;}
    bool AcceptsFileDrop()const noexcept override{return true;}
    std::vector<std::wstring> FilterFileDrop(std::vector<std::wstring> paths)const override{return paths;}
    void OnFileDrop(std::vector<std::wstring>) override;
    bool ShowContextMenu(lumen::Point window_dip) override;
    bool PrefersDragOverPan()const noexcept override{return scrollbar_drag_||moving_.has_value()||page_drag_.active||tool_!=Tool::Select||view_==View::Pages||zoom_box_;}
private:
    friend struct CanvasTestAccess;
    struct Layout {lumen::Rect rect;float scale{};std::wstring label;int row{-1};};
    struct Tile {TileRequest request;Bitmap pixels;Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;};
    // 拖动中的批注外观：跟随鼠标绘制，瓦片同步隐藏原位内容，落下后保留到新瓦片到达为止。
    struct Ghost {SpriteRequest request;Sprite sprite;Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;};
    struct Settle {int page{-1},id{-1};Rect to;uint64_t generation{};unsigned long long deadline{};};
    void LayoutPages();
    void RequestVisible();
    void LayoutSpread();
    void FinishLayout(float height);
    void TrimTiles();
    int Hidden(int page)const;
    float TileScale(int page)const;
    void BeginGhost(int page,const Annotation&);
    void DrawGhost(lumen::Painter&,int page)const;
    int Hit(lumen::Point)const;
    Point PagePoint(int,lumen::Point)const;
    int Rot()const{return view_==View::Reading?rotation_:0;}
    float DisplayWidth(const PageInfo& p)const{return Rot()%180?p.height:p.width;}
    float DisplayHeight(const PageInfo& p)const{return Rot()%180?p.width:p.height;}
    lumen::Point Local(int page,Point p)const;   // 页面坐标 → 页面显示框内的偏移（含旋转）
    int PageHandle(int screenHandle)const;       // 屏幕上的控制点序号 → 页面坐标系中的序号
    lumen::Rect ScreenRect(int,Rect)const;
    void Scroll(float,bool smooth=true);
    void UpdateViewport();
    lumen::Rect ScrollThumb()const;
    int HandleAt(const Annotation&,lumen::Point)const;
    void PlaceEditor();
    void DrawSelection(lumen::Painter&,const Annotation&,bool hover=false)const;
    void DrawVector(lumen::Painter&,int,const Annotation&)const;
    void PrepareTextLayers(lumen::Painter&);
    void DrawTextLayer(lumen::Painter&,int);
    void ZoomTo(float actual);
    void RequestHighlight();
    const Link* LinkAt(int page,Point p)const;
    const FormField* FieldAt(int page,Point p)const;
    void ApplyView(const ViewState& state);
    // —— 翻页模式 / 演示模式 ——
    bool paged_{},presenting_{};
    unsigned long long flip_tick_{};
    bool PagedActive()const noexcept{return (paged_||presenting_)&&view_==View::Reading&&!layout_.empty();}
    void RowRange(int row,float& lo,float& hi)const;   // 当前一屏（行）允许的滚动范围
    void PagedClamp();
    bool FlipRow(int direction,bool toTop);
    float PadTop()const noexcept{return presenting_?absolute_.h:16.0f;}
    float RowGap()const noexcept{return presenting_?absolute_.h:24.0f;}
    // —— 文字选择：跨页、双击 / 三击吸附 ——
    int PageNear(lumen::Point local)const;
    void UpdateSpans();
    void RequestSpans();
    void ExtendSelection(lumen::Point local);
    void SnapSelect(int page,Point p,int unit);
    std::set<int> span_requested_;
    lumen::Point select_mouse_{};
    unsigned long long double_tick_{},triple_tick_{};
    lumen::Point double_local_{};
    // —— 网格（页面视图 / 侧栏缩略图）拖动重排 ——
    // 拖起的页面浮起跟手；其余页面按预览顺序平滑让位；松手后浮起的页面飞入空位。
    // 所有位移、浮起、悬停与滚动使用同一条缓动（kMotion），动效一致。
    struct PageDrag {bool active{};std::vector<int> group;int grabbed{-1};int insert{};lumen::Point grab{},mouse{};};
    bool Grid()const noexcept{return view_!=View::Reading;}
    lumen::Rect PageRect(int page)const;
    bool InDragGroup(int page)const;
    bool Lifted(int page)const;
    std::vector<int> DragOrder(int insert)const;
    int DropIndex(lumen::Point content)const;
    void BeginPageDrag();
    void UpdatePageDrag();
    void EndPageDrag(bool commit);
    void DrawPageTiles(lumen::Painter&,int page,bool& loading);
    void DrawGridChrome(lumen::Painter&,int page,const lumen::Rect& r,bool lifted)const;
    PageDrag page_drag_;
    std::vector<int> order_;            // 显示顺序（拖动中为预览顺序）；空 = 原顺序
    std::vector<lumen::Rect> shown_;    // 网格中每页的当前显示位置（内容坐标），缓动逼近 layout_
    std::vector<float> hover_mix_;      // 悬停描边淡入淡出
    std::vector<int> lifted_;           // 浮起层：拖动中与落下飞入过程中的页面
    float lift_{};
    bool motion_{};
    int grid_columns_{1};
    float grid_cell_{},grid_zoom_{1};
    std::vector<float> grid_rows_;      // 每行顶部（内容坐标）
    bool hide_annotations_{};
    TextSelection text_selection_;
    bool area_select_{},selecting_text_{};
    std::optional<Link> press_link_;
    struct LinkPage {uint64_t generation{};std::vector<Link> links;std::vector<FormField> fields;};
    std::map<int,LinkPage> links_;
    std::set<int> links_pending_;
    int hover_link_page_{-1},hover_link_{-1};
    int hover_field_page_{-1},hover_field_{-1};   // hover_field_：字段 id
    std::optional<std::pair<int,FormField>> press_field_;
    struct RedactDrag{int page{};Point start,current;};
    bool redact_mode_{};std::vector<RedactionMark> redact_marks_;std::optional<RedactDrag> redact_drag_;int redact_hover_{-1};
    int RedactMarkAt(int page,Point p)const;
    bool highlight_fields_{true};
    std::optional<ViewState> pending_view_;
    PageTone tone_{PageTone::Normal};
    bool spread_{},spread_cover_{true};

    struct TextVisual { Annotation annotation; lumen::TextLayout layout; Rect display; };
    struct TextPage { uint64_t generation{}; std::vector<TextVisual> items; };
    std::map<int,TextPage> text_pages_;
    Fit fit_{Fit::Page};
    bool loading_{};
    float spin_{};
    int hover_page_{-1};
    std::vector<SearchHit> search_hits_;
    int search_current_{-1};
    void DrawPageFrame(lumen::Painter&,const lumen::Rect&,int page)const;
    void DrawLoading(lumen::Painter&,const lumen::Rect&)const;
    std::optional<Annotation> draft_;
    int draft_page_{-1};
    View view_;
    Tool tool_{Tool::Select};
    std::vector<PageInfo> pages_;
    std::vector<Layout> layout_;
    std::map<TileKey,Tile> tiles_;
    std::map<TileKey,Cancel> pending_;
    std::set<TileKey> wanted_;
    std::vector<int> visible_,selected_pages_;
    std::vector<Annotation> annotations_;
    int annotations_page_{-1},selected_annotation_{-1},current_page_{};
    float horizontal_{},content_width_{},scroll_{},target_scroll_{},content_height_{},zoom_{1},dpi_{1};
    uint64_t generation_{};
    void* device_{};
    bool editable_{true},dragging_{},resizing_{};
    // 框选放大 / 放大镜
    struct ZoomBack_ {ViewState view;float horizontal{};};
    bool zoom_box_{};std::optional<lumen::Rect> zoom_drag_;lumen::Point zoom_origin_{};std::vector<ZoomBack_> zoom_back_;
    bool lens_on_{},lens_inside_{};lumen::Point lens_point_{};float lens_factor_{2.5f};
    std::optional<Tile> lens_tile_;std::optional<TileRequest> lens_pending_;
    lumen::Rect LensRect()const{return {lens_point_.x-150,lens_point_.y-100,300,200};}
    void RequestLens();
    void DropLens();
    void DrawLens(lumen::Painter& p);
    int speech_page_{-1};std::vector<Rect> speech_boxes_;
    int rotation_{};bool unrotated_{};   // unrotated_：绘制页面内容时已压入旋转变换，映射按未旋转几何
    bool hand_tool_{},space_held_{},panning_{};
    lumen::Point pan_origin_{};
    float pan_scroll_{},pan_horizontal_{};
    bool HandActive()const;
    // 横向可滚动范围只看当前屏幕上的页面：窄页在屏时不能被拖进两侧空白。
    float HorizontalLimit()const;
    void ClampHorizontal();
    bool collapse_on_up_{};
    int anchor_page_{-1},down_page_{-1},resize_handle_{-1},hover_annotation_{-1};
    bool scrollbar_drag_{},scrollbar_hover_{};
    float scroll_grab_{};
    lumen::Point down_local_{};
    Point down_point_{},last_point_{};
    std::optional<Annotation> moving_;
    std::optional<Ghost> ghost_;
    std::optional<SpriteRequest> ghost_pending_;
    std::optional<Settle> settling_;
    std::vector<Point> stroke_;
    AnnotationStyle drawing_style_;
    float drawing_opacity_{1};
    bool area_highlight_{};
    Cancel highlight_cancel_;
    std::vector<Quad> highlight_preview_;
    std::optional<SearchHit> search_;
    std::optional<ViewState> before_present_;
};
// 在引擎线程上取选中文字：跨页时逐页按范围取出，页与页之间换行。
inline std::wstring SelectedText(Document& document,const PdfCanvas::TextSelection& selection){
    if(selection.spans.empty())return selection.page>=0?document.SelectionText(selection.page,selection.start,selection.end):std::wstring{};
    std::wstring text;
    for(const auto& s:selection.spans){
        auto part=document.SelectionText(s.page,s.start,s.end,s.fromStart,s.toEnd);
        if(part.empty())continue;
        if(!text.empty()&&text.back()!=L'\n')text+=L"\r\n";
        text+=part;
    }
    return text;
}
}