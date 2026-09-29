// 真正的涂黑（安全遮盖）：框选 / 选中文字 / 搜索结果 → 标记（仅在程序内，预览）→ 应用并另存副本。
// 应用在工作线程执行：当前文档与原文件都不修改；输出前重新打开逐个标记校验，失败不留下文件。
#include "application.h"
#include "ui.h"
#include "recent_files.h"
#include <lumen/Dialog.h>
#include <lumen/Menu.h>
#include <algorithm>
#include <chrono>
#include <thread>

namespace lpdf {
using namespace lumen;
namespace {
Rect QuadBox(const Quad& q){
    const float x0=std::min({q.ul.x,q.ur.x,q.ll.x,q.lr.x}),x1=std::max({q.ul.x,q.ur.x,q.ll.x,q.lr.x});
    const float y0=std::min({q.ul.y,q.ur.y,q.ll.y,q.lr.y}),y1=std::max({q.ul.y,q.ur.y,q.ll.y,q.lr.y});
    return {x0,y0,x1-x0,y1-y0};
}
// 文字框四周略放大，避免字形边缘（下伸部、重音符）落在框外。
Rect Pad(Rect r,float k=1.0f){return {r.x-k,r.y-k,r.w+2*k,r.h+2*k};}
bool SameGeometry(const std::vector<PageInfo>& a,const std::vector<PageInfo>& b){
    if(a.size()!=b.size())return false;
    for(size_t i=0;i<a.size();++i)if(a[i].width!=b[i].width||a[i].height!=b[i].height||a[i].originX!=b[i].originX||a[i].originY!=b[i].originY)return false;
    return true;
}
}

void Application::EnterRedaction(){
    if(!loaded_||busy_)return;
    if(textEditor_.Active()){FinishText(true,[this]{EnterRedaction();});return;}
    if(mode_!=0)Mode(0);
    if(canvas_->HandTool())SetHand(false);
    ChooseTool(Tool::Select);
    if(redactMarks_.empty()){redactDoc_=activeDocument_?activeDocument_->id:0;redactGeometry_=info_.pages;}
    redacting_=true;canvas_->RedactMode(true);canvas_->RedactMarks(redactMarks_);
    RedactionStatus(L"涂黑模式：在页面上拖动框选要永久删除的内容");
}
void Application::RedactionStatus(std::wstring lead){
    std::wstring text=std::move(lead);
    text+=L"  ·  已标记 "+std::to_wstring(redactMarks_.size())+L" 处";
    text+=redactMarks_.empty()?L"  ·  也可右键选中文字或用搜索结果标记":L"  ·  单击标记可删除  ·  更多 → 应用涂黑";
    text+=L"  ·  Esc 退出";
    status_->Text(text);
}
void Application::ExitRedaction(bool discard){
    if(!redacting_&&redactMarks_.empty())return;
    if(!discard&&!redactMarks_.empty()){
        window_.Confirm(L"退出涂黑模式？",std::to_wstring(redactMarks_.size())+L" 处标记还没有应用，退出后将丢弃（文档本身没有任何改动）。",
            [this](bool yes){if(yes)ExitRedaction(true);},L"丢弃并退出",L"继续标记");
        return;
    }
    redactMarks_.clear();redacting_=false;
    canvas_->RedactMode(false);canvas_->RedactMarks({});
    status_->Text(L"已退出涂黑模式");
}
void Application::AddRedactions(std::vector<RedactionMark> marks,const wchar_t* what){
    if(marks.empty()){status_->Text(L"没有可标记的内容");return;}
    if(!redacting_)EnterRedaction();
    if(!redacting_)return;
    size_t added=0;
    for(auto& m:marks){
        if(m.page<0||m.page>=static_cast<int>(info_.pages.size())||m.bounds.w<1||m.bounds.h<1)continue;
        // 与已有标记完全相同的跳过（重复标记搜索结果时）。
        const bool dup=std::any_of(redactMarks_.begin(),redactMarks_.end(),[&](const RedactionMark& o){
            return o.page==m.page&&std::abs(o.bounds.x-m.bounds.x)<.5f&&std::abs(o.bounds.y-m.bounds.y)<.5f&&std::abs(o.bounds.w-m.bounds.w)<.5f&&std::abs(o.bounds.h-m.bounds.h)<.5f;});
        if(!dup){redactMarks_.push_back(m);++added;}
    }
    canvas_->RedactMarks(redactMarks_);
    RedactionStatus(std::wstring(L"已标记")+what+L" "+std::to_wstring(added)+L" 处");
}
void Application::RemoveRedaction(size_t index){
    if(index>=redactMarks_.size())return;
    redactMarks_.erase(redactMarks_.begin()+static_cast<ptrdiff_t>(index));
    canvas_->RedactMarks(redactMarks_);
    RedactionStatus(L"已删除 1 处标记");
}
void Application::RedactSelection(const PdfCanvas::TextSelection& selection){
    std::vector<RedactionMark> marks;
    if(!selection.spans.empty()){for(const auto& span:selection.spans)for(const auto& q:span.quads)marks.push_back({span.page,Pad(QuadBox(q))});}
    else for(const auto& q:selection.quads)marks.push_back({selection.page,Pad(QuadBox(q))});
    canvas_->ClearTextSelection();
    AddRedactions(std::move(marks),L"选中文字");
}
void Application::RedactSearchHits(){
    std::vector<RedactionMark> marks;
    for(const auto& h:hits_)if(h.annotation<0)marks.push_back({h.page,Pad(h.bounds)});
    const size_t inAnnotations=std::count_if(hits_.begin(),hits_.end(),[](const SearchHit& h){return h.annotation>=0;});
    AddRedactions(std::move(marks),L"搜索结果");
    if(inAnnotations)status_->Text(std::wstring(status_->Text())+L"  ·  另有 "+std::to_wstring(inAnnotations)+L" 处位于批注中：与涂黑区重叠的批注会被删除");
}
// 页数或页面尺寸变化（插入 / 删除 / 旋转 / 裁剪页面、切换文档）后，标记位置不再可信：直接丢弃。
void Application::CheckRedactionMarks(){
    if(!redacting_&&redactMarks_.empty())return;
    const auto doc=activeDocument_?activeDocument_->id:0;
    if(!loaded_||doc!=redactDoc_||!SameGeometry(info_.pages,redactGeometry_)){
        const bool had=!redactMarks_.empty();
        ExitRedaction(true);
        if(had)status_->Text(L"页面结构已变化，涂黑标记已清除，请重新标记");
    }
}

void Application::ShowRedactionDialog(){
    if(!loaded_||busy_||window_.DialogActive())return;
    if(redactMarks_.empty()){status_->Text(L"还没有涂黑标记：在页面上拖动框选要删除的内容");return;}
    struct Controls{CheckBox* images{};CheckBox* lineArt{};CheckBox* metadata{};};
    auto c=std::make_shared<Controls>();
    std::vector<int> pages;for(const auto& m:redactMarks_)pages.push_back(m.page);
    std::sort(pages.begin(),pages.end());pages.erase(std::unique(pages.begin(),pages.end()),pages.end());
    DialogSpec dialog;dialog.title=L"应用涂黑并另存副本";
    dialog.message=L"将在 "+std::to_wstring(pages.size())+L" 页上永久删除 "+std::to_wstring(redactMarks_.size())+L" 处区域下的文字、图片与图形，并画上黑块。"
        L"与涂黑区重叠的批注会被删除，重叠的表单字段会被清空。\n结果另存为新文件，当前文档和原文件都不会改动；涂黑后的内容无法从副本中恢复。";
    dialog.size=DialogSize::Standard;
    dialog.content=[c](Panel& panel){
        c->images=&panel.Add<CheckBox>(L"与涂黑区相交的图片整张删除（默认只涂黑被覆盖的像素）").Checked(false);
        c->lineArt=&panel.Add<CheckBox>(L"删除被涂黑区完全覆盖的线条与形状").Checked(true);
        c->metadata=&panel.Add<CheckBox>(L"同时清除文档属性（标题、作者、主题、关键词）").Checked(true);
    };
    dialog.primary={L"选择保存位置…",{}};dialog.close={L"取消",{}};
    dialog.on_result=[this,c](DialogResult result){
        if(result!=DialogResult::Primary)return;
        RedactionOptions options;options.removeWholeImages=c->images->Checked();options.removeLineArt=c->lineArt->Checked();options.clearMetadata=c->metadata->Checked();
        // 系统保存对话框在自定义对话框关闭后再弹出。
        AfterDialog([this,options]{
            auto stem=source_.empty()?std::wstring(L"文档"):source_.stem().wstring();
            const auto dest=Destination(stem+L"-已涂黑.pdf");
            if(dest.empty())return;
            if(!source_.empty()&&RecentFiles::SamePath(dest,source_)){window_.Alert(L"不能覆盖原文件",L"涂黑结果必须另存为新文件，请换一个文件名。");return;}
            ExportRedaction(dest,options);
        });
    };
    window_.ShowDialog(std::move(dialog));
}
void Application::ExportRedaction(const fs::path& dest,RedactionOptions options,std::function<void()> after){
    if(!loaded_||busy_||redactMarks_.empty())return;
    const auto marks=redactMarks_;
    auto result=std::make_shared<RedactionResult>();
    keepSearch_=true;
    Task(L"涂黑并另存",[marks,options,dest,result](Engine& e,const Cancel& cancel){*result=e.document.ExportRedacted(marks,options,dest,cancel);},
        [this,dest,result,after]{
            ExitRedaction(true);
            std::wstring detail=L"已生成涂黑副本 "+dest.filename().wstring()+L"：删除 "+std::to_wstring(result->marks)+L" 处";
            if(result->annotationsRemoved)detail+=L"，移除批注 "+std::to_wstring(result->annotationsRemoved)+L" 个";
            if(result->fieldsCleared)detail+=L"，清空表单字段 "+std::to_wstring(result->fieldsCleared)+L" 个";
            status_->Text(detail);
            if(after){after();return;}
            window_.Confirm(L"涂黑副本已保存",detail+L"。\n已重新打开副本校验：涂黑区域内没有残留可提取的文字。现在打开副本查看吗？",
                [this,dest](bool yes){if(yes)OpenFiles({dest});},L"打开副本",L"稍后");
        },{},[this](std::wstring error){
            window_.Alert(L"涂黑失败",L"没有生成输出文件，当前文档与原文件均未改动。\n"+error);
        });
}

// 自动化冒烟（--smoke 且 LPDF_REDACT_SMOKE=<输出 PDF>）：搜索 → 标记全部结果 → 框选回调 → 删除一个标记 → 应用并另存。
void Application::RedactionSmoke(const fs::path& output,int step){
    auto later=[this](float delay,std::function<void()> fn){
        auto post=window_.Dispatcher();auto alive=alive_;
        std::thread([post,alive,delay,fn]{std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(delay*1000)));post.Post([alive,fn]{if(alive->load())fn();});}).detach();
    };
    auto next=[this,later,output,step](float delay){later(delay,[this,output,step]{RedactionSmoke(output,step+1);});};
    auto fail=[this,output](const wchar_t* why){
        FILE* file=nullptr;if(_wfopen_s(&file,(output.wstring()+L".fail.txt").c_str(),L"w, ccs=UTF-8")==0&&file){fwprintf(file,L"%ls | 状态栏：%ls\n",why,std::wstring(status_->Text()).c_str());fclose(file);}
        closing_=true;window_.Close();
    };
    if(!loaded_||busy_){later(.3f,[this,output,step]{RedactionSmoke(output,step);});return;}
    switch(step){
    case 0:search_->Text(L"SECRET");Search();return next(1.5f);
    case 1:{
        if(hits_.size()!=2){fail(L"搜索结果数量不是 2");return;}
        RedactSearchHits();
        if(!redacting_||redactMarks_.size()!=2){fail(L"标记搜索结果");return;}
        RedactSearchHits();   // 重复标记应被去重
        if(redactMarks_.size()!=2){fail(L"重复标记未去重");return;}
        // 画布框选回调：多加一个空白区域标记，再删除它。
        if(!canvas_->redact_add||!canvas_->redact_remove){fail(L"画布回调未连接");return;}
        canvas_->redact_add(0,{400,700,60,40});
        if(redactMarks_.size()!=3){fail(L"框选标记");return;}
        canvas_->redact_remove(2);
        if(redactMarks_.size()!=2){fail(L"删除标记");return;}
        ShowRedactionDialog();
        if(!window_.DialogActive()){fail(L"应用对话框未打开");return;}
        window_.CloseDialog();
        return next(.8f);
    }
    case 2:
        if(window_.DialogActive()){fail(L"对话框未关闭");return;}
        ExportRedaction(output,{},[this,later]{later(.5f,[this]{closing_=true;window_.Close();});});
        return;
    default:return;
    }
}
}
