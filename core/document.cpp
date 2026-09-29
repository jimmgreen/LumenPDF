#include "document.h"
#include "annotation_geometry.h"
#include "font_catalog.h"
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <mupdf/ucdn.h>
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <climits>
#include <utility>
#include <cmath>
#include <cwctype>
#include <optional>
#include <cstring>
#include <list>
#include <map>
#include <set>
#include <unordered_map>
#include <numeric>
#include <sstream>
#include <type_traits>
namespace lpdf {
namespace {
#pragma warning(push)
#pragma warning(disable:4611) // Reviewed C-only longjmp boundary; no C++ owned values inside fz_try.
template<class F> auto Call(fz_context* ctx, F&& function) {
    using R = std::invoke_result_t<F>;
    // Only C API calls and trivial values belong inside this boundary. Keep
    // C++ allocations/RAII outside it: MuPDF errors use setjmp/longjmp.
    if constexpr (std::is_void_v<R>) {
        fz_try(ctx) { function(); }
        fz_catch(ctx) { throw std::runtime_error(fz_caught_message(ctx)); }
    } else {
        R value{}; fz_var(value);
        fz_try(ctx) { value = function(); }
        fz_catch(ctx) { throw std::runtime_error(fz_caught_message(ctx)); }
        return value;
    }
}
#pragma warning(pop)
fz_font* WindowsSystemFont(fz_context* ctx,const char* name,int bold,int italic,int){
    try{
        const auto source=ResolveSystemFont(Wide(name?name:""),bold!=0,italic!=0);
        const auto path=Utf8(source.path.wstring());
        auto* font=Call(ctx,[&]{return fz_new_font_from_file(ctx,nullptr,path.c_str(),static_cast<int>(source.faceIndex),0);});
        fz_set_font_embedding(ctx,font,1);
        auto* flags=fz_font_flags(font);flags->fake_bold=source.simulatedBold;flags->fake_italic=source.simulatedItalic;
        return font;
    }catch(...){return nullptr;}
}

fz_font* ChineseFallback(fz_context* ctx,int script,int,int,int bold,int italic){
    if(script!=UCDN_SCRIPT_HAN)return nullptr;
    return WindowsSystemFont(ctx,"Microsoft YaHei",bold,italic,0);
}

template<class T, void(*Drop)(fz_context*,T*)> struct Held {
    fz_context* ctx{}; T* p{};
    Held(fz_context* c, T* v=nullptr) : ctx(c), p(v) {}
    ~Held() { if (p) Drop(ctx,p); }
    Held(const Held&)=delete;
    Held& operator=(const Held&)=delete;
    Held(Held&& o) noexcept : ctx(o.ctx),p(std::exchange(o.p,nullptr)) {}
    T* get() const { return p; }
};
using Page = Held<pdf_page,pdf_drop_page>;
using Pdf = Held<pdf_document,pdf_drop_document>;
using Buffer = Held<fz_buffer,fz_drop_buffer>;
using Stream = Held<fz_stream,fz_drop_stream>;
using Object = Held<pdf_obj,pdf_drop_obj>;
using Device = Held<fz_device,fz_drop_device>;
using Pixmap = Held<fz_pixmap,fz_drop_pixmap>;
using Image = Held<fz_image,fz_drop_image>;
using Stext = Held<fz_stext_page,fz_drop_stext_page>;
using Outline = Held<fz_outline,fz_drop_outline>;
using LinkList = Held<fz_link,fz_drop_link>;
// A direct /Info dictionary attached to the unnumbered trailer has no journal parent.
// Normalize it before enabling the journal, so metadata edits can really be undone.
void PrepareInfoDictionary(fz_context* ctx,pdf_document* doc){
    auto* existing=Call(ctx,[&]{return pdf_dict_get(ctx,pdf_trailer(ctx,doc),PDF_NAME(Info));});
    if(Call(ctx,[&]{return pdf_is_indirect(ctx,existing)&&pdf_is_dict(ctx,existing);}))return;
    Object info(ctx,Call(ctx,[&]{return pdf_is_dict(ctx,existing)?pdf_keep_obj(ctx,existing):pdf_new_dict(ctx,doc,4);}));
    Object indirect(ctx,Call(ctx,[&]{return pdf_add_object(ctx,doc,info.get());}));
    Call(ctx,[&]{pdf_dict_put(ctx,pdf_trailer(ctx,doc),PDF_NAME(Info),indirect.get());});
}
fz_rect Native(Rect r) { return fz_make_rect(r.x,r.y,r.x+r.w,r.y+r.h); }
Rect Public(fz_rect r) { return {r.x0,r.y0,r.x1-r.x0,r.y1-r.y0}; }
std::string XmlText(std::string_view value){
    std::string out;
    for(char c:value){switch(c){case '&':out+="&amp;";break;case '<':out+="&lt;";break;case '>':out+="&gt;";break;case '\"':out+="&quot;";break;default:out+=c;}}
    return out;
}
void SetTextAppearance(fz_context* ctx,pdf_annot* annot,const TextFormat& format,float size,std::wstring_view text,bool fixed,TextSizing sizing){
    if(!std::isfinite(size)||size<6||size>96||format.alignment<0||format.alignment>2)throw std::runtime_error("Invalid text format");
    (void)ResolveSystemFont(format.family,format.bold,format.italic);
    const auto family=Utf8(format.family),content=Utf8(text);
    auto* testFont=WindowsSystemFont(ctx,family.c_str(),format.bold,format.italic,0);
    if(!testFont)throw std::runtime_error("The selected font cannot be embedded into this PDF");
    fz_drop_font(ctx,testFont);
    std::string quoted;
    for(char c:family){if(c=='\\'||c=='\'')quoted+='\\';quoted+=c;}
    char properties[240]{};
    snprintf(properties,sizeof(properties),";font-size:%gpt;line-height:1.2;white-space:pre-wrap;color:#%06x;font-weight:%s;font-style:%s;text-decoration:%s;text-align:%s;",
        size,format.color&0xffffff,format.bold?"bold":"normal",format.italic?"italic":"normal",format.underline?"underline":"none",
        format.alignment==1?"center":format.alignment==2?"right":"left");
    const std::string css="font-family:'"+quoted+"'"+properties;
    const std::string defaults=css+"padding:2pt;";
    const std::string rich="<body xmlns=\"http://www.w3.org/1999/xhtml\"><p style=\""+XmlText(css)+"margin:0;\">"+XmlText(content)+"</p></body>";
    const float color[]{((format.color>>16)&255)/255.0f,((format.color>>8)&255)/255.0f,(format.color&255)/255.0f};
    Call(ctx,[&]{
        pdf_set_annot_default_appearance(ctx,annot,"Helv",size,3,color);
        pdf_set_annot_quadding(ctx,annot,format.alignment);
        pdf_set_annot_rich_contents(ctx,annot,content.c_str(),rich.c_str());
        pdf_set_annot_rich_defaults(ctx,annot,defaults.c_str());
        auto* obj=pdf_annot_obj(ctx,annot);
        pdf_dict_puts_drop(ctx,obj,"LumenFontFamily",pdf_new_text_string(ctx,family.c_str()));
        pdf_dict_puts_drop(ctx,obj,"LumenFontBold",pdf_new_int(ctx,format.bold));
        pdf_dict_puts_drop(ctx,obj,"LumenFontItalic",pdf_new_int(ctx,format.italic));
        pdf_dict_puts_drop(ctx,obj,"LumenFontUnderline",pdf_new_int(ctx,format.underline));
        pdf_dict_puts_drop(ctx,obj,"LumenTextFixedBox",pdf_new_int(ctx,fixed));
        pdf_dict_puts_drop(ctx,obj,"LumenTextSizing",pdf_new_int(ctx,static_cast<int>(sizing)));
    });
}
// Measure the actual MuPDF appearance, using the same fallback fonts as saving.
void FitTextAnnotation(fz_context* ctx,pdf_annot* annot,float width,float fontSize){
    auto rect=Call(ctx,[&]{return pdf_annot_rect(ctx,annot);});
    rect.x1=rect.x0+std::max(width,fontSize*2);
    rect.y1=rect.y0+100000;
    Call(ctx,[&]{pdf_set_annot_rect(ctx,annot,rect);pdf_update_annot(ctx,annot);});
    Stext page(ctx,Call(ctx,[&]{return pdf_new_stext_page_from_annot(ctx,annot,nullptr);}));
    fz_rect ink=fz_empty_rect;
    for(auto* block=page.p->first_block;block;block=block->next){
        if(block->type!=FZ_STEXT_BLOCK_TEXT)continue;
        for(auto* line=block->u.t.first_line;line;line=line->next)
            for(auto* ch=line->first_char;ch;ch=ch->next)
                ink=fz_union_rect(ink,fz_rect_from_quad(ch->quad));
    }
    if(!fz_is_empty_rect(ink)){
        // Center/right alignment is relative to the user's wrap width. Tightening
        // that width would shift text sideways immediately after committing.
        if(Call(ctx,[&]{return pdf_annot_quadding(ctx,annot);})==0)
            rect.x1=rect.x0+std::min(std::max(width,fontSize*2),std::max(fontSize,ink.x1-rect.x0+2));
        rect.y1=rect.y0+std::max(fontSize*1.25f,ink.y1-rect.y0+2);
    }else{rect.x1=rect.x0+fontSize*2;rect.y1=rect.y0+fontSize*1.25f;}
    Call(ctx,[&]{pdf_set_annot_rect(ctx,annot,rect);pdf_update_annot(ctx,annot);});
}
std::string PathString(const fs::path& p) { return Utf8(p.wstring()); }
Tool Type(enum pdf_annot_type t) {
    switch(t) {
    case PDF_ANNOT_FREE_TEXT:return Tool::Text; case PDF_ANNOT_TEXT:return Tool::Note;
    case PDF_ANNOT_HIGHLIGHT:return Tool::Highlight; case PDF_ANNOT_SQUARE:return Tool::Rectangle;
    case PDF_ANNOT_LINE:return Tool::Arrow; case PDF_ANNOT_INK:return Tool::Ink;
    case PDF_ANNOT_STAMP:return Tool::Image; case PDF_ANNOT_UNDERLINE:return Tool::Underline;
    case PDF_ANNOT_STRIKE_OUT:return Tool::StrikeOut; case PDF_ANNOT_CIRCLE:return Tool::Ellipse;
    default:return Tool::Select;
    }
}
enum pdf_annot_type Type(Tool t) {
    switch(t) {
    case Tool::Text:return PDF_ANNOT_FREE_TEXT; case Tool::Note:return PDF_ANNOT_TEXT;
    case Tool::Highlight:return PDF_ANNOT_HIGHLIGHT; case Tool::Rectangle:return PDF_ANNOT_SQUARE;
    case Tool::Arrow:return PDF_ANNOT_LINE; case Tool::Ink:return PDF_ANNOT_INK;
    case Tool::Image:case Tool::Stamp:return PDF_ANNOT_STAMP; case Tool::Line:return PDF_ANNOT_LINE;
    case Tool::Underline:return PDF_ANNOT_UNDERLINE; case Tool::StrikeOut:return PDF_ANNOT_STRIKE_OUT;
    case Tool::Ellipse:return PDF_ANNOT_CIRCLE; default:throw std::runtime_error("Choose an annotation tool");
    }
}
void BakeVisible(fz_context* ctx,pdf_document* doc){
    // A reading export must not expose annotations that were marked NoView.
    // MuPDF's generic baker filters Invisible/Hidden, but not NoView.
    Call(ctx,[&]{
        const int count=pdf_count_pages(ctx,doc);
        for(int p=0;p<count;++p){
            auto* annotations=pdf_dict_get(ctx,pdf_lookup_page_obj(ctx,doc,p),PDF_NAME(Annots));
            for(int i=0;i<pdf_array_len(ctx,annotations);++i){
                auto* a=pdf_array_get(ctx,annotations,i);const int flags=pdf_dict_get_int(ctx,a,PDF_NAME(F));
                if(flags&PDF_ANNOT_IS_NO_VIEW)pdf_dict_put_int(ctx,a,PDF_NAME(F),flags|PDF_ANNOT_IS_HIDDEN);
            }
        }
        pdf_bake_document(ctx,doc,1,1);
    });
}
std::string Html(std::wstring_view text) {
    std::string html="<html><body><div style=\"white-space:pre-wrap;font-family:sans-serif;font-size:11pt;line-height:1.6\">";
    for(char c:Utf8(text)) {
        if(c=='&')html+="&amp;"; else if(c=='<')html+="&lt;"; else if(c=='>')html+="&gt;"; else html+=c;
    }
    return html+"</div></body></html>";
}
struct TemporaryFile {
    fs::path path;
    ~TemporaryFile(){std::error_code error;fs::remove(path,error);}
};
}
std::vector<int> ParsePageRange(std::wstring_view text,int count) {
    if(count<=0)throw std::runtime_error("The document has no pages");
    std::wstring normalized;
    for(wchar_t ch:text)if(!iswspace(ch))normalized+=ch==L'，'?L',':ch;
    std::vector<int> result;
    if(normalized.empty()||normalized==L"全部"){
        result.resize(static_cast<size_t>(count));std::iota(result.begin(),result.end(),0);return result;
    }
    size_t pos=0;
    auto number=[&](){
        if(pos>=normalized.size()||normalized[pos]<L'0'||normalized[pos]>L'9')throw std::runtime_error("Invalid page range");
        int n=0;
        while(pos<normalized.size()&&normalized[pos]>=L'0'&&normalized[pos]<=L'9'){
            if(n>count)throw std::runtime_error("Page number exceeds document length");
            n=n*10+(normalized[pos++]-L'0');
        }
        if(n<1||n>count)throw std::runtime_error("Page number exceeds document length");
        return n;
    };
    while(pos<normalized.size()){
        const int first=number();int last=first;
        if(pos<normalized.size()&&normalized[pos]==L'-'){++pos;last=number();}
        if(last<first)throw std::runtime_error("Page range must be ascending");
        for(int n=first;n<=last;++n)result.push_back(n-1);
        if(pos==normalized.size())break;
        if(normalized[pos++]!=L','||pos==normalized.size())throw std::runtime_error("Invalid page range");
    }
    return result;
}
namespace {
// MuPDF 按字体摘要缓存已嵌入的字体对象；撤销会删除这些对象而缓存仍指向它们，
// 再次写入内容时就会引用不存在的对象。撤销/重做后以及生成新内容前清空缓存。
void ResetResourceCache(fz_context* ctx,pdf_document* doc){
    if(!doc)return;
    pdf_drop_resource_tables(ctx,doc);
    doc->resources.fonts=nullptr;doc->resources.colorspaces=nullptr;doc->resources.images=nullptr;
}
}
struct Document::Impl {
    fz_context* ctx{};
    pdf_document* doc{};
    std::vector<unsigned char> bytes; std::wstring openedPassword;
    bool dirty{};
    fz_stext_page* selectionText{};
    int selectionPage{-1};
    // Render cache: one display list per (page, hidden annotation, hide UI text) so the
    // many tiles of a page replay recorded drawing commands instead of re-parsing the
    // content stream each time. Any mutation calls Invalidate(); caches are local to the
    // single worker thread that owns this document.
    struct ListKey {int page;int hidden;bool hideText;bool operator==(const ListKey&)const=default;};
    struct ListKeyHash {size_t operator()(const ListKey& k)const noexcept{return (static_cast<size_t>(k.page)*1000003u)^(static_cast<size_t>(k.hidden+1)*31u)^static_cast<size_t>(k.hideText?1u:0u);}};
    struct ListEntry {fz_display_list* list{};fz_rect bounds{};bool incomplete{};};
    std::list<ListKey> listOrder;
    std::unordered_map<ListKey,std::pair<ListEntry,std::list<ListKey>::iterator>,ListKeyHash> lists;
    std::vector<std::optional<PageInfo>> geometry;
    static constexpr size_t kMaxLists=48;
    // Page text (content only, no annotations) cached for interactive selection.
    fz_stext_page* SelectionPage(int number){
        if(!selectionText||selectionPage!=number){
            ClearSelection();auto page=Load(number);
            Stext st(ctx,Call(ctx,[&]{return fz_new_stext_page(ctx,pdf_bound_page(ctx,page.get(),FZ_CROP_BOX));}));
            Device dev(ctx,Call(ctx,[&]{return fz_new_stext_device(ctx,st.get(),nullptr);}));
            Call(ctx,[&]{pdf_run_page_contents(ctx,page.get(),dev.get(),fz_identity,nullptr);fz_close_device(ctx,dev.get());});
            selectionText=std::exchange(st.p,nullptr);selectionPage=number;
        }
        return selectionText;
    }
    // MuPDF otherwise snaps a drag in empty margins to unrelated text.
    bool SelectionNearText(int number,Point start,Point end){
        auto* text=SelectionPage(number);
        for(auto* block=text->first_block;block;block=block->next){
            if(block->type!=FZ_STEXT_BLOCK_TEXT)continue;
            for(auto* line=block->u.t.first_line;line;line=line->next){
                const auto box=fz_expand_rect(line->bbox,5);
                auto inside=[&](Point p){return p.x>=box.x0&&p.x<=box.x1&&p.y>=box.y0&&p.y<=box.y1;};
                if(inside(start)||inside(end))return true;
            }
        }
        return false;
    }
    void ClearSelection(){if(selectionText)fz_drop_stext_page(ctx,selectionText);selectionText=nullptr;selectionPage=-1;}
    // 跨页选择：fromStart 取本页第一个字符、toEnd 取最后一个字符（MuPDF 选择按结构化文本顺序，首末字符即范围两端）。
    bool EdgePoints(int number,bool fromStart,bool toEnd,Point& start,Point& end){
        auto* text=SelectionPage(number);fz_stext_char* first=nullptr;fz_stext_char* last=nullptr;
        for(auto* block=text->first_block;block;block=block->next){
            if(block->type!=FZ_STEXT_BLOCK_TEXT)continue;
            for(auto* line=block->u.t.first_line;line;line=line->next)for(auto* ch=line->first_char;ch;ch=ch->next){if(!first)first=ch;last=ch;}
        }
        if(!first)return false;
        if(fromStart)start=CharPoint(first->quad,.2f);
        if(toEnd)end=CharPoint(last->quad,.8f);
        return true;
    }
    // 字符中线上的点：t<.5 落在字符前半（选择从此字符开始），t>.5 落在后半（选择包含此字符）。
    static Point CharPoint(const fz_quad& q,float t){
        const float lx=(q.ul.x+q.ll.x)*.5f,ly=(q.ul.y+q.ll.y)*.5f,rx=(q.ur.x+q.lr.x)*.5f,ry=(q.ur.y+q.lr.y)*.5f;
        return {lx+(rx-lx)*t,ly+(ry-ly)*t};
    }
    void DropLists(){for(auto& [key,value]:lists){(void)key;if(value.first.list)fz_drop_display_list(ctx,value.first.list);}lists.clear();listOrder.clear();}
    void Invalidate(){DropLists();geometry.clear();}
    PageInfo Geometry(int index){
        if(geometry.empty())geometry.resize(static_cast<size_t>(Count()));
        if(index<0||index>=static_cast<int>(geometry.size()))throw std::runtime_error("Page index out of range");
        auto& slot=geometry[static_cast<size_t>(index)];
        if(!slot){
            // Same computation as pdf_bound_page(FZ_CROP_BOX) without loading the page.
            const auto b=Call(ctx,[&]{
                fz_rect box;fz_matrix ctm;pdf_page_obj_transform_box(ctx,pdf_lookup_page_obj(ctx,doc,index),&box,&ctm,FZ_CROP_BOX);
                return fz_transform_rect(box,ctm);
            });
            slot=PageInfo{b.x1-b.x0,b.y1-b.y0,b.x0,b.y0};
        }
        return *slot;
    }
    Impl(){ctx=fz_new_context(nullptr,nullptr,96*1024*1024);if(!ctx)throw std::bad_alloc();fz_install_load_system_font_funcs(ctx,WindowsSystemFont,nullptr,ChineseFallback);}
    ~Impl(){ClearSelection();DropLists();if(doc)pdf_drop_document(ctx,doc);if(ctx)fz_drop_context(ctx);}
    void Require(){if(!doc)throw std::runtime_error("Open a PDF first");}
    int Count(){Require();return Call(ctx,[&]{return pdf_count_pages(ctx,doc);});}
    Page Load(int index){
        if(index<0||index>=Count())throw std::runtime_error("Page index out of range");
        return Page(ctx,Call(ctx,[&]{return pdf_load_page(ctx,doc,index);}));
    }
    template<class F> void Edit(const char* name,F&& operation){
        Require();ClearSelection();Invalidate();Call(ctx,[&]{pdf_begin_operation(ctx,doc,name);});
        try {operation();Call(ctx,[&]{pdf_end_operation(ctx,doc);});dirty=true;Invalidate();}
        catch(...){try{Call(ctx,[&]{pdf_abandon_operation(ctx,doc);});}catch(...){}ResetResourceCache(ctx,doc);Invalidate();throw;}
    }
    pdf_annot* Find(pdf_page* page,int id){
        pdf_annot* a=Call(ctx,[&]{return pdf_first_annot(ctx,page);});
        while(a){
            if(Call(ctx,[&]{return pdf_to_num(ctx,pdf_annot_obj(ctx,a));})==id)return a;
            a=Call(ctx,[&]{return pdf_next_annot(ctx,a);});
        }
        throw std::runtime_error("Annotation is no longer available");
    }
    void Open(const fs::path& path,std::wstring_view password){
        auto data=ReadBytes(path);
        if(data.empty())throw std::runtime_error("The file is empty");
        Stream stream(ctx,Call(ctx,[&]{return fz_open_memory(ctx,data.data(),data.size());}));
        Pdf next(ctx,Call(ctx,[&]{return pdf_open_document_with_stream(ctx,stream.get());}));
        const auto pass=Utf8(password);
        if(Call(ctx,[&]{return pdf_needs_password(ctx,next.get());}) &&
            !Call(ctx,[&]{return pdf_authenticate_password(ctx,next.get(),pass.c_str());}))throw PasswordRequired{};
        if(Call(ctx,[&]{return pdf_count_pages(ctx,next.get());})<1)throw std::runtime_error("The PDF contains no pages");
        PrepareInfoDictionary(ctx,next.get());
        Call(ctx,[&]{pdf_enable_journal(ctx,next.get());});
        ClearSelection();Invalidate();if(doc)pdf_drop_document(ctx,doc);
        doc=std::exchange(next.p,nullptr);bytes=std::move(data);openedPassword=password;dirty=false;
    }
    static constexpr int kDedupeObjectLimit=4000;
    void SaveRaw(const fs::path& path,bool optimize=false){
        const auto filename=PathString(path);
        pdf_write_options options=pdf_default_write_options;
        options.do_compress=1; options.do_compress_images=1; options.do_compress_fonts=1;
        // Renumber only the disposable save copy, never the active undo journal.
        // Level 3 also removes duplicate objects, but MuPDF compares every object pair (O(n²), repeated
        // until nothing changes): a merged 200-page drawing set spent 10+ minutes here. Large files get
        // level 2 (collect + compact, linear); small files keep deduplication.
        int garbage=0;
        if(optimize){
            int limit=kDedupeObjectLimit;
            wchar_t forced[16]{};  // 诊断用：LPDF_DEDUPE_LIMIT 覆盖阈值（基准测试对比新旧耗时）。
            if(GetEnvironmentVariableW(L"LPDF_DEDUPE_LIMIT",forced,16))limit=_wtoi(forced);
            garbage=Call(ctx,[&]{return pdf_xref_len(ctx,doc);})<=limit?3:2;
        }
        options.do_garbage=garbage;
        Call(ctx,[&]{pdf_save_document(ctx,doc,filename.c_str(),&options);});
    }
};
Document::Document():impl_(std::make_unique<Impl>()){}
Document::~Document()=default;
void Document::Open(const fs::path& path,std::wstring_view password){impl_->Open(path,password);}
void Document::Close(){
    auto& d=*impl_;d.ClearSelection();d.Invalidate();
    if(d.doc){pdf_drop_document(d.ctx,d.doc);d.doc=nullptr;}
    d.bytes.clear();d.openedPassword.clear();d.dirty=false;
}
bool Document::IsOpen()const{return impl_->doc!=nullptr;}
void Document::Reload(const fs::path& path){const auto password=impl_->openedPassword;Open(path,password);}
void Document::New(){
    auto& d=*impl_;Pdf next(d.ctx,Call(d.ctx,[&]{return pdf_create_document(d.ctx);}));
    PrepareInfoDictionary(d.ctx,next.get());
    Call(d.ctx,[&]{pdf_enable_journal(d.ctx,next.get());});
    d.ClearSelection();d.Invalidate();if(d.doc)pdf_drop_document(d.ctx,d.doc);
    d.doc=std::exchange(next.p,nullptr);d.bytes.clear();d.openedPassword.clear();d.dirty=false;InsertBlank(-1);
}
DocumentInfo Document::Info(){
    auto& d=*impl_;DocumentInfo info;
    // 未打开文档时 d.doc 为空：pdf_trailer 会直接解引用空指针（不是 MuPDF 异常），必须先判断。
    info.hasForm=d.doc&&Call(d.ctx,[&]{return pdf_array_len(d.ctx,pdf_dict_getp(d.ctx,pdf_trailer(d.ctx,d.doc),"Root/AcroForm/Fields"))>0;});
    const int n=d.Count();info.pages.reserve(static_cast<size_t>(n));
    for(int i=0;i<n;++i){info.pages.push_back(d.Geometry(i));char label[1024]{};Call(d.ctx,[&]{pdf_page_label(d.ctx,d.doc,i,label,sizeof(label));});info.labels.push_back(Wide(label));}
    Outline outline(d.ctx,Call(d.ctx,[&]{return fz_load_outline(d.ctx,reinterpret_cast<fz_document*>(d.doc));}));
    std::function<void(fz_outline*,int)> visit=[&](fz_outline* o,int depth){
        for(;o;o=o->next){
            if(o->page.page>=0)info.outline.push_back({Wide(o->title?o->title:""),o->page.page,depth,std::isfinite(o->y)?o->y:NAN});
            else ++info.outlineSkipped;
            if(depth<32)visit(o->down,depth+1);
        }
    };visit(outline.get(),0);
    info.canUndo=Call(d.ctx,[&]{return pdf_can_undo(d.ctx,d.doc);})!=0;
    info.canRedo=Call(d.ctx,[&]{return pdf_can_redo(d.ctx,d.doc);})!=0;
    info.dirty=d.dirty;info.encrypted=d.doc->crypt!=nullptr;return info;
}
DocumentMetadata Document::Metadata(){
    auto& d=*impl_;d.Require();DocumentMetadata m;
    auto value=[&](const char* key){const int n=Call(d.ctx,[&]{return pdf_lookup_metadata(d.ctx,d.doc,key,nullptr,0);});if(n<=0)return std::wstring{};if(n>1048576)throw std::runtime_error("Metadata field exceeds limit");std::vector<char> text(static_cast<size_t>(n)+1);Call(d.ctx,[&]{pdf_lookup_metadata(d.ctx,d.doc,key,text.data(),text.size());});return Wide(text.data());};
    m.title=value("info:Title");m.author=value("info:Author");m.subject=value("info:Subject");m.keywords=value("info:Keywords");m.creator=value("info:Creator");m.producer=value("info:Producer");m.created=value("info:CreationDate");m.modified=value("info:ModDate");m.format=value("format");return m;
}
void Document::SetMetadata(const DocumentMetadata& m){
    auto& d=*impl_;const std::pair<const char*,std::string> values[]={{"Title",Utf8(m.title)},{"Author",Utf8(m.author)},{"Subject",Utf8(m.subject)},{"Keywords",Utf8(m.keywords)}};
    for(const auto& v:values)if(v.second.size()>65536)throw std::runtime_error("Metadata field exceeds limit");
    d.Edit("Edit metadata",[&]{
        auto* info=Call(d.ctx,[&]{return pdf_dict_get(d.ctx,pdf_trailer(d.ctx,d.doc),PDF_NAME(Info));});
        if(!info){Object obj(d.ctx,Call(d.ctx,[&]{return pdf_new_dict(d.ctx,d.doc,4);}));Call(d.ctx,[&]{pdf_dict_put(d.ctx,pdf_trailer(d.ctx,d.doc),PDF_NAME(Info),obj.get());});info=Call(d.ctx,[&]{return pdf_dict_get(d.ctx,pdf_trailer(d.ctx,d.doc),PDF_NAME(Info));});}
        for(const auto& v:values)Call(d.ctx,[&]{pdf_dict_puts_drop(d.ctx,info,v.first,pdf_new_text_string(d.ctx,v.second.c_str()));});
    });
}
void Document::SetPageLabels(int first,int style,std::wstring_view prefix,int start){
    auto& d=*impl_;if(first<0||first>=d.Count()||start<1)throw std::runtime_error("Invalid page label range");
    if(style!=0&&style!='D'&&style!='R'&&style!='r'&&style!='A'&&style!='a')throw std::runtime_error("Invalid page label style");
    const auto text=Utf8(prefix);if(text.size()>512)throw std::runtime_error("Page label prefix too long");
    d.Edit("Page labels",[&]{Call(d.ctx,[&]{pdf_set_page_labels(d.ctx,d.doc,first,static_cast<pdf_page_label_style>(style),text.c_str(),start);});});
}
namespace {
// UI text can be composited after a PDF tile only when no later engine-painted
// annotation covers it. Walk backwards to preserve the document's annotation
// stacking order, including chains of overlapping text and non-text comments.
std::vector<int> LumenTextLayer(fz_context* ctx,pdf_page* page){
    struct Item {int id;fz_rect bounds;bool text;};
    std::vector<Item> items;
    for(auto* a=Call(ctx,[&]{return pdf_first_annot(ctx,page);});a;a=Call(ctx,[&]{return pdf_next_annot(ctx,a);})){
        const int flags=Call(ctx,[&]{return pdf_dict_get_int(ctx,pdf_annot_obj(ctx,a),PDF_NAME(F));});
        if(flags&(PDF_ANNOT_IS_HIDDEN|PDF_ANNOT_IS_NO_VIEW|PDF_ANNOT_IS_INVISIBLE))continue;
        const bool text=Call(ctx,[&]{
            const auto* family=pdf_to_text_string(ctx,pdf_dict_gets(ctx,pdf_annot_obj(ctx,a),"LumenFontFamily"));
            return pdf_annot_type(ctx,a)==PDF_ANNOT_FREE_TEXT&&pdf_dict_get_int(ctx,pdf_annot_obj(ctx,a),PDF_NAME(Rotate))==0&&family&&*family;
        });
        const int id=Call(ctx,[&]{return pdf_to_num(ctx,pdf_annot_obj(ctx,a));});
        const auto bounds=Call(ctx,[&]{return pdf_bound_annot(ctx,a);});
        items.push_back({id,bounds,text});
    }
    std::vector<fz_rect> covered;std::vector<int> result;
    for(auto it=items.rbegin();it!=items.rend();++it){
        const bool clear=it->text&&std::none_of(covered.begin(),covered.end(),[&](fz_rect r){return !fz_is_empty_rect(fz_intersect_rect(r,it->bounds));});
        if(clear)result.push_back(it->id);else covered.push_back(it->bounds);
    }
    return result;
}
}
namespace {
using DisplayList = Held<fz_display_list,fz_drop_display_list>;
// Record one page variant into a display list. Only the list device is driven here;
// replay rasterizes each tile. Mirrors the previous direct-draw paths exactly.
void RecordPage(fz_context* ctx,pdf_page* page,fz_device* device,int hiddenAnnotation,const std::vector<int>& uiText,fz_cookie* cookie){
    if(hiddenAnnotation==-2){pdf_run_page_contents(ctx,page,device,fz_identity,cookie);pdf_run_page_widgets(ctx,page,device,fz_identity,cookie);return;}
    if(hiddenAnnotation<0&&uiText.empty()){pdf_run_page(ctx,page,device,fz_identity,cookie);return;}
    pdf_run_page_contents(ctx,page,device,fz_identity,cookie);
    for(auto* a=pdf_first_annot(ctx,page);a;a=pdf_next_annot(ctx,a)){
        const int id=pdf_to_num(ctx,pdf_annot_obj(ctx,a));
        if(id!=hiddenAnnotation&&std::find(uiText.begin(),uiText.end(),id)==uiText.end())pdf_run_annot(ctx,a,device,fz_identity,cookie);
    }
    pdf_run_page_widgets(ctx,page,device,fz_identity,cookie);
}
}
Bitmap Document::Render(int number,float scale,std::optional<Rect> clip,int hiddenAnnotation,bool hideLumenText,bool hideAnnotations){
    auto& d=*impl_;d.Require();
    if(!std::isfinite(scale)||scale<=0||scale>8)throw std::runtime_error("Invalid render scale");
    if(hideAnnotations){hiddenAnnotation=-2;hideLumenText=false;}
    const Impl::ListKey key{number,hiddenAnnotation,hideLumenText};
    auto found=d.lists.find(key);
    if(found==d.lists.end()){
        auto page=d.Load(number);
        const auto bounds=Call(d.ctx,[&]{return pdf_bound_page(d.ctx,page.get(),FZ_CROP_BOX);});
        const auto uiText=hideLumenText?LumenTextLayer(d.ctx,page.get()):std::vector<int>{};
        DisplayList list(d.ctx,Call(d.ctx,[&]{return fz_new_display_list(d.ctx,bounds);}));
        Device recorder(d.ctx,Call(d.ctx,[&]{return fz_new_list_device(d.ctx,list.get());}));
        fz_cookie cookie{};
        Call(d.ctx,[&]{RecordPage(d.ctx,page.get(),recorder.get(),hiddenAnnotation,uiText,&cookie);fz_close_device(d.ctx,recorder.get());});
        while(d.lists.size()>=Impl::kMaxLists&&!d.listOrder.empty()){
            auto last=d.lists.find(d.listOrder.back());
            if(last!=d.lists.end()){if(last->second.first.list)fz_drop_display_list(d.ctx,last->second.first.list);d.lists.erase(last);}
            d.listOrder.pop_back();
        }
        d.listOrder.push_front(key);
        found=d.lists.emplace(key,std::make_pair(Impl::ListEntry{std::exchange(list.p,nullptr),bounds,cookie.errors!=0},d.listOrder.begin())).first;
    }else if(found->second.second!=d.listOrder.begin()){
        d.listOrder.splice(d.listOrder.begin(),d.listOrder,found->second.second);
    }
    const auto& entry=found->second.first;
    if(entry.incomplete)throw std::runtime_error("PDF page could not be rendered completely");
    const fz_rect region=clip?fz_intersect_rect(entry.bounds,Native(*clip)):entry.bounds;
    const auto matrix=fz_scale(scale,scale);
    const auto box=fz_round_rect(fz_transform_rect(region,matrix));
    if(box.x1<=box.x0||box.y1<=box.y0||static_cast<int64_t>(box.x1-box.x0)*(box.y1-box.y0)>20000000)
        throw std::runtime_error("Render region exceeds bitmap limit");
    Pixmap pix(d.ctx,Call(d.ctx,[&]{return fz_new_pixmap_with_bbox(d.ctx,fz_device_bgr(d.ctx),box,nullptr,1);}));
    Call(d.ctx,[&]{fz_clear_pixmap_with_value(d.ctx,pix.get(),255);});
    Device device(d.ctx,Call(d.ctx,[&]{return fz_new_draw_device(d.ctx,fz_identity,pix.get());}));
    fz_cookie cookie{};
    Call(d.ctx,[&]{fz_run_display_list(d.ctx,entry.list,device.get(),matrix,fz_transform_rect(region,matrix),&cookie);fz_close_device(d.ctx,device.get());});
    if(cookie.errors)throw std::runtime_error("PDF page could not be rendered completely");
    Bitmap result;result.width=pix.p->w;result.height=pix.p->h;result.stride=pix.p->stride;
    result.bgra.assign(pix.p->samples,pix.p->samples+static_cast<size_t>(result.stride)*result.height);
    return result;
}
Sprite Document::RenderAnnotation(int number,int id,float scale){
    auto& d=*impl_;auto page=d.Load(number);auto* annot=d.Find(page.get(),id);
    if(!std::isfinite(scale)||scale<=0||scale>8)throw std::runtime_error("Invalid render scale");
    // 外扩 2 pt 容纳边框与抗锯齿，坐标系与 Render 的页面裁剪一致。
    const auto region=fz_expand_rect(Call(d.ctx,[&]{return pdf_bound_annot(d.ctx,annot);}),2);
    const auto matrix=fz_scale(scale,scale);
    const auto box=fz_round_rect(fz_transform_rect(region,matrix));
    if(box.x1<=box.x0||box.y1<=box.y0||static_cast<int64_t>(box.x1-box.x0)*(box.y1-box.y0)>20000000)
        throw std::runtime_error("Annotation region exceeds bitmap limit");
    Pixmap pix(d.ctx,Call(d.ctx,[&]{return fz_new_pixmap_with_bbox(d.ctx,fz_device_bgr(d.ctx),box,nullptr,1);}));
    Call(d.ctx,[&]{fz_clear_pixmap(d.ctx,pix.get());});
    Device device(d.ctx,Call(d.ctx,[&]{return fz_new_draw_device(d.ctx,fz_identity,pix.get());}));
    fz_cookie cookie{};
    Call(d.ctx,[&]{pdf_run_annot(d.ctx,annot,device.get(),matrix,&cookie);fz_close_device(d.ctx,device.get());});
    if(cookie.errors)throw std::runtime_error("Annotation could not be rendered completely");
    Sprite sprite;
    sprite.bounds={box.x0/scale,box.y0/scale,(box.x1-box.x0)/scale,(box.y1-box.y0)/scale};
    sprite.pixels.width=pix.p->w;sprite.pixels.height=pix.p->h;sprite.pixels.stride=pix.p->stride;
    sprite.pixels.bgra.assign(pix.p->samples,pix.p->samples+static_cast<size_t>(sprite.pixels.stride)*sprite.pixels.height);
    return sprite;
}
namespace {
uint32_t Rgb(int n,const float* c,uint32_t fallback){
    float r=0,g=0,b=0;
    if(n==1)r=g=b=c[0];
    else if(n==3){r=c[0];g=c[1];b=c[2];}
    else if(n==4){r=1-std::min(1.0f,c[0]+c[3]);g=1-std::min(1.0f,c[1]+c[3]);b=1-std::min(1.0f,c[2]+c[3]);}
    else return fallback;
    auto byte=[](float f){return static_cast<uint32_t>(std::lround(std::clamp(f,0.0f,1.0f)*255));};
    return (byte(r)<<16)|(byte(g)<<8)|byte(b);
}
void SetStyle(fz_context* ctx,pdf_annot* a,Tool tool,const AnnotationStyle& style){
    const float color[]{((style.color>>16)&255)/255.0f,((style.color>>8)&255)/255.0f,(style.color&255)/255.0f};
    const float fill[]{((style.fillColor>>16)&255)/255.0f,((style.fillColor>>8)&255)/255.0f,(style.fillColor&255)/255.0f};
    Call(ctx,[&]{
        if(tool!=Tool::Text&&tool!=Tool::Image)pdf_set_annot_color(ctx,a,3,color);
        if(IsStrokedTool(tool)){
            pdf_set_annot_border_width(ctx,a,style.lineWidth);
            pdf_set_annot_border_style(ctx,a,style.dashed?PDF_BORDER_STYLE_DASHED:PDF_BORDER_STYLE_SOLID);
            pdf_clear_annot_border_dash(ctx,a);
            if(style.dashed){pdf_add_annot_border_dash_item(ctx,a,4);pdf_add_annot_border_dash_item(ctx,a,3);}
        }
        if(IsBoxShape(tool))pdf_set_annot_interior_color(ctx,a,style.filled?3:0,fill);
        if(IsLineTool(tool)){
            // 直线始终无端点；箭头按样式设置端点。
            const auto start=tool==Tool::Line?PDF_ANNOT_LE_NONE:static_cast<pdf_line_ending>(style.startEnding);
            const auto end=tool==Tool::Line?PDF_ANNOT_LE_NONE:static_cast<pdf_line_ending>(style.endEnding);
            pdf_set_annot_line_ending_styles(ctx,a,start,end);
            pdf_dict_puts_drop(ctx,pdf_annot_obj(ctx,a),"LumenLine",pdf_new_int(ctx,tool==Tool::Line));
            // A closed arrowhead is filled in the same ink as its shaft.
            pdf_set_annot_interior_color(ctx,a,3,color);
        }
    });
}
void ValidateGeometry(Rect r,float opacity,const AnnotationStyle& style,const std::vector<Point>& points){
    if(!std::isfinite(r.x)||!std::isfinite(r.y)||!std::isfinite(r.w)||!std::isfinite(r.h)||r.w<=0||r.h<=0||
       !std::isfinite(opacity)||opacity<.05f||opacity>1||!std::isfinite(style.lineWidth)||style.lineWidth<0||style.lineWidth>72)
        throw std::runtime_error("Invalid annotation geometry or appearance");
    if(points.size()>262144)throw std::runtime_error("The drawing contains too many points");
    for(auto p:points)if(!std::isfinite(p.x)||!std::isfinite(p.y))throw std::runtime_error("Invalid drawing point");
    if(style.startEnding<0||style.startEnding>9||style.endEnding<0||style.endEnding>9)throw std::runtime_error("Invalid arrow ending");
}
// 同一 PDF 子类型下区分 LumenPDF 的工具：直线/箭头都是 Line，文字印章/图片都是 Stamp。
Tool Detect(fz_context* ctx,pdf_annot* a){
    return Call(ctx,[&]{
        const auto type=pdf_annot_type(ctx,a);auto* obj=pdf_annot_obj(ctx,a);
        if(type==PDF_ANNOT_LINE){
            if(auto* key=pdf_dict_gets(ctx,obj,"LumenLine"))return pdf_to_int(ctx,key)?Tool::Line:Tool::Arrow;
            pdf_line_ending start{},end{};pdf_annot_line_ending_styles(ctx,a,&start,&end);
            return start==PDF_ANNOT_LE_NONE&&end==PDF_ANNOT_LE_NONE?Tool::Line:Tool::Arrow;
        }
        if(type==PDF_ANNOT_STAMP&&pdf_is_string(ctx,pdf_dict_gets(ctx,obj,"LumenStamp")))return Tool::Stamp;
        return Type(type);
    });
}
void DropText(fz_context* ctx,fz_text* v){fz_drop_text(ctx,v);}
void DropPath(fz_context* ctx,fz_path* v){fz_drop_path(ctx,v);}
void DropStroke(fz_context* ctx,fz_stroke_state* v){fz_drop_stroke_state(ctx,v);}
// 只含 POD 局部变量，fz_try 的 setjmp 不跨越 C++ 析构。
pdf_obj* AddContentStream(fz_context* ctx,pdf_document* doc,const char* data,size_t size){
    fz_buffer* b=fz_new_buffer_from_copied_data(ctx,reinterpret_cast<const unsigned char*>(data),size);
    pdf_obj* ref=nullptr;
#pragma warning(push)
#pragma warning(disable:4611)
    fz_try(ctx)ref=pdf_add_stream(ctx,doc,b,nullptr,0);
    fz_always(ctx)fz_drop_buffer(ctx,b);
    fz_catch(ctx)fz_rethrow(ctx);
#pragma warning(pop)
    return ref;
}
using FontHeld = Held<fz_font,fz_drop_font>;
using TextHeld = Held<fz_text,DropText>;
using PathHeld = Held<fz_path,DropPath>;
FontHeld SystemFont(fz_context* ctx,bool bold){
    FontHeld font(ctx,WindowsSystemFont(ctx,"Microsoft YaHei",bold,0,0));
    if(!font.get())font.p=WindowsSystemFont(ctx,"SimSun",bold,0,0);
    if(!font.get())throw std::runtime_error("No usable Chinese system font was found");
    return font;
}
// 文本宽度（fitz 页面坐标，y 向下；字号 size）。
float TextWidth(fz_context* ctx,fz_font* font,const std::string& text,float size){
    return Call(ctx,[&]{return fz_measure_string(ctx,font,fz_make_matrix(size,0,0,-size,0,0),text.c_str(),0,0,FZ_BIDI_LTR,FZ_LANG_UNSET).e;});
}
// 在 placement（把基线原点放到页面上）处绘制一行文字。
void DrawText(fz_context* ctx,fz_device* dev,fz_font* font,const std::string& text,float size,fz_matrix placement,uint32_t rgb,float alpha){
    const float color[]{((rgb>>16)&255)/255.0f,((rgb>>8)&255)/255.0f,(rgb&255)/255.0f};
    TextHeld run(ctx,Call(ctx,[&]{return fz_new_text(ctx);}));
    Call(ctx,[&]{
        fz_show_string(ctx,run.get(),font,fz_make_matrix(size,0,0,-size,0,0),text.c_str(),0,0,FZ_BIDI_LTR,FZ_LANG_UNSET);
        fz_fill_text(ctx,dev,run.get(),placement,fz_device_rgb(ctx),color,alpha,fz_default_color_params);
    });
}
void RoundRect(fz_context* ctx,fz_path* path,float x0,float y0,float x1,float y1,float r){
    r=std::max(0.0f,std::min({r,(x1-x0)/2,(y1-y0)/2}));const float k=r*.5523f;
    fz_moveto(ctx,path,x0+r,y0);fz_lineto(ctx,path,x1-r,y0);fz_curveto(ctx,path,x1-r+k,y0,x1,y0+r-k,x1,y0+r);
    fz_lineto(ctx,path,x1,y1-r);fz_curveto(ctx,path,x1,y1-r+k,x1-r+k,y1,x1-r,y1);
    fz_lineto(ctx,path,x0+r,y1);fz_curveto(ctx,path,x0+r-k,y1,x0,y1-r+k,x0,y1-r);
    fz_lineto(ctx,path,x0,y0+r);fz_curveto(ctx,path,x0,y0+r-k,x0+r-k,y0,x0+r,y0);fz_closepath(ctx,path);
}
constexpr float StampPadding=.42f;   // 左右留白（相对高度）
constexpr float StampText=.5f;       // 字号（相对高度）
// 文字印章：双线圆角边框 + 居中粗体文字，外观由 LumenPDF 生成（支持中文），名称非标准，MuPDF 不会重绘覆盖。
void SetStampAppearance(fz_context* ctx,pdf_document* doc,pdf_annot* annot,Rect bounds,const std::string& text,uint32_t rgb,float opacity){
    // MuPDF 首次更新会按标准橡皮章比例改 Rect；这里恢复为 LumenPDF 的尺寸再生成外观。
    const auto rect=Native(bounds);Call(ctx,[&]{pdf_set_annot_rect(ctx,annot,rect);});
    const float w=std::max(8.0f,rect.x1-rect.x0),h=std::max(8.0f,rect.y1-rect.y0);
    auto font=SystemFont(ctx,true);
    Buffer contents(ctx,Call(ctx,[&]{return fz_new_buffer(ctx,512);}));
    Object resources(ctx,Call(ctx,[&]{return pdf_new_dict(ctx,doc,2);}));
    ResetResourceCache(ctx,doc);
    Device dev(ctx,Call(ctx,[&]{return pdf_new_pdf_device(ctx,doc,fz_make_matrix(1,0,0,-1,0,h),resources.get(),contents.get());}));
    const float color[]{((rgb>>16)&255)/255.0f,((rgb>>8)&255)/255.0f,(rgb&255)/255.0f};
    const float outer=std::clamp(h*.055f,.8f,6.0f),inner=outer*.45f;
    for(int ring=0;ring<2;++ring){
        const float width=ring?inner:outer,inset=ring?outer*2.4f:outer/2;
        PathHeld path(ctx,Call(ctx,[&]{return fz_new_path(ctx);}));
        Held<fz_stroke_state,DropStroke> stroke(ctx,Call(ctx,[&]{return fz_new_stroke_state(ctx);}));
        stroke.p->linewidth=width;
        Call(ctx,[&]{RoundRect(ctx,path.get(),inset,inset,w-inset,h-inset,h*.16f-inset*.5f);
            fz_stroke_path(ctx,dev.get(),path.get(),stroke.get(),fz_identity,fz_device_rgb(ctx),color,opacity,fz_default_color_params);});
    }
    const float unit=std::max(.01f,TextWidth(ctx,font.get(),text,1));
    const float size=std::max(1.0f,std::min(h*StampText,(w-h*StampPadding*2*.6f)/unit));
    const float width=unit*size;
    DrawText(ctx,dev.get(),font.get(),text,size,fz_translate((w-width)/2,h/2+size*.36f),rgb,opacity);
    Call(ctx,[&]{fz_close_device(ctx,dev.get());
        pdf_set_annot_appearance(ctx,annot,"N",nullptr,fz_identity,fz_make_rect(0,0,w,h),resources.get(),contents.get());});
}
// 按文字长度给出印章的默认尺寸（高度 42pt）。
Rect StampSize(fz_context* ctx,const std::string& text,Point at){
    auto font=SystemFont(ctx,true);
    const float h=42,w=std::max(h*1.6f,TextWidth(ctx,font.get(),text,h*StampText)+h*StampPadding*2);
    return {at.x,at.y,w,h};
}
}
std::vector<Annotation> Document::Annotations(int number){
    auto& d=*impl_;auto page=d.Load(number);std::vector<Annotation> values;
    const auto uiText=LumenTextLayer(d.ctx,page.get());
    for(pdf_annot* a=Call(d.ctx,[&]{return pdf_first_annot(d.ctx,page.get());});a;a=Call(d.ctx,[&]{return pdf_next_annot(d.ctx,a);})){
        Annotation value;
        value.id=Call(d.ctx,[&]{return pdf_to_num(d.ctx,pdf_annot_obj(d.ctx,a));});
        value.type=Detect(d.ctx,a);
        const bool boxed=value.type==Tool::Text||value.type==Tool::Image||value.type==Tool::Note||value.type==Tool::Stamp||IsBoxShape(value.type);
        value.bounds=Public(Call(d.ctx,[&]{return boxed?pdf_annot_rect(d.ctx,a):pdf_bound_annot(d.ctx,a);}));
        const char* text=Call(d.ctx,[&]{return pdf_annot_contents(d.ctx,a);});value.text=Wide(text?text:"");
        if(Call(d.ctx,[&]{return pdf_annot_has_author(d.ctx,a);})){const char* author=Call(d.ctx,[&]{return pdf_annot_author(d.ctx,a);});value.author=Wide(author?author:"");}
        value.modified=Call(d.ctx,[&]{return pdf_annot_modification_date(d.ctx,a);});
        value.opacity=Call(d.ctx,[&]{return pdf_annot_opacity(d.ctx,a);});
        value.style=DefaultAnnotationStyle(value.type);
        int n=0;float color[4]{};
        Call(d.ctx,[&]{pdf_annot_color(d.ctx,a,&n,color);});value.style.color=Rgb(n,color,value.style.color);
        if(value.type==Tool::Text){
            const char* font=nullptr;Call(d.ctx,[&]{pdf_annot_default_appearance(d.ctx,a,&font,&value.fontSize,&n,color);});
            value.textFormat.color=Rgb(n,color,0x1f1f1f);
            const char* family=Call(d.ctx,[&]{return pdf_to_text_string(d.ctx,pdf_dict_gets(d.ctx,pdf_annot_obj(d.ctx,a),"LumenFontFamily"));});
            value.lumenText=family&&*family&&std::find(uiText.begin(),uiText.end(),value.id)!=uiText.end();
            if(family&&*family)value.textFormat.family=Wide(family);
            else if(font&&std::string_view(font).find("Cour")!=std::string_view::npos)value.textFormat.family=L"Courier New";
            else if(font&&std::string_view(font).find("Ti")!=std::string_view::npos)value.textFormat.family=L"Times New Roman";
            value.textFormat.bold=Call(d.ctx,[&]{return pdf_to_int(d.ctx,pdf_dict_gets(d.ctx,pdf_annot_obj(d.ctx,a),"LumenFontBold"));})!=0;
            value.textFormat.italic=Call(d.ctx,[&]{return pdf_to_int(d.ctx,pdf_dict_gets(d.ctx,pdf_annot_obj(d.ctx,a),"LumenFontItalic"));})!=0;
            value.textFormat.underline=Call(d.ctx,[&]{return pdf_to_int(d.ctx,pdf_dict_gets(d.ctx,pdf_annot_obj(d.ctx,a),"LumenFontUnderline"));})!=0;
            value.textFormat.alignment=Call(d.ctx,[&]{return pdf_annot_quadding(d.ctx,a);});
            value.fixedTextBox=Call(d.ctx,[&]{return pdf_to_int(d.ctx,pdf_dict_gets(d.ctx,pdf_annot_obj(d.ctx,a),"LumenTextFixedBox"));})!=0;
            const int mode=Call(d.ctx,[&]{return pdf_to_int(d.ctx,pdf_dict_gets(d.ctx,pdf_annot_obj(d.ctx,a),"LumenTextSizing"));});
            value.textSizing=static_cast<TextSizing>(std::clamp(mode,0,2));
        }
        if(IsStrokedTool(value.type)){
            value.style.lineWidth=Call(d.ctx,[&]{return pdf_annot_border_width(d.ctx,a);});
            value.style.dashed=Call(d.ctx,[&]{return pdf_annot_border_style(d.ctx,a);})==PDF_BORDER_STYLE_DASHED;
        }
        if(IsBoxShape(value.type)){Call(d.ctx,[&]{pdf_annot_interior_color(d.ctx,a,&n,color);});value.style.filled=n>0;value.style.fillColor=Rgb(n,color,value.style.fillColor);}
        if(IsLineTool(value.type)){
            fz_point p{},q{};pdf_line_ending start{},end{};
            Call(d.ctx,[&]{pdf_annot_line(d.ctx,a,&p,&q);pdf_annot_line_ending_styles(d.ctx,a,&start,&end);});
            value.points={{p.x,p.y},{q.x,q.y}};value.style.startEnding=start;value.style.endEnding=end;
        }
        if(value.type==Tool::Ink){
            const int count=Call(d.ctx,[&]{return pdf_annot_ink_list_count(d.ctx,a);});
            for(int i=0;i<count;++i){
                const int size=Call(d.ctx,[&]{return pdf_annot_ink_list_stroke_count(d.ctx,a,i);});value.strokes.push_back(size);
                for(int k=0;k<size;++k){auto p=Call(d.ctx,[&]{return pdf_annot_ink_list_stroke_vertex(d.ctx,a,i,k);});value.points.push_back({p.x,p.y});}
            }
        }
        if(IsMarkupTool(value.type)){
            const int count=Call(d.ctx,[&]{return pdf_annot_quad_point_count(d.ctx,a);});
            for(int i=0;i<count;++i){auto q=Call(d.ctx,[&]{return pdf_annot_quad_point(d.ctx,a,i);});value.quads.push_back({{q.ul.x,q.ul.y},{q.ur.x,q.ur.y},{q.ll.x,q.ll.y},{q.lr.x,q.lr.y}});}
            value.areaHighlight=Call(d.ctx,[&]{return pdf_to_int(d.ctx,pdf_dict_gets(d.ctx,pdf_annot_obj(d.ctx,a),"LumenAreaHighlight"));})!=0;
        }
        value.rotation=Call(d.ctx,[&]{return pdf_dict_get_int(d.ctx,pdf_annot_obj(d.ctx,a),PDF_NAME(Rotate));});
        const int flags=Call(d.ctx,[&]{return pdf_dict_get_int(d.ctx,pdf_annot_obj(d.ctx,a),PDF_NAME(F));});
        value.readOnly=(flags&(PDF_ANNOT_IS_READ_ONLY|PDF_ANNOT_IS_LOCKED))!=0;
        if(!(flags&(PDF_ANNOT_IS_HIDDEN|PDF_ANNOT_IS_NO_VIEW|PDF_ANNOT_IS_INVISIBLE)))values.push_back(std::move(value));
    }return values;
}
namespace {
// 单词边界。Windows 10 1903 起系统自带 icu.dll（中文按词典分词）；缺失时退回字符类别：
// 连续汉字 / 假名、连续字母数字（含 ' 与 -）各成一词，其余符号单独成词。text 为一行文字，at 为字符下标。
struct IcuWords {
    using Open=void*(*)(int,const char*,const char16_t*,int32_t,int*);
    using Close=void(*)(void*);
    using Step=int32_t(*)(void*,int32_t);
    Open open{};Close close{};Step following{},preceding{};
    static const IcuWords& Get(){
        static const IcuWords icu=[]{
            IcuWords v;HMODULE m=LoadLibraryExW(L"icu.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!m)return v;
            v.open=reinterpret_cast<Open>(reinterpret_cast<void*>(GetProcAddress(m,"ubrk_open")));
            v.close=reinterpret_cast<Close>(reinterpret_cast<void*>(GetProcAddress(m,"ubrk_close")));
            v.following=reinterpret_cast<Step>(reinterpret_cast<void*>(GetProcAddress(m,"ubrk_following")));
            v.preceding=reinterpret_cast<Step>(reinterpret_cast<void*>(GetProcAddress(m,"ubrk_preceding")));
            if(!v.open||!v.close||!v.following||!v.preceding)v={};
            return v;
        }();
        return icu;
    }
};
bool Ideograph(int c){return (c>=0x3040&&c<=0x30ff)||(c>=0x3400&&c<=0x4dbf)||(c>=0x4e00&&c<=0x9fff)||(c>=0xf900&&c<=0xfaff)||(c>=0x20000&&c<=0x3134f);}
bool WordChar(int c){return (c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||(c>=0xc0&&c<0x2000&&c!=0xd7&&c!=0xf7)||(c>=0xac00&&c<=0xd7a3)||(c>=0xff10&&c<=0xff5a&&(c<=0xff19||c>=0xff21));}
std::pair<size_t,size_t> WordBounds(const std::vector<int>& text,size_t at){
    if(at>=text.size())return {at,at};
    const auto& icu=IcuWords::Get();
    if(icu.open){
        std::u16string units;std::vector<size_t> first;  // first[i]：第 i 个字符的 UTF-16 起点
        for(int c:text){first.push_back(units.size());if(c>0xffff){c-=0x10000;units+=static_cast<char16_t>(0xd800+(c>>10));units+=static_cast<char16_t>(0xdc00+(c&0x3ff));}else units+=static_cast<char16_t>(c);}
        int status=0;void* it=icu.open(1/*UBRK_WORD*/,"",units.data(),static_cast<int32_t>(units.size()),&status);
        if(it&&status<=0){
            const int32_t pos=static_cast<int32_t>(first[at]);
            const int32_t a=icu.preceding(it,pos+1),b=icu.following(it,pos);icu.close(it);
            if(a>=0&&b>a){
                size_t i0=at,i1=at+1;
                while(i0>0&&first[i0]>static_cast<size_t>(a))--i0;
                while(i1<text.size()&&first[i1]<static_cast<size_t>(b))++i1;
                if(i0<=at&&at<i1)return {i0,i1};
            }
        }else if(it)icu.close(it);
    }
    auto same=[&](int c){return Ideograph(text[at])?Ideograph(c):WordChar(text[at])?(WordChar(c)||c=='\''||c==0x2019||c=='-'):false;};
    if(!Ideograph(text[at])&&!WordChar(text[at]))return {at,at+1};
    size_t i0=at,i1=at+1;
    while(i0>0&&same(text[i0-1]))--i0;
    while(i1<text.size()&&same(text[i1]))++i1;
    // 词尾的撇号 / 连字符不算在词内。
    while(i1>at+1&&(text[i1-1]=='\''||text[i1-1]==0x2019||text[i1-1]=='-'))--i1;
    return {i0,i1};
}
bool NearChar(const fz_quad& q,Point p,float pad){const auto r=fz_expand_rect(fz_rect_from_quad(q),pad);return p.x>=r.x0&&p.x<=r.x1&&p.y>=r.y0&&p.y<=r.y1;}
}
bool Document::ResolveSelection(int number,Point& start,Point& end,bool fromStart,bool toEnd){
    auto& d=*impl_;
    if(!std::isfinite(start.x)||!std::isfinite(start.y)||!std::isfinite(end.x)||!std::isfinite(end.y))return false;
    if(fromStart||toEnd)return d.EdgePoints(number,fromStart,toEnd,start,end);
    return d.SelectionNearText(number,start,end);
}
std::optional<std::pair<Point,Point>> Document::SnapSelection(int number,Point p,SnapUnit unit){
    auto& d=*impl_;
    if(!std::isfinite(p.x)||!std::isfinite(p.y))return std::nullopt;
    auto* text=d.SelectionPage(number);
    for(auto* block=text->first_block;block;block=block->next){
        if(block->type!=FZ_STEXT_BLOCK_TEXT)continue;
        for(auto* line=block->u.t.first_line;line;line=line->next){
            const auto box=fz_expand_rect(line->bbox,1.5f);
            if(p.x<box.x0||p.x>box.x1||p.y<box.y0||p.y>box.y1)continue;
            std::vector<fz_stext_char*> chars;for(auto* ch=line->first_char;ch;ch=ch->next)chars.push_back(ch);
            if(chars.empty())continue;
            // 命中的字符：先找包含该点的，否则取同一行里水平距离最近的。
            size_t at=chars.size();float best=1e30f;
            for(size_t i=0;i<chars.size();++i){
                if(NearChar(chars[i]->quad,p,.5f)){at=i;break;}
                const auto r=fz_rect_from_quad(chars[i]->quad);const float dx=p.x<r.x0?r.x0-p.x:p.x>r.x1?p.x-r.x1:0;
                if(dx<best){best=dx;at=i;}
            }
            if(at>=chars.size()||best>6)continue;
            if(unit==SnapUnit::Paragraph){
                fz_stext_char* first=nullptr;fz_stext_char* last=nullptr;
                for(auto* l=block->u.t.first_line;l;l=l->next)for(auto* ch=l->first_char;ch;ch=ch->next){if(!first)first=ch;last=ch;}
                if(!first)return std::nullopt;
                return std::pair{Impl::CharPoint(first->quad,.2f),Impl::CharPoint(last->quad,.8f)};
            }
            std::vector<int> codes;for(auto* ch:chars)codes.push_back(ch->c);
            if(codes[at]==' '||codes[at]=='\t'||codes[at]==0x3000||codes[at]==0xa0)return std::nullopt;
            const auto [i0,i1]=WordBounds(codes,at);
            if(i1<=i0)return std::nullopt;
            return std::pair{Impl::CharPoint(chars[i0]->quad,.2f),Impl::CharPoint(chars[i1-1]->quad,.8f)};
        }
    }
    return std::nullopt;
}
std::vector<Quad> Document::HighlightQuads(int number,Point start,Point end,bool fromStart,bool toEnd){
    auto& d=*impl_;
    if(!ResolveSelection(number,start,end,fromStart,toEnd))return {};
    std::vector<fz_quad> native(4097);
    const int count=Call(d.ctx,[&]{return fz_highlight_selection(d.ctx,d.selectionText,{start.x,start.y},{end.x,end.y},native.data(),static_cast<int>(native.size()));});
    if(count>=static_cast<int>(native.size()))throw std::runtime_error("Select a smaller text range");
    std::vector<Quad> result;result.reserve(count);
    for(int i=0;i<count;++i){const auto& q=native[i];result.push_back({{q.ul.x,q.ul.y},{q.ur.x,q.ur.y},{q.ll.x,q.ll.y},{q.lr.x,q.lr.y}});}
    return result;
}
std::wstring Document::Text(int number,std::optional<Rect> selection){
    auto& d=*impl_;auto page=d.Load(number);
    const auto bounds=Call(d.ctx,[&]{return pdf_bound_page(d.ctx,page.get(),FZ_CROP_BOX);});
    Stext st(d.ctx,Call(d.ctx,[&]{return fz_new_stext_page(d.ctx,bounds);}));
    Device dev(d.ctx,Call(d.ctx,[&]{return fz_new_stext_device(d.ctx,st.get(),nullptr);}));
    Call(d.ctx,[&]{pdf_run_page(d.ctx,page.get(),dev.get(),fz_identity,nullptr);fz_close_device(d.ctx,dev.get());});
    char* text=Call(d.ctx,[&]{return fz_copy_rectangle(d.ctx,st.get(),selection?Native(*selection):bounds,1);});
    std::string copy;
    try{copy=text?text:"";}catch(...){fz_free(d.ctx,text);throw;}
    fz_free(d.ctx,text);return Wide(copy);
}
std::vector<ReadingSentence> Document::ReadingSentences(int number){
    auto& d=*impl_;auto* text=d.SelectionPage(number);
    std::vector<ReadingSentence> out;ReadingSentence current;std::optional<fz_rect> box;
    auto flushBox=[&]{if(box){current.boxes.push_back({box->x0,box->y0,box->x1-box->x0,box->y1-box->y0});box.reset();}};
    auto finish=[&]{
        flushBox();auto& t=current.text;
        while(!t.empty()&&iswspace(t.back()))t.pop_back();
        size_t lead=0;while(lead<t.size()&&iswspace(t[lead]))++lead;t.erase(0,lead);
        bool speakable=false;for(wchar_t ch:t)if(iswalnum(ch)||(ch>=0x2E80&&ch!=0xFFFD&&(ch<0xE000||ch>0xF8FF))){speakable=true;break;}
        if(speakable&&!current.boxes.empty())out.push_back(std::move(current));
        current={};
    };
    auto append=[&](int c){
        if(c>0xFFFF){c-=0x10000;current.text+=static_cast<wchar_t>(0xD800+(c>>10));current.text+=static_cast<wchar_t>(0xDC00+(c&0x3FF));}
        else current.text+=static_cast<wchar_t>(c);
    };
    auto cjk=[](wchar_t c){return (c>=0x2E80&&c<=0x9FFF)||(c>=0xF900&&c<=0xFAFF)||(c>=0xFF00&&c<=0xFFEF)||(c>=0x3000&&c<=0x303F);};
    auto closing=[](int c){return c==0x201D||c==0x2019||c=='"'||c=='\''||c==0x300D||c==0x300F||c==')'||c==0xFF09||c==0x300B;};
    // 有的 PDF 每行都是独立的块，所以段落不按块判断：新块且（行距明显变大 / 回到上方另起一栏 / 首行缩进）才算分段，
    // 其余换行都当作句子在下一行继续（英文补空格、行尾连字符合并）。
    std::optional<fz_rect> previous;
    for(auto* block=text->first_block;block;block=block->next){
        if(block->type!=FZ_STEXT_BLOCK_TEXT)continue;
        bool newBlock=true;
        for(auto* line=block->u.t.first_line;line;line=line->next){
            const fz_rect bounds=line->bbox;
            if(previous){
                const float height=std::max(1.0f,previous->y1-previous->y0);
                const bool paragraph=newBlock&&(bounds.y0-previous->y1>height*.9f||bounds.y1<previous->y0||bounds.x0>previous->x0+height*1.5f);
                if(paragraph)finish();
                else if(!current.text.empty()){
                    const wchar_t last=current.text.back();
                    if(last=='-'&&current.text.size()>1&&iswalpha(current.text[current.text.size()-2]))current.text.pop_back();   // 行尾连字符
                    else if(!cjk(last)&&last!=' ')current.text+=L' ';
                }
            }
            previous=bounds;newBlock=false;
            for(auto* ch=line->first_char;ch;ch=ch->next){
                int c=ch->c;
                if(c<0x20||c==0xFFFD)c=' ';   // 控制字符 / 无法识别的字形
                if((c==' '||c=='\t')&&current.text.empty())continue;
                append(c);
                if(c!=' '&&c!='\t'){const fz_rect r=fz_rect_from_quad(ch->quad);box=box?fz_union_rect(*box,r):r;}
                const bool strong=c==0x3002||c==0xFF01||c==0xFF1F||c==0xFF1B||c==0x2026||c=='!'||c=='?'||c==';';
                const bool period=(c=='.'||c==0xFF0E)&&(!ch->next||ch->next->c==' ');
                if(strong||period){
                    while(ch->next&&closing(ch->next->c)){ch=ch->next;append(ch->c);box=fz_union_rect(*box,fz_rect_from_quad(ch->quad));}
                    finish();
                }else if(current.text.size()>=240&&(c==0xFF0C||c==','||c==' '||c==0x3001))finish();
            }
            flushBox();
        }
    }
    finish();
    return out;
}
std::wstring Document::SelectionText(int number,Point start,Point end,bool fromStart,bool toEnd){
    auto& d=*impl_;
    if(!ResolveSelection(number,start,end,fromStart,toEnd))return {};
    char* text=Call(d.ctx,[&]{return fz_copy_selection(d.ctx,d.selectionText,{start.x,start.y},{end.x,end.y},1);});
    std::string copy;
    try{copy=text?text:"";}catch(...){fz_free(d.ctx,text);throw;}
    fz_free(d.ctx,text);return Wide(copy);
}
namespace {
FieldType WidgetKind(enum pdf_widget_type t){
    switch(t){
    case PDF_WIDGET_TYPE_TEXT:return FieldType::Text;case PDF_WIDGET_TYPE_CHECKBOX:return FieldType::CheckBox;
    case PDF_WIDGET_TYPE_RADIOBUTTON:return FieldType::Radio;case PDF_WIDGET_TYPE_COMBOBOX:return FieldType::ComboBox;
    case PDF_WIDGET_TYPE_LISTBOX:return FieldType::ListBox;case PDF_WIDGET_TYPE_BUTTON:return FieldType::Button;
    case PDF_WIDGET_TYPE_SIGNATURE:return FieldType::Signature;default:return FieldType::Unknown;
    }
}
pdf_annot* FindWidget(fz_context* ctx,pdf_page* page,int id){
    pdf_annot* w=Call(ctx,[&]{return pdf_first_widget(ctx,page);});
    while(w){
        if(Call(ctx,[&]{return pdf_to_num(ctx,pdf_annot_obj(ctx,w));})==id)return w;
        w=Call(ctx,[&]{return pdf_next_widget(ctx,w);});
    }
    throw std::runtime_error("The form field is no longer available");
}
FormField DescribeWidget(fz_context* ctx,pdf_annot* w,int number){
    FormField f;f.page=number;
    pdf_obj* obj=Call(ctx,[&]{return pdf_annot_obj(ctx,w);});
    f.id=Call(ctx,[&]{return pdf_to_num(ctx,obj);});
    f.type=WidgetKind(Call(ctx,[&]{return pdf_widget_type(ctx,w);}));
    f.bounds=Public(Call(ctx,[&]{return pdf_bound_widget(ctx,w);}));
    const int flags=Call(ctx,[&]{return pdf_field_flags(ctx,obj);});
    {
        char* name=Call(ctx,[&]{return pdf_load_field_name(ctx,obj);});
        try{f.name=Wide(name?name:"");}catch(...){fz_free(ctx,name);throw;}
        fz_free(ctx,name);
    }
    const char* label=Call(ctx,[&]{return pdf_field_label(ctx,obj);});f.label=Wide(label?label:"");
    if(f.label.empty())f.label=f.name;
    const char* value=Call(ctx,[&]{return pdf_field_value(ctx,obj);});f.value=Wide(value?value:"");
    f.readOnly=(flags&PDF_FIELD_IS_READ_ONLY)!=0||Call(ctx,[&]{return pdf_widget_is_readonly(ctx,w);})!=0;
    f.required=(flags&2)!=0;
    if(f.type==FieldType::Text){
        f.multiline=(flags&PDF_TX_FIELD_IS_MULTILINE)!=0;f.password=(flags&PDF_TX_FIELD_IS_PASSWORD)!=0;
        f.maxLength=std::max(0,Call(ctx,[&]{return pdf_text_widget_max_len(ctx,w);}));
    }
    if(f.type==FieldType::ComboBox||f.type==FieldType::ListBox){
        f.editable=(flags&PDF_CH_FIELD_IS_EDIT)!=0;
        f.multiSelect=Call(ctx,[&]{return pdf_choice_widget_is_multiselect(ctx,w);})!=0;
        const int n=std::clamp(Call(ctx,[&]{return pdf_choice_widget_options(ctx,w,0,nullptr);}),0,4096);
        std::vector<const char*> shown(static_cast<size_t>(n)),exported(static_cast<size_t>(n));
        if(n){Call(ctx,[&]{pdf_choice_widget_options(ctx,w,0,shown.data());pdf_choice_widget_options(ctx,w,1,exported.data());});}
        for(int i=0;i<n;++i){f.options.push_back(Wide(shown[i]?shown[i]:""));f.exports.push_back(Wide(exported[i]?exported[i]:(shown[i]?shown[i]:"")));}
    }
    if(f.type==FieldType::CheckBox||f.type==FieldType::Radio){
        Call(ctx,[&]{
            pdf_obj* as=pdf_dict_get(ctx,obj,PDF_NAME(AS));
            f.checked=as&&pdf_is_name(ctx,as)&&!pdf_name_eq(ctx,as,PDF_NAME(Off));
        });
        pdf_obj* on=Call(ctx,[&]{return pdf_button_field_on_state(ctx,obj);});
        if(on)f.onState=Wide(Call(ctx,[&]{return pdf_to_name(ctx,on);}));
    }
    if(f.type==FieldType::Signature)f.signedField=Call(ctx,[&]{return pdf_widget_is_signed(ctx,w);})!=0;
    return f;
}
}
std::vector<FormField> Document::FormFields(int number){
    auto& d=*impl_;auto page=d.Load(number);std::vector<FormField> out;
    for(pdf_annot* w=Call(d.ctx,[&]{return pdf_first_widget(d.ctx,page.get());});w;w=Call(d.ctx,[&]{return pdf_next_widget(d.ctx,w);})){
        auto f=DescribeWidget(d.ctx,w,number);
        if(f.type==FieldType::Unknown||!(f.bounds.w>0)||!(f.bounds.h>0))continue;
        out.push_back(std::move(f));
    }
    return out;
}
void Document::SetFieldValue(int number,int id,std::wstring_view value){
    auto& d=*impl_;auto page=d.Load(number);auto* w=FindWidget(d.ctx,page.get(),id);
    const auto field=DescribeWidget(d.ctx,w,number);
    if(field.readOnly)throw std::runtime_error("This form field is read-only");
    std::wstring text(value);
    if(field.type==FieldType::Text){
        if(!field.multiline)text.erase(std::remove_if(text.begin(),text.end(),[](wchar_t c){return c==L'\r'||c==L'\n';}),text.end());
        if(field.maxLength>0&&text.size()>static_cast<size_t>(field.maxLength))throw std::runtime_error("The text exceeds the field's maximum length");
    }else if(field.type==FieldType::ComboBox||field.type==FieldType::ListBox){
        // 显示文字换成导出值；非可编辑下拉只接受已有选项。
        for(size_t i=0;i<field.options.size();++i)if(text==field.options[i]){text=field.exports[i];break;}
        const bool known=text.empty()||std::find(field.exports.begin(),field.exports.end(),text)!=field.exports.end();
        if(!known&&!(field.type==FieldType::ComboBox&&field.editable))throw std::runtime_error("The value is not one of the field's options");
    }else throw std::runtime_error("This form field does not take text");
    const auto utf8=Utf8(text);
    d.Edit("Fill form field",[&]{
        const int accepted=Call(d.ctx,[&]{
            return field.type==FieldType::Text?pdf_set_text_field_value(d.ctx,w,utf8.c_str()):pdf_set_choice_field_value(d.ctx,w,utf8.c_str());
        });
        if(!accepted)throw std::runtime_error("The form rejected this value");
        Call(d.ctx,[&]{pdf_update_widget(d.ctx,w);});
    });
}
void Document::ToggleField(int number,int id){
    auto& d=*impl_;auto page=d.Load(number);auto* w=FindWidget(d.ctx,page.get(),id);
    const auto field=DescribeWidget(d.ctx,w,number);
    if(field.readOnly)throw std::runtime_error("This form field is read-only");
    if(field.type!=FieldType::CheckBox&&field.type!=FieldType::Radio)throw std::runtime_error("This form field cannot be toggled");
    d.Edit("Toggle form field",[&]{
        Call(d.ctx,[&]{pdf_toggle_widget(d.ctx,w);});
        // 同组单选按钮可能在本页其它控件上：逐个刷新外观。
        for(pdf_annot* x=Call(d.ctx,[&]{return pdf_first_widget(d.ctx,page.get());});x;x=Call(d.ctx,[&]{return pdf_next_widget(d.ctx,x);}))
            Call(d.ctx,[&]{pdf_update_widget(d.ctx,x);});
    });
}
void Document::ResetForm(){
    auto& d=*impl_;d.Require();
    d.Edit("Reset form",[&]{
        Call(d.ctx,[&]{pdf_reset_form(d.ctx,d.doc,nullptr,1);});
        for(int i=0;i<d.Count();++i){
            auto page=d.Load(i);
            for(pdf_annot* x=Call(d.ctx,[&]{return pdf_first_widget(d.ctx,page.get());});x;x=Call(d.ctx,[&]{return pdf_next_widget(d.ctx,x);}))
                Call(d.ctx,[&]{pdf_update_widget(d.ctx,x);});
        }
    });
}
std::optional<FormField> Document::NextFillableField(int number,int id,bool textOnly){
    auto& d=*impl_;const int count=d.Count();if(count<=0)return std::nullopt;
    number=std::clamp(number,0,count-1);
    auto eligible=[textOnly](const FormField& f){
        return f.Fillable()&&(f.type==FieldType::Text||(!textOnly&&(f.type==FieldType::ComboBox||f.type==FieldType::ListBox)));
    };
    const auto here=FormFields(number);
    const bool found=std::any_of(here.begin(),here.end(),[id](const auto& f){return f.id==id;});
    // 1. 本页当前字段之后（当前字段不在本页时：本页第一个）
    bool after=!found;
    for(const auto& f:here){if(after&&eligible(f)&&f.id!=id)return f;if(f.id==id)after=true;}
    // 2. 之后各页（到末尾回到开头）
    for(int step=1;step<count;++step)for(const auto& f:FormFields((number+step)%count))if(eligible(f))return f;
    // 3. 回到本页当前字段之前
    if(found)for(const auto& f:here){if(f.id==id)break;if(eligible(f))return f;}
    return std::nullopt;
}
std::vector<Link> Document::Links(int number){
    auto& d=*impl_;auto page=d.Load(number);
    LinkList links(d.ctx,Call(d.ctx,[&]{return pdf_load_links(d.ctx,page.get());}));
    std::vector<Link> result;
    for(fz_link* l=links.get();l;l=l->next){
        if(!l->uri||!*l->uri||fz_is_empty_rect(l->rect))continue;
        Link item;item.bounds=Public(l->rect);
        const char* uri=l->uri;
        if(Call(d.ctx,[&]{return fz_is_external_link(d.ctx,uri);})){item.uri=Wide(uri);result.push_back(std::move(item));continue;}
        // Unresolvable internal destinations are ignored instead of failing the page.
        int target=-1;float y=NAN;
        try{
            const auto dest=Call(d.ctx,[&]{return pdf_resolve_link_dest(d.ctx,d.doc,uri);});
            target=Call(d.ctx,[&]{return fz_page_number_from_location(d.ctx,reinterpret_cast<fz_document*>(d.doc),dest.loc);});
            if(dest.type==FZ_LINK_DEST_XYZ||dest.type==FZ_LINK_DEST_FIT_H||dest.type==FZ_LINK_DEST_FIT_BH||dest.type==FZ_LINK_DEST_FIT_R)y=dest.y;
        }catch(const std::runtime_error&){target=-1;}
        if(target<0||target>=d.Count())continue;
        item.page=target;item.targetY=std::isfinite(y)?y:NAN;result.push_back(std::move(item));
    }
    return result;
}
namespace {
bool SearchSpace(int c){return c==9||c==10||c==13||c==32||c==0xa0||c==0x3000;}
bool SearchWord(int c){
    if(c==L'_')return true;
    if(c>0xffff)return true;
    wchar_t ch=static_cast<wchar_t>(c);WORD type=0;
    return GetStringTypeW(CT_CTYPE1,&ch,1,&type)&&(type&(C1_ALPHA|C1_DIGIT));
}
std::vector<int> SearchRunes(std::wstring_view text){
    const auto bytes=Utf8(text);std::vector<int> out;
    for(const char* p=bytes.c_str();*p;){int c=0;p+=fz_chartorune(&c,p);if(SearchSpace(c)){if(!out.empty()&&out.back()!=32)out.push_back(32);}else out.push_back(c);}
    if(!out.empty()&&out.back()==32)out.pop_back();return out;
}
std::wstring SearchContext(const std::vector<int>& text,size_t at,size_t length){
    std::string bytes;const auto first=at>35?at-35:0,last=std::min(text.size(),at+length+55);
    for(size_t i=first;i<last;++i){char b[8];const int n=fz_runetochar(b,text[i]);bytes.append(b,n);}
    return (first?L"…":L"")+Wide(bytes)+(last<text.size()?L"…":L"");
}
}
std::vector<SearchHit> Document::SearchPage(int number,std::wstring_view query,const SearchOptions& options,const Cancel& cancel){
    auto& d=*impl_;CheckCancel(cancel);const auto needle=SearchRunes(query);std::vector<SearchHit> result;if(needle.empty())return result;
    auto find=[&](const std::vector<int>& text,const std::vector<Rect>& boxes,int annotation,std::wstring_view prefix){
        for(size_t i=0;i+needle.size()<=text.size();++i){
            if((i&255)==0)CheckCancel(cancel);
            bool equal=true;for(size_t j=0;j<needle.size();++j)if((options.matchCase?text[i+j]:fz_toupper(text[i+j]))!=(options.matchCase?needle[j]:fz_toupper(needle[j]))){equal=false;break;}
            if(!equal||(options.wholeWord&&((i&&SearchWord(text[i-1]))||(i+needle.size()<text.size()&&SearchWord(text[i+needle.size()])))))continue;
            Rect box=boxes.empty()?Rect{}:boxes[i];
            for(size_t j=1;!boxes.empty()&&j<needle.size();++j){auto r=boxes[i+j];const float x=std::min(box.x,r.x),y=std::min(box.y,r.y);box={x,y,std::max(box.x+box.w,r.x+r.w)-x,std::max(box.y+box.h,r.y+r.h)-y};}
            result.push_back({number,box,std::wstring(prefix)+SearchContext(text,i,needle.size()),annotation});i+=needle.size()-1;
        }
    };
    auto* st=d.SelectionPage(number);std::vector<int> text;std::vector<Rect> boxes;
    auto append=[&](int c,Rect r){if(SearchSpace(c)){if(text.empty()||text.back()==32)return;c=32;}text.push_back(c);boxes.push_back(r);};
    for(auto* block=st->first_block;block;block=block->next){
        CheckCancel(cancel);if(block->type!=FZ_STEXT_BLOCK_TEXT)continue;
        for(auto* line=block->u.t.first_line;line;line=line->next){
            for(auto* ch=line->first_char;ch;ch=ch->next)append(ch->c,Public(fz_rect_from_quad(ch->quad)));
            if(!boxes.empty())append(32,boxes.back());
        }
    }
    find(text,boxes,-1,L"");
    if(options.annotations)for(const auto& a:Annotations(number)){auto chars=SearchRunes(a.text);find(chars,std::vector<Rect>(chars.size(),a.bounds),a.id,L"批注：");}
    if(options.bookmarks){
        auto outline=Info().outline;
        for(const auto& item:outline)if(item.page==number){auto chars=SearchRunes(item.title);find(chars,{},-1,L"书签：");}
    }
    return result;
}
std::vector<SearchHit> Document::Search(std::wstring_view query,const Cancel& cancel){
    std::vector<SearchHit> result;
    for(int n=0,total=impl_->Count();n<total;++n){SearchOptions options;options.annotations=true;auto hits=SearchPage(n,query,options,cancel);result.insert(result.end(),std::make_move_iterator(hits.begin()),std::make_move_iterator(hits.end()));}
    return result;
}
void Document::AddAnnotation(int number,Tool tool,Rect rect,std::wstring_view text,const fs::path& image,const std::vector<Point>& ink,float fontSize,float opacity,std::optional<AnnotationStyle> appearance,bool textHighlight,std::optional<TextFormat> textFormat,bool fixedTextBox,TextSizing textSizing){
    const auto style=appearance.value_or(DefaultAnnotationStyle(tool));
    ValidateGeometry(rect,opacity,style,ink);
    if(!std::isfinite(fontSize)||fontSize<6||fontSize>96)throw std::runtime_error("Invalid font size");
    auto& d=*impl_;const auto utf8=Utf8(text);const auto imagePath=PathString(image);
    auto page=d.Load(number);
    Image loaded(d.ctx,tool==Tool::Image?Call(d.ctx,[&]{return fz_new_image_from_file(d.ctx,imagePath.c_str());}):nullptr);
    if(loaded.get()){const float factor=std::min(rect.w/loaded.p->w,rect.h/loaded.p->h);rect.w=loaded.p->w*factor;rect.h=loaded.p->h*factor;}
    const auto smooth=tool==Tool::Ink?SmoothInk(ink):ink;
    std::vector<fz_point> points;for(const auto& p:smooth)points.push_back(fz_make_point(p.x,p.y));
    if(tool==Tool::Ink&&points.empty())throw std::runtime_error("Draw a stroke first");
    if(IsLineTool(tool)&&points.size()>=2&&PointDistance(smooth.front(),smooth.back())<.5f)throw std::runtime_error("Draw a longer arrow");
    std::vector<fz_quad> quads;
    if(tool==Tool::Stamp){
        if(text.find_first_not_of(L" \t\r\n")==std::wstring_view::npos)throw std::runtime_error("Enter the stamp text");
        if(text.size()>40)throw std::runtime_error("Stamp text is too long");
        const auto bounds=Public(Call(d.ctx,[&]{return pdf_bound_page(d.ctx,page.get(),FZ_CROP_BOX);}));
        if(rect.w<30||rect.h<16)rect=StampSize(d.ctx,utf8,{rect.x,rect.y});
        rect.w=std::min(rect.w,bounds.w);rect.h=std::min(rect.h,bounds.h);
        rect.x=std::clamp(rect.x,bounds.x,bounds.x+bounds.w-rect.w);rect.y=std::clamp(rect.y,bounds.y,bounds.y+bounds.h-rect.h);
    }
    if(IsMarkupTool(tool)){
        if(textHighlight){
            if(ink.size()<2)throw std::runtime_error("Select text to highlight");
            for(const auto& q:HighlightQuads(number,ink.front(),ink.back()))quads.push_back({{q.ul.x,q.ul.y},{q.ur.x,q.ur.y},{q.ll.x,q.ll.y},{q.lr.x,q.lr.y}});
            if(quads.empty())throw std::runtime_error("No text selected. Hold Ctrl and drag for an area highlight.");
        }else quads.push_back(fz_quad_from_rect(Native(rect)));
    }
    if(tool==Tool::Note){
        const auto bounds=Public(Call(d.ctx,[&]{return pdf_bound_page(d.ctx,page.get(),FZ_CROP_BOX);}));
        rect.w=rect.h=24;rect.x=std::clamp(rect.x,bounds.x,bounds.x+std::max(0.0f,bounds.w-24));rect.y=std::clamp(rect.y,bounds.y,bounds.y+std::max(0.0f,bounds.h-24));
    }
    d.Edit("Add annotation",[&]{
        Held<pdf_annot,pdf_drop_annot> annot(d.ctx,Call(d.ctx,[&]{return pdf_create_annot(d.ctx,page.get(),Type(tool));}));
        const float black[3]{0.12f,0.12f,0.12f};
        Call(d.ctx,[&]{
            pdf_set_annot_flags(d.ctx,annot.get(),PDF_ANNOT_IS_PRINT);
            pdf_set_annot_opacity(d.ctx,annot.get(),opacity);
            if(tool==Tool::Text){pdf_set_annot_color(d.ctx,annot.get(),0,black);pdf_set_annot_border_width(d.ctx,annot.get(),0);}
            pdf_set_annot_contents(d.ctx,annot.get(),utf8.c_str());
        });
        SetStyle(d.ctx,annot.get(),tool,style);
        if(IsLineTool(tool)){
            Call(d.ctx,[&]{pdf_set_annot_line(d.ctx,annot.get(),points.size()>=2?points.front():fz_make_point(rect.x,rect.y+rect.h),points.size()>=2?points.back():fz_make_point(rect.x+rect.w,rect.y));});
        }else if(tool==Tool::Ink){
            const int count=static_cast<int>(points.size());
            Call(d.ctx,[&]{pdf_set_annot_ink_list(d.ctx,annot.get(),1,&count,points.data());});
        }else if(IsMarkupTool(tool)){
            Call(d.ctx,[&]{pdf_set_annot_quad_points(d.ctx,annot.get(),static_cast<int>(quads.size()),quads.data());pdf_dict_puts_drop(d.ctx,pdf_annot_obj(d.ctx,annot.get()),"LumenAreaHighlight",pdf_new_int(d.ctx,!textHighlight));});
        }else{
            Call(d.ctx,[&]{pdf_set_annot_rect(d.ctx,annot.get(),Native(rect));});
            if(tool==Tool::Text){
                SetTextAppearance(d.ctx,annot.get(),textFormat.value_or(TextFormat{}),fontSize,text,fixedTextBox,textSizing);
            }else if(tool==Tool::Image){
                Call(d.ctx,[&]{pdf_set_annot_stamp_image(d.ctx,annot.get(),loaded.get());pdf_set_annot_rect(d.ctx,annot.get(),Native(rect));});
            }else if(tool==Tool::Note)Call(d.ctx,[&]{pdf_set_annot_icon_name(d.ctx,annot.get(),"Comment");});
            else if(tool==Tool::Stamp)Call(d.ctx,[&]{
                pdf_set_annot_icon_name(d.ctx,annot.get(),"LumenStamp");
                pdf_dict_puts_drop(d.ctx,pdf_annot_obj(d.ctx,annot.get()),"LumenStamp",pdf_new_text_string(d.ctx,utf8.c_str()));
            });
        }
        Call(d.ctx,[&]{pdf_update_annot(d.ctx,annot.get());});
        if(tool==Tool::Stamp)SetStampAppearance(d.ctx,d.doc,annot.get(),rect,utf8,style.color,opacity);
        if(tool==Tool::Text){
            Call(d.ctx,[&]{pdf_dict_puts_drop(d.ctx,pdf_annot_obj(d.ctx,annot.get()),"LumenTextWidth",pdf_new_real(d.ctx,rect.w));});
            if(!fixedTextBox)FitTextAnnotation(d.ctx,annot.get(),rect.w,fontSize);
        }
    });
}
void Document::AddInkStrokes(int number,const std::vector<std::vector<Point>>& strokes,const AnnotationStyle& style,float opacity){
    std::vector<fz_point> points;std::vector<int> counts;std::vector<Point> flat;
    float x0=INFINITY,y0=INFINITY,x1=-INFINITY,y1=-INFINITY;
    for(const auto& s:strokes){
        if(s.empty())continue;
        counts.push_back(static_cast<int>(s.size()));
        for(auto p:s){flat.push_back(p);points.push_back(fz_make_point(p.x,p.y));x0=std::min(x0,p.x);y0=std::min(y0,p.y);x1=std::max(x1,p.x);y1=std::max(y1,p.y);}
    }
    if(counts.empty())throw std::runtime_error("Draw a stroke first");
    ValidateGeometry({x0,y0,std::max(1.0f,x1-x0),std::max(1.0f,y1-y0)},opacity,style,flat);
    auto& d=*impl_;auto page=d.Load(number);
    d.Edit("Add annotation",[&]{
        Held<pdf_annot,pdf_drop_annot> annot(d.ctx,Call(d.ctx,[&]{return pdf_create_annot(d.ctx,page.get(),PDF_ANNOT_INK);}));
        Call(d.ctx,[&]{pdf_set_annot_flags(d.ctx,annot.get(),PDF_ANNOT_IS_PRINT);pdf_set_annot_opacity(d.ctx,annot.get(),opacity);});
        SetStyle(d.ctx,annot.get(),Tool::Ink,style);
        Call(d.ctx,[&]{pdf_set_annot_ink_list(d.ctx,annot.get(),static_cast<int>(counts.size()),counts.data(),points.data());pdf_update_annot(d.ctx,annot.get());});
    });
}
void Document::AddTextAnnotation(int page,const Annotation& a){
    AddAnnotation(page,Tool::Text,a.bounds,a.text,{},{},a.fontSize,a.opacity,{},false,a.textFormat,a.fixedTextBox,a.textSizing);
}
void Document::UpdateAnnotation(int number,const Annotation& value){
    ValidateGeometry(value.bounds,value.opacity,value.style,value.points);
    auto& d=*impl_;auto page=d.Load(number);auto* annot=d.Find(page.get(),value.id);const auto text=Utf8(value.text);
    const int flags=Call(d.ctx,[&]{return pdf_dict_get_int(d.ctx,pdf_annot_obj(d.ctx,annot),PDF_NAME(F));});
    if(flags&(PDF_ANNOT_IS_READ_ONLY|PDF_ANNOT_IS_LOCKED))throw std::runtime_error("This annotation is locked");
    const auto type=Call(d.ctx,[&]{return pdf_annot_type(d.ctx,annot);});
    const bool markup=type==PDF_ANNOT_HIGHLIGHT||type==PDF_ANNOT_UNDERLINE||type==PDF_ANNOT_STRIKE_OUT;
    const bool geometric=type==PDF_ANNOT_LINE||type==PDF_ANNOT_INK||markup;
    const Tool tool=Detect(d.ctx,annot);
    if(tool==Tool::Stamp&&(value.text.find_first_not_of(L" \t\r\n")==std::wstring::npos||value.text.size()>40))throw std::runtime_error("Enter the stamp text (up to 40 characters)");
    if(!geometric&&type!=PDF_ANNOT_FREE_TEXT&&type!=PDF_ANNOT_STAMP&&type!=PDF_ANNOT_TEXT&&type!=PDF_ANNOT_SQUARE&&type!=PDF_ANNOT_CIRCLE)
        throw std::runtime_error("This annotation type is not editable");
    const auto old=Call(d.ctx,[&]{return geometric?pdf_bound_annot(d.ctx,annot):pdf_annot_rect(d.ctx,annot);});
    auto map=[&](fz_point p){return fz_make_point(
        value.bounds.x+(p.x-old.x0)*value.bounds.w/std::max(.01f,old.x1-old.x0),
        value.bounds.y+(p.y-old.y0)*value.bounds.h/std::max(.01f,old.y1-old.y0));};
    std::vector<fz_point> points;std::vector<int> counts;std::vector<fz_quad> quads;
    if(type==PDF_ANNOT_LINE){
        fz_point a{},b{};Call(d.ctx,[&]{pdf_annot_line(d.ctx,annot,&a,&b);});points={map(a),map(b)};
        if(value.geometryEdited&&value.points.size()==2)points={{value.points[0].x,value.points[0].y},{value.points[1].x,value.points[1].y}};
        if(std::hypot(points[0].x-points[1].x,points[0].y-points[1].y)<.5f)throw std::runtime_error("Arrow endpoints must remain distinct");
    }else if(type==PDF_ANNOT_INK){
        const int strokes=Call(d.ctx,[&]{return pdf_annot_ink_list_count(d.ctx,annot);});
        for(int i=0;i<strokes;++i){
            const int n=Call(d.ctx,[&]{return pdf_annot_ink_list_stroke_count(d.ctx,annot,i);});counts.push_back(n);
            for(int j=0;j<n;++j)points.push_back(map(Call(d.ctx,[&]{return pdf_annot_ink_list_stroke_vertex(d.ctx,annot,i,j);})));
        }
    }else if(markup){
        const int n=Call(d.ctx,[&]{return pdf_annot_quad_point_count(d.ctx,annot);});
        for(int i=0;i<n;++i){auto q=Call(d.ctx,[&]{return pdf_annot_quad_point(d.ctx,annot,i);});q.ul=map(q.ul);q.ur=map(q.ur);q.ll=map(q.ll);q.lr=map(q.lr);quads.push_back(q);}
    }
    if(value.geometryEdited&&type==PDF_ANNOT_INK&&!value.points.empty()){
        if(std::accumulate(value.strokes.begin(),value.strokes.end(),0)!=static_cast<int>(value.points.size()))throw std::runtime_error("Invalid stroke lengths");
        counts=value.strokes;points.clear();for(auto p:value.points)points.push_back({p.x,p.y});
    }
    if(value.geometryEdited&&markup&&!value.quads.empty()){
        quads.clear();for(const auto& q:value.quads)quads.push_back({{q.ul.x,q.ul.y},{q.ur.x,q.ur.y},{q.ll.x,q.ll.y},{q.lr.x,q.lr.y}});
    }
    Rect bounds=value.bounds;
    if(tool==Tool::Stamp){
        // 改了印章文字：保持高度与中心，按新文字重算宽度。
        const char* previous=Call(d.ctx,[&]{return pdf_to_text_string(d.ctx,pdf_dict_gets(d.ctx,pdf_annot_obj(d.ctx,annot),"LumenStamp"));});
        if(text!=(previous?previous:"")){
            const auto crop=Public(Call(d.ctx,[&]{return pdf_bound_page(d.ctx,page.get(),FZ_CROP_BOX);}));
            const float scale=bounds.h/42,cx=bounds.x+bounds.w/2;
            bounds.w=std::min(crop.w,StampSize(d.ctx,text,{}).w*scale);
            bounds.x=std::clamp(cx-bounds.w/2,crop.x,crop.x+crop.w-bounds.w);
        }
    }
    float wrap=Call(d.ctx,[&]{return pdf_to_real(d.ctx,pdf_dict_gets(d.ctx,pdf_annot_obj(d.ctx,annot),"LumenTextWidth"));});
    if(value.fixedTextWidth||std::abs(value.bounds.w-(old.x1-old.x0))>.5f)wrap=value.bounds.w;
    d.Edit("Edit annotation",[&]{
        SetStyle(d.ctx,annot,tool,value.style);
        if(type==PDF_ANNOT_FREE_TEXT)SetTextAppearance(d.ctx,annot,value.textFormat,value.fontSize,value.text,value.fixedTextBox,value.textSizing);
        Call(d.ctx,[&]{
            if(type==PDF_ANNOT_LINE)pdf_set_annot_line(d.ctx,annot,points[0],points[1]);
            else if(type==PDF_ANNOT_INK)pdf_set_annot_ink_list(d.ctx,annot,static_cast<int>(counts.size()),counts.data(),points.data());
            else if(markup)pdf_set_annot_quad_points(d.ctx,annot,static_cast<int>(quads.size()),quads.data());
            else pdf_set_annot_rect(d.ctx,annot,Native(bounds));
            pdf_set_annot_opacity(d.ctx,annot,std::clamp(value.opacity,0.05f,1.0f));
            if(type!=PDF_ANNOT_FREE_TEXT)pdf_set_annot_contents(d.ctx,annot,text.c_str());
            if(tool==Tool::Stamp)pdf_dict_puts_drop(d.ctx,pdf_annot_obj(d.ctx,annot),"LumenStamp",pdf_new_text_string(d.ctx,text.c_str()));
            if(tool==Tool::Image||type==PDF_ANNOT_FREE_TEXT)pdf_dict_put_int(d.ctx,pdf_annot_obj(d.ctx,annot),PDF_NAME(Rotate),value.rotation);
            pdf_update_annot(d.ctx,annot);
            if(tool==Tool::Image){auto* appearance=pdf_dict_getp(d.ctx,pdf_annot_obj(d.ctx,annot),"AP/N");pdf_dict_put_matrix(d.ctx,appearance,PDF_NAME(Matrix),fz_rotate(static_cast<float>(value.rotation)));}
        });
        if(tool==Tool::Stamp)SetStampAppearance(d.ctx,d.doc,annot,bounds,text,value.style.color,std::clamp(value.opacity,0.05f,1.0f));
        if(type==PDF_ANNOT_FREE_TEXT&&wrap>0&&value.rotation==0&&!value.fixedTextBox){
            Call(d.ctx,[&]{pdf_dict_puts_drop(d.ctx,pdf_annot_obj(d.ctx,annot),"LumenTextWidth",pdf_new_real(d.ctx,wrap));});
            FitTextAnnotation(d.ctx,annot,wrap,std::clamp(value.fontSize,6.0f,96.0f));
        }
    });
}
void Document::DeleteAnnotation(int number,int id){
    auto& d=*impl_;auto page=d.Load(number);auto* a=d.Find(page.get(),id);
    const int flags=Call(d.ctx,[&]{return pdf_dict_get_int(d.ctx,pdf_annot_obj(d.ctx,a),PDF_NAME(F));});
    if(flags&(PDF_ANNOT_IS_READ_ONLY|PDF_ANNOT_IS_LOCKED))throw std::runtime_error("This annotation is locked");
    d.Edit("Delete annotation",[&]{Call(d.ctx,[&]{pdf_delete_annot(d.ctx,page.get(),a);});});
}
void Document::RotatePage(int number,int degrees){RotatePages({number},degrees);}
void Document::RotatePages(const std::vector<int>& pages,int degrees){
    auto& d=*impl_;if(degrees%90)throw std::runtime_error("Rotation must be a multiple of 90 degrees");
    if(pages.empty())return;for(int p:pages)if(p<0||p>=d.Count())throw std::runtime_error("Page index out of range");
    d.Edit("Rotate pages",[&]{for(int p:pages)Call(d.ctx,[&]{auto* obj=pdf_lookup_page_obj(d.ctx,d.doc,p);int r=pdf_dict_get_inheritable_int(d.ctx,obj,PDF_NAME(Rotate));pdf_dict_put_int(d.ctx,obj,PDF_NAME(Rotate),((r+degrees)%360+360)%360);});});
}
void Document::DuplicatePages(const std::vector<int>& pages){
    auto& d=*impl_;if(pages.empty())return;
    // Re-open a snapshot as a separate graft source. This gives independent page resources.
    TempDirectory temp;const auto file=temp.path/L"duplicate.pdf";Snapshot(file);
    const auto filename=PathString(file);
    Pdf input(d.ctx,Call(d.ctx,[&]{return pdf_open_document(d.ctx,filename.c_str());}));
    const auto pass=Utf8(d.openedPassword);Call(d.ctx,[&]{pdf_authenticate_password(d.ctx,input.get(),pass.c_str());});
    auto sorted=pages;std::sort(sorted.begin(),sorted.end());sorted.erase(std::unique(sorted.begin(),sorted.end()),sorted.end());
    for(int p:sorted)if(p<0||p>=d.Count())throw std::runtime_error("Page index out of range");
    BakeVisible(d.ctx,input.get());
    d.Edit("Duplicate pages",[&]{Held<pdf_graft_map,pdf_drop_graft_map> map(d.ctx,Call(d.ctx,[&]{return pdf_new_graft_map(d.ctx,d.doc);}));
        for(auto i=sorted.rbegin();i!=sorted.rend();++i)Call(d.ctx,[&]{pdf_graft_mapped_page(d.ctx,map.get(),*i+1,input.get(),*i);});});
}
void Document::CropPages(const std::vector<int>& pages,float left,float top,float right,float bottom){
    auto& d=*impl_;if(pages.empty())return;
    for(float v:{left,top,right,bottom})if(!std::isfinite(v)||v<0)throw std::runtime_error("Crop margins must be non-negative");
    std::vector<Rect> boxes;for(int p:pages){const auto g=d.Geometry(p);if(left+right>=g.width-1||top+bottom>=g.height-1)throw std::runtime_error("Crop removes the entire page");boxes.push_back({g.originX+left,g.originY+top,g.width-left-right,g.height-top-bottom});}
    d.Edit("Crop pages",[&]{for(size_t i=0;i<pages.size();++i){auto page=d.Load(pages[i]);Call(d.ctx,[&]{pdf_set_page_box(d.ctx,page.get(),FZ_CROP_BOX,Native(boxes[i]));});}});
}
void Document::ReplacePages(const std::vector<int>& pages,const fs::path& source,std::wstring_view password){
    auto& d=*impl_;if(pages.empty())return;auto sorted=pages;std::sort(sorted.begin(),sorted.end());sorted.erase(std::unique(sorted.begin(),sorted.end()),sorted.end());
    for(int p:sorted)if(p<0||p>=d.Count())throw std::runtime_error("Page index out of range");
    const auto name=PathString(source),pass=Utf8(password);
    Pdf input(d.ctx,Call(d.ctx,[&]{return pdf_open_document(d.ctx,name.c_str());}));
    if(Call(d.ctx,[&]{return pdf_needs_password(d.ctx,input.get());})&&!Call(d.ctx,[&]{return pdf_authenticate_password(d.ctx,input.get(),pass.c_str());}))throw PasswordRequired{};
    if(Call(d.ctx,[&]{return pdf_count_pages(d.ctx,input.get());})!=static_cast<int>(sorted.size()))throw std::runtime_error("Replacement PDF must have the same number of pages as the selection");
    BakeVisible(d.ctx,input.get());
    d.Edit("Replace pages",[&]{Held<pdf_graft_map,pdf_drop_graft_map> map(d.ctx,Call(d.ctx,[&]{return pdf_new_graft_map(d.ctx,d.doc);}));
        for(size_t i=0;i<sorted.size();++i)Call(d.ctx,[&]{pdf_delete_page(d.ctx,d.doc,sorted[i]);pdf_graft_mapped_page(d.ctx,map.get(),sorted[i],input.get(),static_cast<int>(i));});});
}
void Document::Reorder(const std::vector<int>& order){
    auto& d=*impl_;if(order.empty())throw std::runtime_error("Keep at least one page");
    for(int n:order)if(n<0||n>=d.Count())throw std::runtime_error("Invalid page order");
    d.Edit("Reorder pages",[&]{Call(d.ctx,[&]{pdf_rearrange_pages(d.ctx,d.doc,static_cast<int>(order.size()),order.data(),PDF_CLEAN_STRUCTURE_DROP);});});
}
void Document::DeletePages(const std::vector<int>& pages){
    std::vector<int> order;for(int n=0;n<impl_->Count();++n)if(std::find(pages.begin(),pages.end(),n)==pages.end())order.push_back(n);
    Reorder(order);
}
void Document::InsertBlank(int after){
    auto& d=*impl_;d.Require();const int index=std::clamp(after+1,0,d.Count());
    d.Edit("Insert blank page",[&]{
        Object page(d.ctx,Call(d.ctx,[&]{return pdf_add_page(d.ctx,d.doc,fz_make_rect(0,0,595.28f,841.89f),0,nullptr,nullptr);}));
        Call(d.ctx,[&]{pdf_insert_page(d.ctx,d.doc,index,page.get());});
    });
}
void Document::InsertPdf(int after,const fs::path& source,std::wstring_view password){
    auto& d=*impl_;const auto name=PathString(source),pass=Utf8(password);
    Pdf input(d.ctx,Call(d.ctx,[&]{return pdf_open_document(d.ctx,name.c_str());}));
    if(Call(d.ctx,[&]{return pdf_needs_password(d.ctx,input.get());})&&!Call(d.ctx,[&]{return pdf_authenticate_password(d.ctx,input.get(),pass.c_str());}))throw PasswordRequired{};
    // Page grafting omits annotations: bake their visible appearance before copying.
    BakeVisible(d.ctx,input.get());
    const int count=Call(d.ctx,[&]{return pdf_count_pages(d.ctx,input.get());});
    const int position=std::clamp(after+1,0,d.Count());
    d.Edit("Insert PDF",[&]{
        Held<pdf_graft_map,pdf_drop_graft_map> map(d.ctx,Call(d.ctx,[&]{return pdf_new_graft_map(d.ctx,d.doc);}));
        for(int i=0;i<count;++i)Call(d.ctx,[&]{pdf_graft_mapped_page(d.ctx,map.get(),position+i,input.get(),i);});
    });
}
void Document::Undo(){auto& d=*impl_;d.Require();d.ClearSelection();d.Invalidate();Call(d.ctx,[&]{pdf_undo(d.ctx,d.doc);});ResetResourceCache(d.ctx,d.doc);d.dirty=true;}
void Document::Redo(){auto& d=*impl_;d.Require();d.ClearSelection();d.Invalidate();Call(d.ctx,[&]{pdf_redo(d.ctx,d.doc);});ResetResourceCache(d.ctx,d.doc);d.dirty=true;}
void Document::Save(const fs::path& destination,const Cancel& cancel,const ProgressSink& progress){
    CheckCancel(cancel);
    auto& d=*impl_;d.Require();
    TemporaryFile temp{UniquePath(fs::absolute(destination).parent_path(),L".pdf")};
    ReportProgress(progress,ProgressStage::Writing);CheckCancel(cancel);
    d.SaveRaw(temp.path);CheckCancel(cancel);
    ReportProgress(progress,ProgressStage::Validating);CheckCancel(cancel);
    Document validation;validation.Open(temp.path,d.openedPassword);
    if(validation.impl_->Count()!=d.Count())throw std::runtime_error("Saved PDF verification failed");
    ReportProgress(progress,ProgressStage::Optimizing);CheckCancel(cancel);
    validation.impl_->Edit("Subset embedded fonts",[&]{Call(validation.impl_->ctx,[&]{pdf_subset_fonts(validation.impl_->ctx,validation.impl_->doc,0,nullptr);});});
    TemporaryFile optimized{UniquePath(fs::absolute(destination).parent_path(),L".pdf")};
    validation.impl_->SaveRaw(optimized.path,true);CheckCancel(cancel);
    ReportProgress(progress,ProgressStage::Validating);CheckCancel(cancel);
    Document verified;verified.Open(optimized.path,d.openedPassword);
    if(verified.impl_->Count()!=d.Count())throw std::runtime_error("Optimized PDF verification failed");
    ReportProgress(progress,ProgressStage::Publishing);CheckCancel(cancel);
    AtomicReplace(optimized.path,fs::absolute(destination));d.dirty=false;
}
namespace {
bool Overlaps(fz_rect a,fz_rect b){return a.x0<b.x1&&b.x0<a.x1&&a.y0<b.y1&&b.y0<a.y1;}
}
RedactionResult Document::ApplyRedactions(const std::vector<RedactionMark>& marks,const RedactionOptions& options){
    auto& d=*impl_;d.Require();
    const int count=d.Count();
    std::map<int,std::vector<fz_rect>> byPage;
    for(const auto& m:marks){
        if(m.page<0||m.page>=count)throw std::runtime_error("Redaction page is out of range");
        const auto& r=m.bounds;
        if(!std::isfinite(r.x)||!std::isfinite(r.y)||!std::isfinite(r.w)||!std::isfinite(r.h)||r.w<.5f||r.h<.5f)throw std::runtime_error("Redaction area is empty");
        byPage[m.page].push_back(Native(r));
    }
    if(byPage.empty())throw std::runtime_error("Mark at least one area to redact");
    pdf_redact_options redact{};
    redact.black_boxes=1;
    redact.image_method=options.removeWholeImages?PDF_REDACT_IMAGE_REMOVE:PDF_REDACT_IMAGE_PIXELS;
    redact.line_art=options.removeLineArt?PDF_REDACT_LINE_ART_REMOVE_IF_COVERED:PDF_REDACT_LINE_ART_NONE;
    redact.text=PDF_REDACT_TEXT_REMOVE;   // 与区域有任何重叠的文字都删除（包括 OCR 隐藏文字层）
    RedactionResult result;
    d.Edit("Redact",[&]{
        for(auto& [number,rects]:byPage){
            auto page=d.Load(number);
            auto hit=[&](fz_rect r){return std::any_of(rects.begin(),rects.end(),[&](const fz_rect& m){return Overlaps(r,m);});};
            Call(d.ctx,[&]{
                fz_context* ctx=d.ctx;pdf_page* pg=page.get();
                // 1. 重叠的批注：其内容 / 外观可能包含被涂黑的文字。连同弹出注释（/Popup、/Parent 互相引用）一起删除；
                //    按对象号删除，避免 MuPDF 自动删除弹出注释后再次访问已释放的指针。
                std::set<int> doomed;
                for(pdf_annot* a=pdf_first_annot(ctx,pg);a;a=pdf_next_annot(ctx,a)){
                    if(pdf_annot_type(ctx,a)==PDF_ANNOT_REDACT)continue;
                    if(!hit(pdf_bound_annot(ctx,a)))continue;
                    pdf_obj* obj=pdf_annot_obj(ctx,a);doomed.insert(pdf_to_num(ctx,obj));
                    if(pdf_obj* popup=pdf_dict_get(ctx,obj,PDF_NAME(Popup)))doomed.insert(pdf_to_num(ctx,popup));
                }
                for(pdf_annot* a=pdf_first_annot(ctx,pg);a;a=pdf_next_annot(ctx,a)){
                    pdf_obj* parent=pdf_dict_get(ctx,pdf_annot_obj(ctx,a),PDF_NAME(Parent));
                    if(parent&&doomed.contains(pdf_to_num(ctx,parent)))doomed.insert(pdf_to_num(ctx,pdf_annot_obj(ctx,a)));
                }
                for(int num:doomed){
                    for(pdf_annot* a=pdf_first_annot(ctx,pg);a;a=pdf_next_annot(ctx,a)){
                        if(pdf_to_num(ctx,pdf_annot_obj(ctx,a))==num){pdf_delete_annot(ctx,pg,a);++result.annotationsRemoved;break;}
                    }
                }
                // 2. 重叠的表单控件：清空值（沿父字段向上）、默认值与外观，并设为隐藏 + 只读。
                //    控件对象仍被表单树引用，不能只从页面移除，否则旧外观会随文件保留。
                for(pdf_annot* w=pdf_first_widget(ctx,pg);w;w=pdf_next_widget(ctx,w)){
                    if(!hit(pdf_bound_widget(ctx,w)))continue;
                    pdf_obj* obj=pdf_annot_obj(ctx,w);
                    int depth=0;
                    for(pdf_obj* f=obj;f&&depth<32;f=pdf_dict_get(ctx,f,PDF_NAME(Parent)),++depth){pdf_dict_del(ctx,f,PDF_NAME(V));pdf_dict_del(ctx,f,PDF_NAME(DV));}
                    pdf_dict_del(ctx,obj,PDF_NAME(AP));pdf_dict_del(ctx,obj,PDF_NAME(AS));pdf_dict_del(ctx,obj,PDF_NAME(MK));
                    pdf_dict_put_int(ctx,obj,PDF_NAME(F),pdf_dict_get_int(ctx,obj,PDF_NAME(F))|PDF_ANNOT_IS_HIDDEN|PDF_ANNOT_IS_READ_ONLY);
                    pdf_dict_put_int(ctx,obj,PDF_NAME(Ff),pdf_dict_get_int(ctx,obj,PDF_NAME(Ff))|PDF_FIELD_IS_READ_ONLY);
                    ++result.fieldsCleared;
                }
            });
            // 3. 为每个区域建立涂黑批注并应用到页面内容。
            for(const auto& r:rects){
                Held<pdf_annot,pdf_drop_annot> annot(d.ctx,Call(d.ctx,[&]{return pdf_create_annot(d.ctx,page.get(),PDF_ANNOT_REDACT);}));
                Call(d.ctx,[&]{pdf_set_annot_rect(d.ctx,annot.get(),r);});
            }
            Call(d.ctx,[&]{pdf_redact_page(d.ctx,d.doc,page.get(),&redact);});
            ++result.pages;result.marks+=static_cast<int>(rects.size());
        }
        if(options.clearMetadata)Call(d.ctx,[&]{
            pdf_obj* trailer=pdf_trailer(d.ctx,d.doc);
            if(pdf_obj* info=pdf_dict_get(d.ctx,trailer,PDF_NAME(Info))){
                for(pdf_obj* key:{PDF_NAME(Title),PDF_NAME(Author),PDF_NAME(Subject),PDF_NAME(Keywords),PDF_NAME(Creator)})pdf_dict_del(d.ctx,info,key);
            }
            if(pdf_obj* root=pdf_dict_get(d.ctx,trailer,PDF_NAME(Root)))pdf_dict_del(d.ctx,root,PDF_NAME(Metadata));
        });
    });
    d.Invalidate();
    return result;
}
RedactionResult Document::ExportRedacted(const std::vector<RedactionMark>& marks,const RedactionOptions& options,const fs::path& destination,const Cancel& cancel){
    auto& d=*impl_;d.Require();CheckCancel(cancel);
    if(marks.empty())throw std::runtime_error("Mark at least one area to redact");
    const auto folder=fs::absolute(destination).parent_path();
    TemporaryFile plain{UniquePath(folder,L".pdf")};
    d.SaveRaw(plain.path);CheckCancel(cancel);
    Document copy;copy.Open(plain.path,d.openedPassword);
    const auto result=copy.ApplyRedactions(marks,options);CheckCancel(cancel);
    // Save 的最终写出做垃圾回收：被删除的内容流、图片与批注不会作为孤立对象留在文件里。
    TemporaryFile staged{UniquePath(folder,L".pdf")};
    copy.Save(staged.path,cancel);CheckCancel(cancel);
    Document check;check.Open(staged.path,d.openedPassword);
    if(check.impl_->Count()!=d.Count())throw std::runtime_error("Redacted PDF verification failed");
    for(const auto& m:marks){
        const auto left=check.Text(m.page,m.bounds);
        if(std::any_of(left.begin(),left.end(),[](wchar_t c){return !iswspace(c);}))throw std::runtime_error("Redaction verification failed: text remains inside a marked area");
    }
    AtomicReplace(staged.path,fs::absolute(destination));
    return result;
}
void Document::ExportSecured(const fs::path& destination,const Security& security,const Cancel& cancel){
    auto& d=*impl_;d.Require();CheckCancel(cancel);
    const auto user=Utf8(security.userPassword);auto owner=Utf8(security.ownerPassword);
    if(user.size()>=127||owner.size()>=127)throw std::runtime_error("Password is too long");
    const bool restricted=!(security.allowPrint&&security.allowCopy&&security.allowModify&&security.allowAnnotate);
    const bool encrypt=!user.empty()||!owner.empty()||restricted;
    if(encrypt&&owner.empty()){
        // 没有权限口令时用随机口令：否则任何人都能以空口令获得全部权限，限制形同虚设。
        unsigned char random[18]{};
        if(BCryptGenRandom(nullptr,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)throw std::runtime_error("Random password generation failed");
        static const char digits[]="0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        for(unsigned char c:random)owner+=digits[c%62];
    }
    const auto folder=fs::absolute(destination).parent_path();
    TemporaryFile plain{UniquePath(folder,L".pdf")};
    d.SaveRaw(plain.path);CheckCancel(cancel);
    Document copy;copy.Open(plain.path,d.openedPassword);
    auto& c=*copy.impl_;
    TemporaryFile output{UniquePath(folder,L".pdf")};
    pdf_write_options options=pdf_default_write_options;
    options.do_compress=1;options.do_compress_images=1;options.do_compress_fonts=1;options.do_garbage=3;
    if(!encrypt)options.do_encrypt=PDF_ENCRYPT_NONE;
    else{
        options.do_encrypt=PDF_ENCRYPT_AES_256;
        int permissions=PDF_PERM_ACCESSIBILITY;
        if(security.allowPrint)permissions|=PDF_PERM_PRINT|PDF_PERM_PRINT_HQ;
        if(security.allowCopy)permissions|=PDF_PERM_COPY;
        if(security.allowModify)permissions|=PDF_PERM_MODIFY|PDF_PERM_ASSEMBLE;
        if(security.allowAnnotate)permissions|=PDF_PERM_ANNOTATE|PDF_PERM_FORM;
        options.permissions=restricted?permissions:-1;
        strncpy_s(options.upwd_utf8,user.c_str(),_TRUNCATE);strncpy_s(options.opwd_utf8,owner.c_str(),_TRUNCATE);
    }
    const auto filename=PathString(output.path);
    Call(c.ctx,[&]{pdf_save_document(c.ctx,c.doc,filename.c_str(),&options);});CheckCancel(cancel);
    // 校验：能用新口令打开且页数一致；设置了打开口令时，空口令必须打不开。
    Document verified;verified.Open(output.path,security.userPassword);
    if(verified.impl_->Count()!=d.Count())throw std::runtime_error("Secured PDF verification failed");
    if((verified.impl_->doc->crypt!=nullptr)!=encrypt)throw std::runtime_error("Secured PDF encryption state mismatch");
    if(!user.empty()){
        bool locked=false;try{Document probe;probe.Open(output.path);}catch(const PasswordRequired&){locked=true;}
        if(!locked)throw std::runtime_error("Secured PDF opens without password");
    }
    AtomicReplace(output.path,fs::absolute(destination));
}
std::vector<fs::path> Document::ExportImages(const std::vector<int>& pages,const fs::path& folder,std::wstring_view stem,float dpi,const Cancel& cancel,const std::function<void(int,int)>& progress){
    auto& d=*impl_;d.Require();
    if(pages.empty())throw std::runtime_error("Select at least one page");
    if(!std::isfinite(dpi)||dpi<36||dpi>600)throw std::runtime_error("Invalid export resolution");
    std::error_code error;fs::create_directories(folder,error);
    const int count=d.Count();const int width=count>=1000?4:count>=100?3:count>=10?2:1;
    std::vector<fs::path> written;
    for(size_t i=0;i<pages.size();++i){
        CheckCancel(cancel);
        const int number=pages[i];
        if(number<0||number>=count)throw std::runtime_error("Page index out of range");
        const auto geometry=d.Geometry(number);
        float scale=dpi/72.0f;
        // 与 Render 相同的像素上限，超大页面自动降低分辨率。
        const double pixels=static_cast<double>(geometry.width)*geometry.height*scale*scale;
        if(pixels>60000000.0)scale*=static_cast<float>(std::sqrt(60000000.0/pixels));
        auto page=d.Load(number);
        Pixmap pix(d.ctx,Call(d.ctx,[&]{return fz_new_pixmap_from_page(d.ctx,reinterpret_cast<fz_page*>(page.get()),fz_scale(scale,scale),fz_device_rgb(d.ctx),0);}));
        std::wstring digits=std::to_wstring(number+1);if(static_cast<int>(digits.size())<width)digits.insert(0,static_cast<size_t>(width)-digits.size(),L'0');
        const auto target=fs::absolute(folder/(std::wstring(stem)+L"-"+digits+L".png"));
        TemporaryFile temp{UniquePath(target.parent_path(),L".png")};
        const auto name=PathString(temp.path);
        Call(d.ctx,[&]{fz_save_pixmap_as_png(d.ctx,pix.get(),name.c_str());});
        AtomicReplace(temp.path,target);written.push_back(target);
        if(progress)progress(static_cast<int>(i+1),static_cast<int>(pages.size()));
    }
    return written;
}
void Document::ExportText(const fs::path& destination,const Cancel& cancel){
    auto& d=*impl_;d.Require();
    std::string text="\xEF\xBB\xBF";
    for(int i=0;i<d.Count();++i){
        CheckCancel(cancel);
        if(i)text+="\r\n\f\r\n";
        auto page=Text(i);std::wstring normalized;normalized.reserve(page.size());
        for(size_t k=0;k<page.size();++k){if(page[k]==L'\n'&&(k==0||page[k-1]!=L'\r'))normalized+=L'\r';normalized+=page[k];}
        text+=Utf8(normalized);
    }
    const auto target=fs::absolute(destination);
    TemporaryFile temp{UniquePath(target.parent_path(),L".txt")};
    WriteBytes(temp.path,std::vector<unsigned char>(text.begin(),text.end()));
    AtomicReplace(temp.path,target);
}
void Document::MarkDirty(){impl_->dirty=true;}
void Document::Snapshot(const fs::path& path){const bool dirty=impl_->dirty;try{Save(path);}catch(...){impl_->dirty=dirty;throw;}impl_->dirty=dirty;}
void Document::Extract(const std::vector<int>& pages,const fs::path& destination){
    if(pages.empty())throw std::runtime_error("Select at least one page");
    TempDirectory temp;const auto copy=temp.path/L"source.pdf";
    impl_->SaveRaw(copy);
    Document extracted;extracted.Open(copy,impl_->openedPassword);extracted.Reorder(pages);extracted.Save(destination);
}
CompressResult Document::Compress(const fs::path& destination,CompressLevel level,const Cancel& cancel){
    auto& d=*impl_;d.Require();CheckCancel(cancel);
    const auto target=fs::absolute(destination);const auto folder=target.parent_path();
    TemporaryFile plain{UniquePath(folder,L".pdf")};
    d.SaveRaw(plain.path);CheckCancel(cancel);
    // 基准用磁盘上的原文件大小；新建文档没有原文件时，用正式保存会得到的大小（字体子集化后）估算。
    CompressResult result;result.before=d.bytes.size();
    Document copy;copy.Open(plain.path,d.openedPassword);auto& c=*copy.impl_;
    c.Edit("Subset embedded fonts",[&]{Call(c.ctx,[&]{pdf_subset_fonts(c.ctx,c.doc,0,nullptr);});});
    CheckCancel(cancel);
    if(result.before==0){TemporaryFile baseline{UniquePath(folder,L".pdf")};c.SaveRaw(baseline.path,true);result.before=fs::file_size(baseline.path);}
    if(level!=CompressLevel::Lossless){
        // 只处理超过阈值分辨率的图片，且只在结果更小时替换；个别图片格式不支持时退回无损压缩。
        const bool small=level==CompressLevel::Smallest;
        char quality[8]{};strcpy_s(quality,small?"55":"75");
        pdf_image_rewriter_options options{};
        options.color_lossy_image_subsample_method=options.color_lossless_image_subsample_method=FZ_SUBSAMPLE_BICUBIC;
        options.gray_lossy_image_subsample_method=options.gray_lossless_image_subsample_method=FZ_SUBSAMPLE_BICUBIC;
        options.color_lossy_image_subsample_threshold=options.color_lossless_image_subsample_threshold=small?150:225;
        options.color_lossy_image_subsample_to=options.color_lossless_image_subsample_to=small?100:150;
        options.gray_lossy_image_subsample_threshold=options.gray_lossless_image_subsample_threshold=small?150:225;
        options.gray_lossy_image_subsample_to=options.gray_lossless_image_subsample_to=small?100:150;
        options.color_lossy_image_recompress_method=options.gray_lossy_image_recompress_method=FZ_RECOMPRESS_JPEG;
        options.color_lossless_image_recompress_method=options.gray_lossless_image_recompress_method=small?FZ_RECOMPRESS_JPEG:FZ_RECOMPRESS_LOSSLESS;
        options.color_lossy_image_recompress_quality=options.color_lossless_image_recompress_quality=quality;
        options.gray_lossy_image_recompress_quality=options.gray_lossless_image_recompress_quality=quality;
        options.bitonal_image_recompress_method=FZ_RECOMPRESS_NEVER;
        options.recompress_when=FZ_RECOMPRESS_WHEN_SMALLER;
        try{c.Edit("Rewrite images",[&]{Call(c.ctx,[&]{pdf_rewrite_images(c.ctx,c.doc,&options);});});}
        catch(const std::exception&){/* 保持原图片，其余优化照常进行。 */}
        CheckCancel(cancel);
    }
    TemporaryFile output{UniquePath(folder,L".pdf")};
    pdf_write_options write=pdf_default_write_options;
    write.do_compress=1;write.do_compress_images=1;write.do_compress_fonts=1;
    write.do_garbage=level==CompressLevel::Smallest?4:3;write.do_use_objstms=1;
    write.do_clean=level==CompressLevel::Smallest;write.compression_effort=level==CompressLevel::Lossless?0:100;
    const auto filename=PathString(output.path);
    Call(c.ctx,[&]{pdf_save_document(c.ctx,c.doc,filename.c_str(),&write);});CheckCancel(cancel);
    Document verified;verified.Open(output.path,d.openedPassword);
    if(verified.impl_->Count()!=d.Count())throw std::runtime_error("Compressed PDF verification failed");
    result.after=fs::file_size(output.path);
    AtomicReplace(output.path,target);
    return result;
}
namespace {
std::wstring SafeName(std::wstring text){
    for(auto& ch:text)if(ch<32||wcschr(L"\\/:*?\"<>|",ch))ch=L'_';
    while(!text.empty()&&(text.back()==L' '||text.back()==L'.'))text.pop_back();
    while(!text.empty()&&text.front()==L' ')text.erase(text.begin());
    if(text.size()>60)text.resize(60);
    return text;
}
}
std::vector<fs::path> Document::Split(SplitMode mode,int amount,const fs::path& folder,std::wstring_view stem,const Cancel& cancel,const std::function<void(int,int)>& progress){
    auto& d=*impl_;d.Require();CheckCancel(cancel);
    const int count=d.Count();
    struct Part{int first{},last{};std::wstring label;};
    std::vector<Part> parts;
    TempDirectory temp;const auto source=temp.path/L"source.pdf";
    d.SaveRaw(source);CheckCancel(cancel);
    auto range=[](int first,int last){std::vector<int> pages(static_cast<size_t>(last-first+1));std::iota(pages.begin(),pages.end(),first);return pages;};
    if(mode==SplitMode::EveryPages){
        if(amount<1||amount>=count)throw std::runtime_error("Choose a page count smaller than the document");
        for(int first=0;first<count;first+=amount)parts.push_back({first,std::min(count,first+amount)-1,{}});
    }else if(mode==SplitMode::TopBookmarks){
        std::vector<std::pair<int,std::wstring>> starts;
        for(const auto& item:Info().outline)if(item.depth==0&&(starts.empty()||item.page>starts.back().first))starts.push_back({item.page,item.title});
        if(starts.empty())throw std::runtime_error("The document has no top-level bookmarks");
        if(starts.front().first>0)starts.insert(starts.begin(),{0,L"开头"});
        if(starts.size()<2)throw std::runtime_error("Bookmarks do not divide the document");
        for(size_t i=0;i<starts.size();++i)parts.push_back({starts[i].first,i+1<starts.size()?starts[i+1].first-1:count-1,starts[i].second});
    }else{
        if(amount<1||amount>4096)throw std::runtime_error("Choose a size between 1 and 4096 MB");
        const uint64_t limit=static_cast<uint64_t>(amount)*1024*1024;
        // 实测大小：对候选页码范围生成优化后的副本，倍增后二分，找出不超过上限的最长范围。
        auto measure=[&](int first,int last){
            CheckCancel(cancel);
            Document probe;probe.Open(source,d.openedPassword);probe.Reorder(range(first,last));
            // 与正式保存一致：嵌入字体先子集化，否则整套中文字体会让估算大小失真。
            auto& pi=*probe.impl_;pi.Edit("Subset embedded fonts",[&]{Call(pi.ctx,[&]{pdf_subset_fonts(pi.ctx,pi.doc,0,nullptr);});});
            const auto file=UniquePath(temp.path,L".pdf");pi.SaveRaw(file,true);
            const auto size=fs::file_size(file);std::error_code error;fs::remove(file,error);return size;
        };
        for(int first=0;first<count;){
            int good=first,bad=count,step=1;   // 单页总是自成一份，即使超过上限
            while(good<count-1){
                const int last=std::min(count-1,good+step);
                if(measure(first,last)<=limit){good=last;step*=2;}else{bad=last;break;}
            }
            while(bad<count&&bad-good>1){const int mid=(good+bad)/2;if(measure(first,mid)<=limit)good=mid;else bad=mid;}
            parts.push_back({first,good,{}});first=good+1;
        }
        if(parts.size()<2)throw std::runtime_error("The whole document already fits within this size");
    }
    std::vector<fs::path> written;
    const int digits=parts.size()>=100?3:2;
    std::error_code error;fs::create_directories(folder,error);
    for(size_t i=0;i<parts.size();++i){
        CheckCancel(cancel);
        const auto& part=parts[i];
        std::wstring number=std::to_wstring(i+1);if(static_cast<int>(number.size())<digits)number.insert(0,static_cast<size_t>(digits)-number.size(),L'0');
        std::wstring label=SafeName(part.label);
        if(label.empty())label=part.first==part.last?L"第"+std::to_wstring(part.first+1)+L"页":L"第"+std::to_wstring(part.first+1)+L"-"+std::to_wstring(part.last+1)+L"页";
        auto target=fs::absolute(folder/(std::wstring(stem)+L"-"+number+L" "+label+L".pdf"));
        for(int n=2;fs::exists(target);++n)target=fs::absolute(folder/(std::wstring(stem)+L"-"+number+L" "+label+L" ("+std::to_wstring(n)+L").pdf"));
        Document piece;piece.Open(source,d.openedPassword);piece.Reorder(range(part.first,part.last));piece.Save(target,cancel);
        written.push_back(target);
        if(progress)progress(static_cast<int>(i+1),static_cast<int>(parts.size()));
    }
    return written;
}
void Document::Decorate(const PageDecoration& deco,const Cancel& cancel){
    auto& d=*impl_;d.Require();
    const bool any=!deco.header.empty()||!deco.footer.empty()||!deco.watermark.empty();
    if(!any)throw std::runtime_error("Enter a header, footer or watermark");
    if(!std::isfinite(deco.fontSize)||deco.fontSize<4||deco.fontSize>72||!std::isfinite(deco.margin)||deco.margin<0||deco.margin>200||
       !std::isfinite(deco.watermarkSize)||deco.watermarkSize<8||deco.watermarkSize>400||!std::isfinite(deco.watermarkOpacity)||
       deco.watermarkOpacity<.02f||deco.watermarkOpacity>1||!std::isfinite(deco.watermarkAngle)||std::abs(deco.watermarkAngle)>360||
       deco.header.size()>200||deco.footer.size()>200||deco.watermark.size()>100)throw std::runtime_error("Invalid header, footer or watermark settings");
    const int count=d.Count();
    std::vector<int> pages=deco.pages;
    if(pages.empty()){pages.resize(static_cast<size_t>(count));std::iota(pages.begin(),pages.end(),0);}
    for(int p:pages)if(p<0||p>=count)throw std::runtime_error("Page index out of range");
    auto expand=[&](const std::wstring& text,int page){
        std::wstring out=text;
        auto replace=[&](std::wstring_view key,const std::wstring& value){for(size_t at=out.find(key);at!=std::wstring::npos;at=out.find(key,at+value.size()))out.replace(at,key.size(),value);};
        replace(L"{page}",std::to_wstring(page+deco.startNumber));replace(L"{total}",std::to_wstring(count+deco.startNumber-1));
        return Utf8(out);
    };
    auto font=SystemFont(d.ctx,false);
    d.Edit("Add header, footer and watermark",[&]{
        for(int index:pages){
            CheckCancel(cancel);
            auto* pageobj=Call(d.ctx,[&]{return pdf_lookup_page_obj(d.ctx,d.doc,index);});
            fz_rect box{};fz_matrix ctm{};
            Call(d.ctx,[&]{pdf_page_obj_transform_box(d.ctx,pageobj,&box,&ctm,FZ_CROP_BOX);});
            const auto visible=fz_transform_rect(box,ctm);
            const float w=visible.x1-visible.x0,h=visible.y1-visible.y0;
            // 在独立资源的表单对象里绘制，避免与页面已有的字体、图形状态重名。
            Buffer front(d.ctx,Call(d.ctx,[&]{return fz_new_buffer(d.ctx,256);})),back(d.ctx,Call(d.ctx,[&]{return fz_new_buffer(d.ctx,128);}));
            Object frontRes(d.ctx,Call(d.ctx,[&]{return pdf_new_dict(d.ctx,d.doc,2);})),backRes(d.ctx,Call(d.ctx,[&]{return pdf_new_dict(d.ctx,d.doc,2);}));
            const auto topctm=fz_invert_matrix(ctm);
            ResetResourceCache(d.ctx,d.doc);
            Device frontDev(d.ctx,Call(d.ctx,[&]{return pdf_new_pdf_device(d.ctx,d.doc,topctm,frontRes.get(),front.get());}));
            Device backDev(d.ctx,Call(d.ctx,[&]{return pdf_new_pdf_device(d.ctx,d.doc,topctm,backRes.get(),back.get());}));
            bool drewFront=false,drewBack=false;
            auto line=[&](const std::wstring& raw,int align,bool top){
                if(raw.empty())return;
                const auto text=expand(raw,index);
                const float width=TextWidth(d.ctx,font.get(),text,deco.fontSize);
                const float x=align==0?visible.x0+deco.margin:align==2?visible.x1-deco.margin-width:visible.x0+(w-width)/2;
                const float y=top?visible.y0+deco.margin+deco.fontSize*.8f:visible.y1-deco.margin;
                DrawText(d.ctx,frontDev.get(),font.get(),text,deco.fontSize,fz_translate(x,y),deco.color,1);drewFront=true;
            };
            line(deco.header,deco.headerAlign,true);line(deco.footer,deco.footerAlign,false);
            if(!deco.watermark.empty()){
                const auto text=expand(deco.watermark,index);
                const float unit=std::max(.01f,TextWidth(d.ctx,font.get(),text,1));
                const float size=std::min(deco.watermarkSize,std::hypot(w,h)*.8f/unit);
                const float width=unit*size;
                const auto placement=fz_concat(fz_concat(fz_translate(-width/2,size*.36f),fz_rotate(-deco.watermarkAngle)),fz_translate(visible.x0+w/2,visible.y0+h/2));
                DrawText(d.ctx,deco.watermarkBehind?backDev.get():frontDev.get(),font.get(),text,size,placement,deco.watermarkColor,deco.watermarkOpacity);
                (deco.watermarkBehind?drewBack:drewFront)=true;
            }
            Call(d.ctx,[&]{fz_close_device(d.ctx,frontDev.get());fz_close_device(d.ctx,backDev.get());});
            Call(d.ctx,[&]{
                // 页面资源若继承自父节点，先复制到本页再添加表单对象名。
                // 资源可能继承自父节点或被多页共享：复制一份浅拷贝到本页，再添加表单对象名。
                auto* inherited=pdf_dict_get_inheritable(d.ctx,pageobj,PDF_NAME(Resources));
                pdf_dict_put_drop(d.ctx,pageobj,PDF_NAME(Resources),inherited?pdf_copy_dict(d.ctx,inherited):pdf_new_dict(d.ctx,d.doc,1));
                auto* resources=pdf_dict_get(d.ctx,pageobj,PDF_NAME(Resources));
                auto* xobjects=pdf_dict_get(d.ctx,resources,PDF_NAME(XObject));
                if(!xobjects)xobjects=pdf_dict_put_dict(d.ctx,resources,PDF_NAME(XObject),2);
                else if(pdf_is_indirect(d.ctx,xobjects)){pdf_dict_put_drop(d.ctx,resources,PDF_NAME(XObject),pdf_copy_dict(d.ctx,xobjects));xobjects=pdf_dict_get(d.ctx,resources,PDF_NAME(XObject));}
                const auto bbox=fz_transform_rect(visible,topctm);
                auto name=[&](const char* base){
                    char key[48];for(int n=1;;++n){snprintf(key,sizeof(key),"%s%d",base,n);if(!pdf_dict_gets(d.ctx,xobjects,key))break;}
                    return std::string(key);
                };
                auto addForm=[&](fz_buffer* buf,pdf_obj* res,const char* base){
                    const auto key=name(base);
                    pdf_obj* form=pdf_new_xobject(d.ctx,d.doc,bbox,fz_identity,res,buf);
                    pdf_dict_puts_drop(d.ctx,xobjects,key.c_str(),form);
                    return key;
                };
                auto stream=[&](const std::string& text){return AddContentStream(d.ctx,d.doc,text.data(),text.size());};
                auto* contents=pdf_dict_get(d.ctx,pageobj,PDF_NAME(Contents));
                pdf_obj* list=pdf_new_array(d.ctx,d.doc,4);
                std::string before="q\n";
                if(drewBack)before="q /"+addForm(back.get(),backRes.get(),"LumenMark")+" Do Q\n"+before;
                pdf_array_push_drop(d.ctx,list,stream(before));
                if(pdf_is_array(d.ctx,contents))for(int i=0;i<pdf_array_len(d.ctx,contents);++i)pdf_array_push(d.ctx,list,pdf_array_get(d.ctx,contents,i));
                else if(contents)pdf_array_push(d.ctx,list,contents);
                std::string after="\nQ\n";
                if(drewFront)after+="q /"+addForm(front.get(),frontRes.get(),"LumenDeco")+" Do Q\n";
                pdf_array_push_drop(d.ctx,list,stream(after));
                pdf_dict_put_drop(d.ctx,pageobj,PDF_NAME(Contents),list);
            });
        }
    });
}
void Document::SetOutline(const std::vector<OutlineItem>& items){
    auto& d=*impl_;d.Require();
    const int count=d.Count();
    if(items.size()>20000)throw std::runtime_error("Too many bookmarks");
    for(size_t i=0;i<items.size();++i){
        const auto& item=items[i];
        if(item.page<0||item.page>=count)throw std::runtime_error("Bookmark page out of range");
        if(item.title.empty()||item.title.size()>500)throw std::runtime_error("Bookmark titles must be 1-500 characters");
        const int previous=i?items[i-1].depth:-1;
        if(item.depth<0||item.depth>previous+1||item.depth>31)throw std::runtime_error("Invalid bookmark nesting");
    }
    d.Edit("Edit bookmarks",[&]{Call(d.ctx,[&]{
        auto* root=pdf_dict_get(d.ctx,pdf_trailer(d.ctx,d.doc),PDF_NAME(Root));
        pdf_dict_del(d.ctx,root,PDF_NAME(Outlines));
        if(items.empty())return;
        pdf_obj* outline=pdf_add_new_dict(d.ctx,d.doc,4);
        pdf_dict_put(d.ctx,outline,PDF_NAME(Type),PDF_NAME(Outlines));
        pdf_dict_put_drop(d.ctx,root,PDF_NAME(Outlines),outline);
        std::vector<pdf_obj*> nodes;nodes.reserve(items.size());
        std::vector<int> parents(items.size(),-1);std::vector<int> stack;
        for(size_t i=0;i<items.size();++i){
            while(static_cast<int>(stack.size())>items[i].depth)stack.pop_back();
            parents[i]=stack.empty()?-1:stack.back();stack.push_back(static_cast<int>(i));
            pdf_obj* node=pdf_add_new_dict(d.ctx,d.doc,6);nodes.push_back(node);
            const auto title=Utf8(items[i].title);
            pdf_dict_put_text_string(d.ctx,node,PDF_NAME(Title),title.c_str());
            auto* pageobj=pdf_lookup_page_obj(d.ctx,d.doc,items[i].page);
            auto* dest=pdf_dict_put_array(d.ctx,node,PDF_NAME(Dest),5);
            pdf_array_push(d.ctx,dest,pageobj);
            if(std::isfinite(items[i].y)){
                fz_rect box{};fz_matrix ctm{};pdf_page_obj_transform(d.ctx,pageobj,&box,&ctm);
                const auto p=fz_transform_point(fz_make_point(0,items[i].y),fz_invert_matrix(ctm));
                pdf_array_push(d.ctx,dest,PDF_NAME(XYZ));pdf_array_push(d.ctx,dest,PDF_NULL);pdf_array_push_real(d.ctx,dest,p.y);pdf_array_push(d.ctx,dest,PDF_NULL);
            }else pdf_array_push(d.ctx,dest,PDF_NAME(Fit));
        }
        // 连接兄弟与父子关系；Count 为展开时可见的后代数量。
        std::vector<int> descendants(items.size(),0);
        for(size_t i=items.size();i-->0;)if(parents[i]>=0)descendants[static_cast<size_t>(parents[i])]+=descendants[i]+1;
        auto link=[&](pdf_obj* parent,int parentIndex){
            pdf_obj* first=nullptr;pdf_obj* last=nullptr;int total=0;
            for(size_t i=0;i<items.size();++i){
                if(parents[i]!=parentIndex)continue;
                pdf_dict_put(d.ctx,nodes[i],PDF_NAME(Parent),parent);
                if(last){pdf_dict_put(d.ctx,last,PDF_NAME(Next),nodes[i]);pdf_dict_put(d.ctx,nodes[i],PDF_NAME(Prev),last);}
                else first=nodes[i];
                last=nodes[i];total+=1+descendants[i];
            }
            if(first){pdf_dict_put(d.ctx,parent,PDF_NAME(First),first);pdf_dict_put(d.ctx,parent,PDF_NAME(Last),last);pdf_dict_put_int(d.ctx,parent,PDF_NAME(Count),total);}
        };
        link(outline,-1);
        for(size_t i=0;i<items.size();++i)link(nodes[i],static_cast<int>(i));
        for(auto* node:nodes)pdf_drop_obj(d.ctx,node);
    });});
}
void Document::TextToPdf(std::wstring_view text,const fs::path& output,const Cancel& cancel,const ProgressSink& progress){
    Document context;auto& d=*context.impl_;const auto html=Html(text),name=PathString(output);
    Buffer input(d.ctx,Call(d.ctx,[&]{return fz_new_buffer_from_copied_data(d.ctx,reinterpret_cast<const unsigned char*>(html.data()),html.size());}));
    Held<fz_story,fz_drop_story> story(d.ctx,Call(d.ctx,[&]{return fz_new_story(d.ctx,input.get(),nullptr,11,nullptr);}));
    Held<fz_document_writer,fz_drop_document_writer> writer(d.ctx,Call(d.ctx,[&]{return fz_new_pdf_writer(d.ctx,name.c_str(),"compress=yes,garbage=3");}));
    int more=1,pages=0;fz_rect filled{};
    ReportProgress(progress,ProgressStage::TextLayout);
    do{
        CheckCancel(cancel);
        if(++pages>10000)throw std::runtime_error("Text conversion exceeded 10000 pages");
        fz_device* dev=Call(d.ctx,[&]{return fz_begin_page(d.ctx,writer.get(),fz_make_rect(0,0,595.28f,841.89f));});
        more=Call(d.ctx,[&]{return fz_place_story(d.ctx,story.get(),fz_make_rect(42,42,553.28f,799.89f),&filled);});
        Call(d.ctx,[&]{fz_draw_story(d.ctx,story.get(),dev,fz_identity);fz_end_page(d.ctx,writer.get());});
        ReportProgress(progress,ProgressStage::TextLayout,pages,more?0:pages);
    }while(more);
    CheckCancel(cancel);
    Call(d.ctx,[&]{fz_close_document_writer(d.ctx,writer.get());});
    fz_drop_document_writer(d.ctx,std::exchange(writer.p,nullptr));
    Document compact;compact.Open(output);compact.Save(output,cancel,progress);
}
namespace {
// 在 doc 中生成一页 width×height（点），图片等比缩放后居中，四边留 pad；缩放不超过 maxScale（点 / 像素）。
// 返回新页对象（新引用，调用方负责插入并释放）。
pdf_obj* NewImagePage(fz_context* ctx,pdf_document* doc,fz_image* image,float width,float height,float pad,float maxScale){
    const float scale=std::min({(width-2*pad)/image->w,(height-2*pad)/image->h,maxScale});
    if(!(scale>0))throw std::runtime_error("Invalid image page size");
    const float w=image->w*scale,h=image->h*scale;
    const auto bounds=fz_make_rect(0,0,width,height);
    Object resources(ctx);Buffer contents(ctx);
    Device dev(ctx,Call(ctx,[&]{return pdf_page_write(ctx,doc,bounds,&resources.p,&contents.p);}));
    Call(ctx,[&]{fz_fill_image(ctx,dev.get(),image,fz_make_matrix(w,0,0,h,(width-w)/2,(height-h)/2),1,fz_default_color_params);fz_close_device(ctx,dev.get());});
    return Call(ctx,[&]{return pdf_add_page(ctx,doc,bounds,0,resources.get(),contents.get());});
}
}
std::pair<int,int> Document::ImageSize(const fs::path& image){
    Document context;auto& d=*context.impl_;const auto name=PathString(image);
    Image img(d.ctx,Call(d.ctx,[&]{return fz_new_image_from_file(d.ctx,name.c_str());}));
    return {img.p->w,img.p->h};
}
void Document::NewFromImage(const fs::path& image){
    auto& d=*impl_;const auto name=PathString(image);
    Image img(d.ctx,Call(d.ctx,[&]{return fz_new_image_from_file(d.ctx,name.c_str());}));
    if(img.p->w<=0||img.p->h<=0)throw std::runtime_error("Invalid image dimensions");
    float width=img.p->w*0.75f,height=img.p->h*0.75f;
    const float shrink=std::min(1.0f,14400.0f/std::max(width,height));
    width=std::max(3.0f,width*shrink);height=std::max(3.0f,height*shrink);
    Pdf next(d.ctx,Call(d.ctx,[&]{return pdf_create_document(d.ctx);}));
    PrepareInfoDictionary(d.ctx,next.get());
    {
        Object page(d.ctx,NewImagePage(d.ctx,next.get(),img.get(),width,height,0,0.75f*shrink));
        Call(d.ctx,[&]{pdf_insert_page(d.ctx,next.get(),-1,page.get());});
    }
    Call(d.ctx,[&]{pdf_enable_journal(d.ctx,next.get());});
    d.ClearSelection();d.Invalidate();if(d.doc)pdf_drop_document(d.ctx,d.doc);
    d.doc=std::exchange(next.p,nullptr);d.bytes.clear();d.openedPassword.clear();d.dirty=true;
}
void Document::InsertImagePage(int after,const fs::path& image){
    auto& d=*impl_;d.Require();const auto name=PathString(image);
    Image img(d.ctx,Call(d.ctx,[&]{return fz_new_image_from_file(d.ctx,name.c_str());}));
    if(img.p->w<=0||img.p->h<=0)throw std::runtime_error("Invalid image dimensions");
    const int count=d.Count();const int index=std::clamp(after+1,0,count);
    float width=595.28f,height=841.89f;
    if(count>0){const auto info=Info();const auto& ref=info.pages[static_cast<size_t>(std::clamp(after,0,count-1))];width=ref.width;height=ref.height;}
    if(std::abs(width-height)>1&&(img.p->w>img.p->h)!=(width>height))std::swap(width,height);
    const float pad=std::min(width,height)*0.04f;
    d.Edit("Insert image page",[&]{
        Object page(d.ctx,NewImagePage(d.ctx,d.doc,img.get(),width,height,pad,0.75f));
        Call(d.ctx,[&]{pdf_insert_page(d.ctx,d.doc,index,page.get());});
    });
}
void Document::ImageToPdf(const fs::path& image,const fs::path& output,bool a4,const Cancel& cancel,const ProgressSink& progress){
    CheckCancel(cancel);ReportProgress(progress,ProgressStage::ImageDecode,0,1);CheckCancel(cancel);
    Document context;auto& d=*context.impl_;
    d.doc=Call(d.ctx,[&]{return pdf_create_document(d.ctx);});
    const auto name=PathString(image);
    Image img(d.ctx,Call(d.ctx,[&]{return fz_new_image_from_file(d.ctx,name.c_str());}));
    if(img.p->w<=0||img.p->h<=0)throw std::runtime_error("Invalid image dimensions");
    float width=a4?595.28f:static_cast<float>(img.p->w)*0.75f;
    float height=a4?841.89f:static_cast<float>(img.p->h)*0.75f;
    if(width>14400||height>14400)throw std::runtime_error("Image page is too large; choose A4");
    const float pad=a4?28.0f:0.0f;
    const float scale=std::min((width-2*pad)/img.p->w,(height-2*pad)/img.p->h);
    const float w=img.p->w*scale,h=img.p->h*scale;
    const auto bounds=fz_make_rect(0,0,width,height);
    Object resources(d.ctx);Buffer contents(d.ctx);
    Device dev(d.ctx,Call(d.ctx,[&]{return pdf_page_write(d.ctx,d.doc,bounds,&resources.p,&contents.p);}));
    Call(d.ctx,[&]{fz_fill_image(d.ctx,dev.get(),img.get(),fz_make_matrix(w,0,0,h,(width-w)/2,(height-h)/2),1,fz_default_color_params);fz_close_device(d.ctx,dev.get());});
    Object page(d.ctx,Call(d.ctx,[&]{return pdf_add_page(d.ctx,d.doc,bounds,0,resources.get(),contents.get());}));
    Call(d.ctx,[&]{pdf_insert_page(d.ctx,d.doc,-1,page.get());});
    ReportProgress(progress,ProgressStage::ImageDecode,1,1);CheckCancel(cancel);
    context.Save(output,cancel,progress);
}
void Document::Merge(const std::vector<MergeInput>& inputs,const fs::path& output,bool bookmarks,const Cancel& cancel,const std::function<void(int,int)>& progress,const ProgressSink& detail){
    if(inputs.empty())throw std::runtime_error("Add files to merge");
    Document merged;auto& d=*merged.impl_;d.doc=Call(d.ctx,[&]{return pdf_create_document(d.ctx);});
    std::vector<std::pair<std::string,int>> titles;
    std::vector<std::vector<int>> selections;selections.reserve(inputs.size());
    uint64_t totalPages=0,donePages=0;
    auto report=[&](ProgressStage phase,size_t index,uint64_t localDone=0,uint64_t localTotal=0){
        if(!detail)return;
        OperationProgress event;event.stage=phase;event.fileIndex=index;event.fileCount=inputs.size();
        event.completed=donePages;event.total=totalPages;event.fileCompleted=localDone;event.fileTotal=localTotal;detail(event);
    };
    // Inspect selected page counts first, so page progress has a truthful total.
    // Documents are closed between passes rather than retaining every input.
    for(size_t i=0;i<inputs.size();++i){
        report(ProgressStage::Inspecting,i);CheckCancel(cancel);
        const auto name=PathString(inputs[i].path),password=Utf8(inputs[i].password);
        Pdf input(d.ctx,Call(d.ctx,[&]{return pdf_open_document(d.ctx,name.c_str());}));
        if(Call(d.ctx,[&]{return pdf_needs_password(d.ctx,input.get());})&&!Call(d.ctx,[&]{return pdf_authenticate_password(d.ctx,input.get(),password.c_str());}))throw PasswordRequired{};
        const int count=Call(d.ctx,[&]{return pdf_count_pages(d.ctx,input.get());});
        auto order=ParsePageRange(inputs[i].range,count);
        totalPages+=order.size();if(totalPages>static_cast<uint64_t>(INT_MAX))throw std::runtime_error("Merged document exceeds the page limit");
        selections.push_back(std::move(order));report(ProgressStage::Inspecting,i,0,selections.back().size());
    }
    int processed=0;
    for(size_t index=0;index<inputs.size();++index){
        const auto& source=inputs[index];
        CheckCancel(cancel);const auto name=PathString(source.path),password=Utf8(source.password);
        Pdf input(d.ctx,Call(d.ctx,[&]{return pdf_open_document(d.ctx,name.c_str());}));
        if(Call(d.ctx,[&]{return pdf_needs_password(d.ctx,input.get());})&&!Call(d.ctx,[&]{return pdf_authenticate_password(d.ctx,input.get(),password.c_str());}))throw PasswordRequired{};
        const auto& order=selections[index];
        titles.emplace_back(Utf8(source.path.stem().wstring()),d.Count());
        report(ProgressStage::Flattening,index,0,order.size());CheckCancel(cancel);
        BakeVisible(d.ctx,input.get());
        Held<pdf_graft_map,pdf_drop_graft_map> map(d.ctx,Call(d.ctx,[&]{return pdf_new_graft_map(d.ctx,d.doc);}));
        uint64_t local=0;report(ProgressStage::MergingPages,index,0,order.size());
        for(int page:order){
            CheckCancel(cancel);Call(d.ctx,[&]{pdf_graft_mapped_page(d.ctx,map.get(),-1,input.get(),page);});
            ++donePages;++local;report(ProgressStage::MergingPages,index,local,order.size());
        }
        if(progress)progress(++processed,static_cast<int>(inputs.size()));
    }
    if(bookmarks){
        report(ProgressStage::Bookmarks,NoProgressFile);CheckCancel(cancel);
        Object outline(d.ctx,Call(d.ctx,[&]{return pdf_add_new_dict(d.ctx,d.doc,4);}));
        std::vector<Object> items;items.reserve(titles.size());
        for(const auto& entry:titles){
            Object item(d.ctx,Call(d.ctx,[&]{return pdf_add_new_dict(d.ctx,d.doc,5);}));
            Call(d.ctx,[&]{
                pdf_dict_put_text_string(d.ctx,item.get(),PDF_NAME(Title),entry.first.c_str());
                pdf_dict_put(d.ctx,item.get(),PDF_NAME(Parent),outline.get());
                auto* dest=pdf_dict_put_array(d.ctx,item.get(),PDF_NAME(Dest),2);
                pdf_array_push(d.ctx,dest,pdf_lookup_page_obj(d.ctx,d.doc,entry.second));pdf_array_push(d.ctx,dest,PDF_NAME(Fit));
            });items.push_back(std::move(item));
        }
        for(size_t i=0;i<items.size();++i)Call(d.ctx,[&]{
            if(i)pdf_dict_put(d.ctx,items[i].get(),PDF_NAME(Prev),items[i-1].get());
            if(i+1<items.size())pdf_dict_put(d.ctx,items[i].get(),PDF_NAME(Next),items[i+1].get());
        });
        Call(d.ctx,[&]{
            pdf_dict_put(d.ctx,outline.get(),PDF_NAME(First),items.front().get());pdf_dict_put(d.ctx,outline.get(),PDF_NAME(Last),items.back().get());
            pdf_dict_put_int(d.ctx,outline.get(),PDF_NAME(Count),static_cast<int>(items.size()));
            pdf_dict_put(d.ctx,pdf_dict_get(d.ctx,pdf_trailer(d.ctx,d.doc),PDF_NAME(Root)),PDF_NAME(Outlines),outline.get());
        });
    }
    CheckCancel(cancel);
    merged.Save(output,cancel,[&](const OperationProgress& event){report(event.stage,NoProgressFile);});
    report(ProgressStage::Complete,NoProgressFile);
}
}








