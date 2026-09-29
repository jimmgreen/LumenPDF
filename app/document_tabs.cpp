#include "application.h"
#include "ui.h"
#include "app_log.h"
#include <lumen/Menu.h>
#include <lumen/Dialog.h>
#include <algorithm>
#include <fstream>
#include <cstring>
namespace lpdf {
using namespace lumen;
namespace {
uint64_t TabId(std::wstring_view value){try{return std::stoull(std::wstring(value));}catch(...){return 0;}}
void CopyText(HWND owner,const std::wstring& text){
    if(!OpenClipboard(owner))throw std::runtime_error("Clipboard is busy");
    const size_t bytes=(text.size()+1)*sizeof(wchar_t);HGLOBAL data=GlobalAlloc(GMEM_MOVEABLE,bytes);
    if(!data){CloseClipboard();throw std::bad_alloc();}
    auto* memory=GlobalLock(data);if(!memory){GlobalFree(data);CloseClipboard();throw std::runtime_error("Clipboard allocation failed");}
    memcpy(memory,text.c_str(),bytes);GlobalUnlock(data);
    EmptyClipboard();if(!SetClipboardData(CF_UNICODETEXT,data))GlobalFree(data);CloseClipboard();
}
}
std::shared_ptr<Application::DocumentSession> Application::FindDocument(uint64_t id)const{
    for(const auto& doc:documents_)if(doc->id==id)return doc;return {};
}
void Application::CaptureDocument(){
    if(!activeDocument_)return;auto& s=*activeDocument_;
    s.worker=worker_;s.info=info_;s.annotations=annotations_;s.source=source_;s.recovery=recoveryFile_;s.restored=restoredFile_;
    s.preview=preview_;s.previewFiles=previewFiles_;s.title=name_->Text();s.view=canvas_->CurrentView();s.page=page_;s.annotation=annotation_;
    s.mode=mode_==3?0:mode_;s.side=sidePanel_;s.layout=canvas_->Spread()?(canvas_->SpreadCover()?1:2):0;
    s.hand=canvas_->HandTool();s.hideAnnotations=canvas_->AnnotationsHidden();s.query=search_->Text();s.lastQuery=lastQuery_;s.hits=hits_;s.hit=hit_;s.searchOptions=searchOptions_;
    s.back=back_;s.forward=forward_;
}
bool Application::BeginDocument(){
    if(busy_||window_.DialogActive()||textEditor_.Active())return false;
    if(activeDocument_&&!loaded_)return true;
    if(documents_.size()>=64){Fail(L"最多同时打开 64 个标签页，请先关闭部分文件。");return false;}
    SavePosition();CaptureDocument();SaveReferencePosition();
    if(searchCancel_)searchCancel_->store(true);autoScroll_=false;++*generation_;
    switchingDocuments_=true;canvas_->DocumentPages({},generation_->load());thumbs_->DocumentPages({},generation_->load());
    auto s=std::make_shared<DocumentSession>();s->id=nextDocumentId_++;s->returnTo=activeDocument_?activeDocument_->id:0;s->worker=activeDocument_?std::make_shared<Worker>():worker_;s->title=L"新标签页";
    if(!recoveryRoot_.empty())s->recovery=UniquePath(recoveryRoot_,L".pdf");
    documents_.push_back(s);activeDocument_=s;worker_=s->worker;
    info_={};annotations_.clear();source_.clear();recoveryFile_=s->recovery;restoredFile_.clear();preview_.reset();previewFiles_=0;
    page_=0;annotation_=-1;hit_=-1;loaded_=false;home_=false;pendingSelect_.reset();back_.clear();forward_.clear();hits_.clear();lastQuery_.clear();search_->Text(L"");
    annotRows_.clear();allAnnotRows_.clear();annotList_->ItemCount(0,false);annotRowsGeneration_=~uint64_t{};annotRowsLoading_=false;
    searchResults_->ItemCount(0,false);name_->Text(s->title);canvas_->HideAnnotations(false);thumbs_->HideAnnotations(false);canvas_->HandTool(false);
    syncingTabs_=true;documentTabs_->AddTab(TabItem{.id=std::to_wstring(s->id),.title=s->title,.closable=true});documentTabs_->SelectedId(std::to_wstring(s->id));syncingTabs_=false;
    switchingDocuments_=false;Refresh({}, {},0);Mode(0);SetBusy(false);return true;
}
void Application::NewDocument(){
    if(textEditor_.Active()){FinishText(true,[this]{NewDocument();});return;}
    if(!BeginDocument())return;
    Task(L"创建文档",[](Engine& e,const Cancel&){e.document.New();e.open=true;},[this]{name_->Text(L"未命名.pdf");home_=false;Mode(0);canvas_->FitPage();UpdateTitle();});
}
void Application::ActivateDocument(uint64_t id){
    if(busy_||window_.DialogActive())return;
    auto target=FindDocument(id);if(!target)return;
    if(activeDocument_==target){home_=false;Mode(mode_==3?0:mode_);return;}
    StopReading(true);   // 朗读跟随文档：切换标签即停止
    if(textEditor_.Active()){FinishText(true,[this,id]{ActivateDocument(id);});return;}
    SavePosition();CaptureDocument();SaveReferencePosition();
    const uint64_t previous=activeDocument_?activeDocument_->id:0;
    if(referenceId_==id)referenceId_=previous;
    if(searchCancel_)searchCancel_->store(true);autoScroll_=false;++*generation_;
    switchingDocuments_=true;
    // Never reuse even identical-size page tiles/text across different documents.
    canvas_->DocumentPages({},generation_->load());thumbs_->DocumentPages({},generation_->load());
    activeDocument_=target;worker_=target->worker;const auto s=*target;
    source_=s.source;recoveryFile_=s.recovery;restoredFile_=s.restored;preview_=s.preview;previewFiles_=s.previewFiles;
    annotation_=s.annotation;page_=s.page;name_->Text(s.title);back_=s.back;forward_=s.forward;pendingSelect_.reset();linkStatus_.clear();
    annotRows_.clear();allAnnotRows_.clear();annotList_->ItemCount(0,false);annotRowsGeneration_=~uint64_t{};annotRowsLoading_=false;
    home_=false;Refresh(s.info,s.annotations,s.page);Mode(s.mode);ApplyLayout(s.layout,false);canvas_->HideAnnotations(s.hideAnnotations);thumbs_->HideAnnotations(s.hideAnnotations);canvas_->HandTool(s.hand);
    search_->Text(s.query);lastQuery_=s.lastQuery;hits_=s.hits;hit_=s.hit;searchOptions_=s.searchOptions;searchResults_->ItemCount(hits_.size(),false);canvas_->SearchResults(hits_,hit_,false);SyncSearchButtons();
    hitLabel_->Text(hits_.empty()?L"":std::to_wstring(hits_.size())+L" 处");
    sideTabs_->SelectedIndex(s.side);SidePanel(s.side);canvas_->RestoreView(s.view);thumbs_->GoTo(s.page);ShowPageNumber(s.page);
    syncingTabs_=true;documentTabs_->SelectedId(std::to_wstring(id));syncingTabs_=false;
    switchingDocuments_=false;Page(s.page,false);SetBusy(false,ContextHint());UpdateTitle();if(splitMode_)LoadReference();
}
void Application::CycleDocument(int direction){
    if(busy_||window_.DialogActive()||!documentTabs_->TabCount())return;
    const int count=static_cast<int>(documentTabs_->TabCount());const int next=(documentTabs_->SelectedIndex()+direction+count)%count;
    ActivateDocument(TabId(documentTabs_->Tab(static_cast<size_t>(next)).id));
}
void Application::SyncDocumentTabs(){
    if(!documentTabs_)return;
    if(activeDocument_){activeDocument_->title=name_->Text();activeDocument_->source=source_;activeDocument_->info=info_;activeDocument_->preview=preview_;activeDocument_->previewFiles=previewFiles_;}
    for(const auto& doc:documents_)documentTabs_->Title(std::to_wstring(doc->id),(doc->info.dirty?L"● ":L"")+doc->title);
    documentBar_->Visible(!documents_.empty()&&!presenting_);documentTabs_->Enabled(!busy_);splitButton_->Enabled(!busy_&&loaded_);
    primaryCaption_->Text(L"正在编辑："+(loaded_?name_->Text():std::wstring{}));primaryCaption_->Visible(splitMode_!=0);
    if(referenceTools_)referenceTools_->Enabled(!busy_);
}
void Application::CloseDocument(uint64_t id){
    if(busy_||window_.DialogActive())return;if(!FindDocument(id))return;
    if(textEditor_.Active()){FinishText(true,[this,id]{CloseDocument(id);});return;}
    ActivateDocument(id);if(!activeDocument_||activeDocument_->id!=id)return;
    if(aloud_.active&&aloud_.document==id)StopReading(true);
    Guard([this,id]{DropDocument(id);});
}
void Application::DropDocument(uint64_t id){
    auto doc=FindDocument(id);if(!doc||busy_)return;
    const bool active=activeDocument_==doc;
    uint64_t next=0;for(size_t i=0;i<documentTabs_->TabCount();++i)if(TabId(documentTabs_->Tab(i).id)==id){
        if(i+1<documentTabs_->TabCount())next=TabId(documentTabs_->Tab(i+1).id);else if(i)next=TabId(documentTabs_->Tab(i-1).id);break;
    }
    if(active&&doc->info.pages.empty()&&FindDocument(doc->returnTo))next=doc->returnTo;
    if(active){SavePosition();CaptureDocument();if(searchCancel_)searchCancel_->store(true);++*generation_;switchingDocuments_=true;canvas_->DocumentPages({},generation_->load());thumbs_->DocumentPages({},generation_->load());switchingDocuments_=false;activeDocument_.reset();}
    if(referenceId_==id){SaveReferencePosition();referenceId_=0;++*referenceGeneration_;referenceCanvas_->DocumentPages({},referenceGeneration_->load());}
    doc->worker->Stop();retiredWorkers_.push_back({doc->worker,doc->recovery});
    documents_.erase(std::remove(documents_.begin(),documents_.end(),doc),documents_.end());
    syncingTabs_=true;documentTabs_->CloseTab(std::to_wstring(id));syncingTabs_=false;
    if(active){
        info_={};annotations_.clear();source_.clear();recoveryFile_.clear();restoredFile_.clear();preview_.reset();previewFiles_=0;loaded_=false;name_->Text(L"");hits_.clear();lastQuery_.clear();back_.clear();forward_.clear();
        if(next&&FindDocument(next))ActivateDocument(next);
        else{worker_=std::make_shared<Worker>();SetSplit(0);Refresh({}, {},0);Mode(0);Home(true);}
    }
    if(documents_.size()<2)SetSplit(0);else if(splitMode_&&!referenceId_)SetSplit(splitMode_);
    SyncDocumentTabs();
}
void Application::ConfirmWorkspaceExit(){
    if(busy_||window_.DialogActive())return;CaptureDocument();std::vector<uint64_t> ids;for(const auto& doc:documents_)ids.push_back(doc->id);ConfirmWorkspaceExitAt(std::move(ids),0);
}
void Application::ConfirmWorkspaceExitAt(std::vector<uint64_t> ids,size_t index){
    if(index>=ids.size()){closing_=true;window_.Close();return;}
    auto doc=FindDocument(ids[index]);if(!doc||!doc->info.dirty){ConfirmWorkspaceExitAt(std::move(ids),index+1);return;}
    ActivateDocument(doc->id);
    Guard([this,ids=std::move(ids),index]{window_.Dispatcher().Post([this,ids,index]{ConfirmWorkspaceExitAt(ids,index+1);});});
}
void Application::OpenFiles(std::vector<fs::path> files){
    for(auto& file:files)pendingOpen_.push_back(std::move(file));DrainOpenFiles();
}
void Application::DrainOpenFiles(){
    if(openDrainScheduled_||pendingOpen_.empty()||closing_)return;
    openDrainScheduled_=true;window_.SetTimeout(.12f,[this]{
        openDrainScheduled_=false;if(pendingOpen_.empty()||closing_)return;
        if(!busy_&&!window_.DialogActive()&&!textEditor_.Active()){auto path=std::move(pendingOpen_.front());pendingOpen_.pop_front();Open(path);}
        DrainOpenFiles();
    });
}
void Application::SaveReferencePosition(){
    if(!splitMode_||referenceInfo_.pages.empty())return;
    if(auto doc=FindDocument(referenceId_)){doc->view=referenceCanvas_->CurrentView();if(doc->page!=doc->view.page){doc->annotations.clear();doc->annotation=-1;}doc->page=doc->view.page;
        if(!doc->source.empty()){ReadingPosition p;p.path=doc->source;p.page=doc->view.page;p.offset=doc->view.offset;p.fit=static_cast<int>(doc->view.fit);p.zoom=doc->view.zoom;p.saved=UnixNow();positions_.Remember(std::move(p));}}
}
void Application::SetSplit(int mode,uint64_t reference){
    if(busy_||window_.DialogActive())return;
    if(textEditor_.Active()){FinishText(true,[this,mode,reference]{SetSplit(mode,reference);});return;}
    SaveReferencePosition();mode=std::clamp(mode,0,2);
    if(mode&&documents_.size()<2){auto files=Pick(false);if(files.empty())return;pendingSplitMode_=mode;OpenFiles(std::move(files));return;}
    if(mode){
        if(reference&&FindDocument(reference)&&activeDocument_&&reference!=activeDocument_->id)referenceId_=reference;
        if(!FindDocument(referenceId_)||(activeDocument_&&referenceId_==activeDocument_->id)){
            referenceId_=0;for(const auto& doc:documents_)if(doc!=activeDocument_&&!doc->info.pages.empty()){referenceId_=doc->id;break;}
        }
        if(!referenceId_)return;
    }
    splitMode_=mode;splitBody_->LayoutMode(mode);referencePane_->Visible(mode!=0);primaryCaption_->Visible(mode!=0);
    if(mode){sidebar_->Visible(false);home_=false;if(mode_==3)Mode(0);LoadReference();}
    else{++*referenceGeneration_;referenceCanvas_->DocumentPages({},referenceGeneration_->load());referenceInfo_={};referenceId_=0;}
    SyncDocumentTabs();
}
void Application::ShowSplitMenu(){
    Menu menu;menu.AddItem(L"不分屏",[this]{SetSplit(0);}).Checked(splitMode_==0);
    menu.AddItem(L"左右分屏",[this]{SetSplit(1);}).Checked(splitMode_==1);
    menu.AddItem(L"上下分屏",[this]{SetSplit(2);}).Checked(splitMode_==2);
    menu.AddItem(L"恢复均分",[this]{splitBody_->Equalize();}).Disabled(!splitMode_);
    menu.AddItem(L"选择对照文件…",[this]{ChooseReference();}).Disabled(!splitMode_);
    menu.PopupTo(*splitButton_);
}
void Application::ChooseReference(){
    if(busy_)return;Menu menu;
    for(const auto& doc:documents_)if(doc!=activeDocument_&&!doc->info.pages.empty())menu.AddItem((doc->info.dirty?L"● ":L"")+doc->title,[this,id=doc->id]{SetSplit(splitMode_?splitMode_:1,id);}).Checked(doc->id==referenceId_);
    menu.AddSeparator();menu.AddItem(L"打开另一文件…",[this]{auto paths=Pick(false);if(!paths.empty()){pendingSplitMode_=splitMode_?splitMode_:1;OpenFiles(std::move(paths));}});menu.PopupTo(*referenceTitle_);
}
void Application::LoadReference(bool preserve){
    if(!splitMode_)return;auto doc=FindDocument(referenceId_);if(!doc||doc==activeDocument_)return;
    const auto state=preserve?referenceCanvas_->CurrentView():doc->view;
    ++*referenceGeneration_;const auto gen=referenceGeneration_->load();referenceInfo_=doc->info;
    referenceCanvas_->DocumentPages({},gen);referenceCanvas_->DocumentPages(doc->info.pages,gen);referenceCanvas_->Spread(doc->layout!=0,doc->layout==1);referenceCanvas_->HideAnnotations(doc->hideAnnotations);
    referenceCanvas_->RestoreView(state);referenceTitle_->Text(L"对照："+doc->title);referenceTitle_->ToolTip(doc->source.wstring());referencePage_->Text(std::to_wstring(state.page+1));referenceCount_->Text(L"/ "+std::to_wstring(doc->info.pages.size()));
}
void Application::WireReference(){
    auto alive=alive_;auto post=window_.Dispatcher();auto generation=referenceGeneration_;auto* c=referenceCanvas_;
    c->request_tile=[this,c,alive,post,generation](TileRequest request){
        auto doc=FindDocument(referenceId_);if(!doc)return;const bool hidden=c->AnnotationsHidden();
        doc->worker->Submit([c,alive,post,generation,request,hidden](Engine& e){
            if(!alive->load()||generation->load()!=request.generation||!e.open||(request.cancelled&&request.cancelled->load()))return;
            try{auto bitmap=e.document.Render(request.key.page,request.scale,request.clip,-1,false,hidden);ApplyTone(request.tone,bitmap.bgra,bitmap.width,bitmap.height,bitmap.stride);
                post.Post([c,alive,generation,request,bitmap=std::move(bitmap)]()mutable{if(alive->load()&&generation->load()==request.generation)c->AcceptTile(request,std::move(bitmap));});
            }catch(...){post.Post([c,alive,generation,request]{if(alive->load()&&generation->load()==request.generation)c->FailTile(request.key);});}
        },false);
    };
    c->page_changed=[this](int page){if(splitMode_)referencePage_->Text(std::to_wstring(page+1));};
    c->request_highlight=[this,c,alive,post,generation](HighlightRequest request){
        auto doc=FindDocument(referenceId_);if(!doc)return;doc->worker->Submit([c,alive,post,generation,request](Engine& e){
            if(!alive->load()||generation->load()!=request.generation||request.cancelled->load()||!e.open)return;
            try{auto resolved=request;auto quads=ResolveHighlight(e.document,resolved);post.Post([c,alive,generation,resolved,quads=std::move(quads)]()mutable{if(alive->load()&&generation->load()==resolved.generation)c->AcceptHighlight(resolved,std::move(quads));});}catch(...){}
        },false);
    };
    c->copy_selection=[this](auto selection){CopyReference(selection);};
    c->context_menu=[this](lumen::Point point){Menu menu;menu.AddItem(L"复制对照窗格所选文字",[this]{CopyReference(referenceCanvas_->Selected());}).Disabled(!referenceCanvas_->HasTextSelection());menu.AddItem(L"交换窗格，编辑此文档",[this]{ActivateDocument(referenceId_);}).Disabled(busy_);menu.Popup(window_,point);};
    c->files_dropped=[this](std::vector<fs::path> paths){pendingSplitMode_=splitMode_?splitMode_:1;OpenFiles(std::move(paths));};
    c->request_links=[this,c,alive,post,generation](int page,uint64_t gen){auto doc=FindDocument(referenceId_);if(!doc)return;doc->worker->Submit([c,alive,post,generation,page,gen](Engine& e){if(!alive->load()||generation->load()!=gen||!e.open)return;try{auto links=e.document.Links(page);post.Post([c,alive,generation,page,gen,links=std::move(links)]()mutable{if(alive->load()&&generation->load()==gen)c->AcceptLinks(page,gen,std::move(links));});}catch(...){}},false);};
    c->activate_link=[this](const Link& link){if(link.External())FollowLink(link);else referenceCanvas_->GoToPoint(link.page,link.targetY);};
}
void Application::CopyReference(PdfCanvas::TextSelection selection){
    auto doc=FindDocument(referenceId_);if(!doc||selection.page<0)return;auto alive=alive_;auto post=window_.Dispatcher();
    doc->worker->Submit([this,alive,post,selection](Engine& e){try{const auto text=SelectedText(e.document,selection);post.Post([this,alive,text]{if(alive->load())try{CopyText(static_cast<HWND>(window_.NativeHandle()),text);status_->Text(L"已复制对照窗格的文字");}catch(const std::exception& ex){Fail(Wide(ex.what()));}});}catch(...){} });
}
} // namespace lpdf

namespace lpdf {
// Automated workspace regression, only reachable with --smoke + LPDF_WORKSPACE_TEST.
// Uses test copies for all writes. No keyboard/mouse injection or registry changes.
void Application::WorkspaceSmokeTick(){
    if(workspaceSmokeStage_>=100)return;
    static fs::path fixtures,out;
    static std::vector<unsigned char> originalA,originalB;
    static std::ofstream report;
    auto check=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    try{
        if(++workspaceSmokeTicks_>650)throw std::runtime_error("workspace test timed out");
        if(workspaceSmokeStage_==16)report<<"tick16 busy="<<busy_<<" closing="<<closing_<<" dialog="<<window_.DialogActive()<<" status="<<Utf8(status_->Text())<<"\n"<<std::flush;
        if(busy_)return;
        // 对话框关闭动画期间 DialogActive() 仍为真；关闭对话框后的阶段先等动画结束（最多约 1.8 s）再判定。
        static int settleTicks=0;static int settleStage=-1;
        if(settleStage!=workspaceSmokeStage_){settleStage=workspaceSmokeStage_;settleTicks=0;}
        const bool afterDialogClose=workspaceSmokeStage_==11||workspaceSmokeStage_==17||workspaceSmokeStage_==19||workspaceSmokeStage_==21||workspaceSmokeStage_==26;
        if(afterDialogClose&&window_.DialogActive()&&++settleTicks<12)return;
        if(window_.DialogActive()&&workspaceSmokeStage_!=10&&workspaceSmokeStage_!=16&&workspaceSmokeStage_!=18&&workspaceSmokeStage_!=20&&workspaceSmokeStage_!=25)throw std::runtime_error("unexpected dialog / task failure");
        switch(workspaceSmokeStage_){
        case 0:
            if(!loaded_)return;
            fixtures=source_.parent_path();out=fs::absolute(L"workspace-test-output");fs::create_directories(out);report.open(out/L"workspace.log");
            originalA=ReadBytes(fixtures/L"links.pdf");originalB=ReadBytes(fixtures/L"forms-mixed.pdf");
            check(documents_.size()==1&&activeDocument_,"initial document tab");smokeDocumentA_=activeDocument_->id;
            canvas_->ActualSize();canvas_->GoToPoint(1,80);search_->Text(L"Links");
            Task(L"测试 A 修改",[](Engine& e,const Cancel&){e.document.AddAnnotation(0,Tool::Note,{30,40,24,24},L"workspace-A");});break;
        case 1:
            // 前提：A 停在第 2 页、100%。刚加载完的首帧里画布尚未布局，跳页可能被初始定位覆盖，这里补跳并等待。
            if(canvas_->CurrentPage()!=1||std::abs(canvas_->ActualZoom()-1)>=.01f){if(++settleTicks<20){canvas_->ActualSize();canvas_->GoToPoint(1,80);return;}}
            check(canvas_->CurrentPage()==1&&std::abs(canvas_->ActualZoom()-1)<.01f,"A view precondition (page 2, 100%)");
            check(info_.dirty&&info_.canUndo,"A edit/undo state");Open(fixtures/L"forms-mixed.pdf");break;
        case 2:
            check(documents_.size()==2&&FindDocument(smokeDocumentA_)->info.dirty,"opening B preserved unsaved A");
            smokeDocumentB_=activeDocument_->id;check(smokeDocumentB_!=smokeDocumentA_&&FindDocument(smokeDocumentA_)->worker!=worker_,"independent engine workers");
            check(search_->Text().empty(),"new tab does not inherit search");
            Task(L"测试 B 修改",[](Engine& e,const Cancel&){e.document.AddAnnotation(0,Tool::Note,{60,70,24,24},L"workspace-B");});break;
        case 3:Open(fixtures/L"links.pdf");break;
        case 4:
            check(documents_.size()==2&&activeDocument_&&activeDocument_->id==smokeDocumentA_,"same file selects existing tab");
            // 切回标签后视图在下一次布局时恢复：给几帧时间再判定。
            if(!(canvas_->CurrentPage()==1&&std::abs(canvas_->ActualZoom()-1)<.01f)&&++settleTicks<12)return;
            check(canvas_->CurrentPage()==1&&std::abs(canvas_->ActualZoom()-1)<.01f,"tab view/zoom restored");
            check(search_->Text()==L"Links","tab query restored");
            Task(L"测试 A 独立撤销",[](Engine& e,const Cancel&){
                const auto a=e.document.Annotations(0);bool own=false;for(const auto& value:a){if(value.text==L"workspace-A")own=true;if(value.text==L"workspace-B")throw std::runtime_error("B leaked into A");}if(!own)throw std::runtime_error("A edit lost");e.document.Undo();
            });break;
        case 5:
            check(info_.canRedo,"A undo history survived tab switching");ActivateDocument(smokeDocumentB_);
            Task(L"测试 B 保留修改",[](Engine& e,const Cancel&){bool own=false;for(const auto& a:e.document.Annotations(0))if(a.text==L"workspace-B")own=true;if(!own||!e.document.Info().canUndo)throw std::runtime_error("A undo changed B");});break;
        case 6:
            SetSplit(1,smokeDocumentA_);check(splitMode_==1&&referenceInfo_.pages.size()==3&&info_.pages.size()==2,"different document split binding");
            referenceCanvas_->GoTo(2);SetSplit(2);check(referenceCanvas_->CurrentPage()==2&&splitBody_->LayoutMode()==2,"orientation preserves reference position");Backup();break;
        case 7:
            if(FindDocument(smokeDocumentA_)->backupPending->load()||FindDocument(smokeDocumentB_)->backupPending->load())return;
            check(fs::exists(FindDocument(smokeDocumentA_)->recovery)&&fs::exists(FindDocument(smokeDocumentB_)->recovery),"all dirty tabs backed up independently");
            ActivateDocument(smokeDocumentA_);check(referenceId_==smokeDocumentB_&&referenceInfo_.pages.size()==2&&info_.pages.size()==3,"swap panes uses correct engines");
            check(canvas_->CurrentPage()==2,"reference reading position follows document on swap");documentTabs_->MoveTab(0,1);break;
        case 8:check(activeDocument_->id==smokeDocumentA_,"reorder keeps active identity");CycleDocument(1);break;
        case 9:check(activeDocument_->id==smokeDocumentB_,"cycle follows visual tab order");CloseDocument(smokeDocumentB_);break;
        case 10:
            check(window_.DialogActive()&&documents_.size()==2,"dirty close asks before removing tab");window_.CloseDialog();break;
        case 11:
            check(!window_.DialogActive()&&documents_.size()==2&&info_.dirty,"cancel close preserves dirty document");
            source_=out/L"B.pdf";Save(false);break;
        case 12:
            check(fs::exists(out/L"B.pdf")&&!info_.dirty,"save belongs to active tab");CloseDocument(smokeDocumentB_);break;
        case 13:
            check(documents_.size()==1&&activeDocument_->id==smokeDocumentA_&&!splitMode_,"close releases correct tab and exits split");
            check(info_.canRedo,"closing B did not reset A history");NewDocument();break;
        case 14:
            check(documents_.size()==2&&source_.empty()&&info_.pages.size()==1,"new blank document is separate tab");
            Task(L"测试空白标签",[](Engine& e,const Cancel&){e.document.AddAnnotation(0,Tool::Note,{20,20,24,24},L"workspace-C");});break;
        case 15:
            report<<"exit-before active="<<activeDocument_->id<<" dirty="<<info_.dirty<<" busy="<<busy_<<" dialog="<<window_.DialogActive()<<" docs="<<documents_.size()<<"\n"<<std::flush;
            for(const auto& doc:documents_)report<<"session "<<doc->id<<" dirty="<<doc->info.dirty<<" pages="<<doc->info.pages.size()<<"\n"<<std::flush;
            ConfirmWorkspaceExit();
            report<<"exit-after active="<<activeDocument_->id<<" dirty="<<info_.dirty<<" closing="<<closing_<<" dialog="<<window_.DialogActive()<<" docs="<<documents_.size()<<"\n"<<std::flush;break;
        case 16:
            report<<"exit-check dirty="<<info_.dirty<<" closing="<<closing_<<" dialog="<<window_.DialogActive()<<" docs="<<documents_.size()<<"\n"<<std::flush;
            check(window_.DialogActive()&&!closing_&&documents_.size()==2,"window exit guards inactive dirty tabs");window_.CloseDialog();break;
        case 17:
            check(!closing_&&documents_.size()==2,"cancel exit leaves every tab open");Open(fixtures/L"workspace-does-not-exist.pdf");break;
        case 18:
            check(window_.DialogActive()&&documents_.size()==2&&activeDocument_&&activeDocument_->id==smokeDocumentA_,"failed open returns to original tab");window_.CloseDialog();break;
        case 19:Open(fixtures/L"password.pdf");break;
        case 20:
            check(window_.DialogActive()&&documents_.size()==3,"password prompt isolates new document");window_.CloseDialog();break;
        case 21:
            check(documents_.size()==2&&activeDocument_&&activeDocument_->id==smokeDocumentA_,"password cancel preserves other tabs");
            check(ReadBytes(fixtures/L"links.pdf")==originalA&&ReadBytes(fixtures/L"forms-mixed.pdf")==originalB,"input PDFs unchanged");
            SetSplit(1);break;
        case 22:{
            const auto a=primaryPane_->AbsoluteBounds(),b=referencePane_->AbsoluteBounds();
            check(a.w>100&&b.w>100&&b.x>=a.Right()&&std::abs(a.y-b.y)<1,"left/right non-overlapping pane geometry");SetSplit(2);break;
        }
        case 23:{
            const auto a=primaryPane_->AbsoluteBounds(),b=referencePane_->AbsoluteBounds();
            check(a.h>100&&b.h>100&&b.y>=a.Bottom()&&std::abs(a.x-b.x)<1,"top/bottom non-overlapping pane geometry");
            for(const auto& doc:documents_)if(doc->id!=smokeDocumentA_){ActivateDocument(doc->id);break;}break;
        }
        case 24:
            check(info_.dirty&&info_.canUndo,"new tab retains edits before failed save");fs::create_directories(out/L"blocked.pdf");source_=out/L"blocked.pdf";Save(false);break;
        case 25:
            check(window_.DialogActive()&&info_.dirty&&info_.canUndo&&documents_.size()==2,"failed save preserves dirty state and history");window_.CloseDialog();source_.clear();break;
        case 26:
            check(ReadBytes(fixtures/L"links.pdf")==originalA&&ReadBytes(fixtures/L"forms-mixed.pdf")==originalB,"input PDFs unchanged after failure tests");
            report<<"PASS: independent documents, dirty state, undo/redo, view/query, reorder, close/cancel/save, both split orientations/geometry, failed save, pane swap, per-tab backups, exit cancel, failed/password open.\n";report.flush();
            workspaceSmokeStage_=100;closing_=true;window_.Close();return;
        default:throw std::runtime_error("invalid workspace test stage");
        }
        if(report)report<<"stage "<<workspaceSmokeStage_<<" OK\n"<<std::flush;
        ++workspaceSmokeStage_;
    }catch(const std::exception& error){
        if(report){report<<"FAIL stage "<<workspaceSmokeStage_<<": "<<error.what()<<"\n";report.flush();}
        workspaceSmokeExit_=1;workspaceSmokeStage_=101;closing_=true;window_.Close();
    }
}
}
