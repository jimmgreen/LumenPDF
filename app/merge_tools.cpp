#include "application.h"
#include "ui.h"
#include <algorithm>
#include <cwchar>
namespace lpdf {
using namespace lumen;
namespace {
std::wstring_view KindName(InputFileKind kind){
    switch(kind){case InputFileKind::Pdf:return L"PDF";case InputFileKind::Word:return L"Word";
        case InputFileKind::Excel:return L"Excel";case InputFileKind::PowerPoint:return L"PowerPoint";
        case InputFileKind::Text:return L"文本";case InputFileKind::Image:return L"图片";default:return L"其他文件";}
}
std::wstring Elapsed(uint64_t milliseconds){
    const auto seconds=milliseconds/1000;wchar_t buffer[40]{};
    swprintf_s(buffer,L"已用时 %02llu:%02llu",static_cast<unsigned long long>(seconds/60),static_cast<unsigned long long>(seconds%60));return buffer;
}
std::wstring StepName(ProgressStage stage,int phase){
    switch(stage){
    case ProgressStage::Reading:return L"读取文件";
    case ProgressStage::TextLayout:return L"文本分页排版";
    case ProgressStage::ImageDecode:return L"解码并排版图片";
    case ProgressStage::OfficeConversion:return L"Office 转换为 PDF";
    case ProgressStage::ConversionReady:return L"文件已准备，等待读取页码";
    case ProgressStage::Inspecting:return L"检查页码与密码";
    case ProgressStage::Flattening:return L"整理批注与表单外观";
    case ProgressStage::MergingPages:return L"合并页面";
    case ProgressStage::Bookmarks:return L"生成文件目录";
    case ProgressStage::Writing:return phase==1?L"写入转换结果":L"写入 PDF";
    case ProgressStage::Validating:return L"校验 PDF";
    case ProgressStage::Optimizing:return L"整理嵌入字体";
    case ProgressStage::Publishing:return phase==1?L"完成转换文件":L"安全写入目标文件";
    case ProgressStage::Complete:return L"正在完成";
    }
    return L"处理中";
}
}
// 转换器的英文错误 → 中文说明（合并与打开文件共用）。
std::wstring FriendlyConversionError(std::wstring error){
    const std::pair<std::wstring_view,std::wstring_view> known[]{
        {L"Text encoding is not UTF-8; choose GB18030 or UTF-16 in conversion settings",L"文本编码不是 UTF-8，请在右侧选择 GB18030 或 UTF-16 后重试。"},
        {L"Incomplete UTF-16 text",L"UTF-16 文本数据不完整，请检查文件编码。"},
        {L"Text contains NUL characters; check its encoding",L"文本包含空字符，请检查或更换所选编码。"},
        {L"Install Microsoft Office or LibreOffice, or select the Office backend in settings",L"未找到可用的 Office / LibreOffice，请安装转换器或在合并视图右侧选择可用的后端。"},
        {L"PowerPoint is already running; close PowerPoint or choose LibreOffice",L"PowerPoint 正在运行。为避免影响你打开的演示文稿，LumenPDF 不会借用它：请先保存并关闭 PowerPoint 后重试，或在合并视图右侧改用 LibreOffice。"},
        {L"Excel returned an existing instance; close Excel or choose LibreOffice",L"Excel 返回了你正在使用的窗口，LumenPDF 不会借用它：请先关闭 Excel 后重试，或改用 LibreOffice。"},
        {L"Word returned an existing instance; close Word or choose LibreOffice",L"Word 返回了你正在使用的窗口，LumenPDF 不会借用它：请先关闭 Word 后重试，或改用 LibreOffice。"},
        {L"Supported files: PDF, Word, Excel, PowerPoint, TXT, PNG, JPG",L"支持的文件：PDF、Word、Excel、PowerPoint、TXT、PNG、JPG。"}
    };
    for(const auto& [from,to]:known){const auto at=error.find(from);if(at!=std::wstring::npos){error.replace(at,from.size(),to);break;}}
    return error;
}
namespace {
std::wstring RowStatus(const MergeRowProgress& row){
    switch(row.state){
    case MergeRowState::Waiting:return L"待处理";
    case MergeRowState::Converting:return L"正在准备 / 转换";
    case MergeRowState::Prepared:return L"已准备";
    case MergeRowState::Inspecting:return row.pages?L"已检查 "+std::to_wstring(row.pages)+L" 页":L"检查页码中";
    case MergeRowState::Flattening:return L"整理页面外观";
    case MergeRowState::Merging:return L"合并 "+std::to_wstring(row.done)+L" / "+std::to_wstring(row.pages)+L" 页";
    case MergeRowState::Merged:return L"已合并 "+std::to_wstring(row.pages)+L" 页";
    case MergeRowState::Failed:return L"处理失败";
    case MergeRowState::Cancelled:return L"已取消";
    case MergeRowState::NotProcessed:return L"未处理";
    }
    return {};
}
}
void Application::SyncQueueFields(){
    if(queueUpdating_||queueSelection_<0||queueSelection_>=static_cast<ptrdiff_t>(queue_.size()))return;
    auto& row=queue_[static_cast<size_t>(queueSelection_)];
    const bool changed=row.range!=range_->Text()||row.password!=password_->Text();
    row.range=range_->Text();row.password=password_->Text();
    if(changed){ResetMergeStatus();RefreshQueueRows();}
}
void Application::ResetMergeStatus(){
    if(mergeRunning_)return;
    mergeProgress_.reset();mergeRevision_=~uint64_t{};
    if(mergeProgressPanel_)mergeProgressPanel_->Visible(false);
}
void Application::RefreshQueueRows(const MergeSnapshot* progress){
    if(queueRows_.size()!=queue_.size())queueRows_.resize(queue_.size());
    for(size_t i=0;i<queue_.size();++i){
        auto& view=queueRows_[i];const auto& item=queue_[i];view.kind=FileKind(item.path);
        view.title=std::to_wstring(i+1)+L"   "+item.path.filename().wstring();
        const bool known=progress&&i<progress->rows.size();
        view.failed=known&&progress->rows[i].state==MergeRowState::Failed;
        const auto state=known?RowStatus(progress->rows[i]):L"待处理";
        view.secondary=state+L"  ·  "+std::wstring(KindName(view.kind))+L"  ·  "+(item.range.empty()?L"全部页":L"页码 "+item.range);
    }
    queueList_->RefreshItems();
}
void Application::Queue(std::vector<fs::path> files){
    if(busy_||files.empty())return;SyncQueueFields();
    for(const auto& path:files)queue_.push_back({path,L"",L""});QueueChanged();Mode(3);
}
void Application::QueueSelection(){
    const auto n=queueList_->SelectedIndex();const bool selected=n>=0&&n<static_cast<ptrdiff_t>(queue_.size());
    range_->Enabled(selected&&!busy_);password_->Enabled(selected&&!busy_);
    queueUp_->Enabled(selected&&n>0&&!busy_);queueDown_->Enabled(selected&&n+1<static_cast<ptrdiff_t>(queue_.size())&&!busy_);
    queueRemove_->Enabled(selected&&!busy_);previewButton_->Enabled(!queue_.empty()&&!busy_);exportButton_->Enabled(!queue_.empty()&&!busy_);
}
void Application::QueueChanged(){
    ResetMergeStatus();RefreshQueueRows();
    queueUpdating_=true;queueList_->ItemCount(queue_.size(),false);queueList_->SelectedIndex(-1);queueSelection_=-1;
    queueUpdating_=false;QueueSelection();range_->Text(L"");password_->Text(L"");
    mergeStatus_->Text(std::to_wstring(queue_.size())+L" 个文件 · 可选择文件设置页码与密码");window_.Invalidate();
}
void Application::MoveQueue(int delta){
    if(busy_)return;SyncQueueFields();
    const auto n=queueList_->SelectedIndex(),to=n+delta;if(n<0||to<0||to>=static_cast<ptrdiff_t>(queue_.size()))return;
    std::swap(queue_[n],queue_[to]);QueueChanged();queueList_->SelectedIndex(to);
}
void Application::BeginMergeProgress(size_t files,bool exportFile,const fs::path& output){
    mergeProgress_=std::make_shared<MergeProgressState>(files);mergeRunning_=true;mergeCancelling_=false;
    mergeExport_=exportFile;mergeOutput_=output;mergeStarted_=GetTickCount64();mergeElapsed_=0;mergeRevision_=~uint64_t{};
    mergeProgressPanel_->Visible(true);mergeCancel_->Visible(true).Enabled(true);mergeCancel_->Text(L"取消任务");
    mergeProgressBar_->Value(0).Indeterminate(true);UpdateMergeProgress();
    // WM_TIMER is lower-priority than an indeterminate bar's paint loop.
    // Sample on the LUMEN frame clock so visible progress and elapsed time do
    // not stay at the initial caption while a long converter is working.
    mergeFrame_={};mergeFrameDelay_=0;
    mergeFrame_=window_.OnFrame([this](float dt){
        if(!mergeRunning_)return false;
        mergeFrameDelay_+=dt;
        if(mergeFrameDelay_>=.15f){mergeFrameDelay_=0;UpdateMergeProgress();}
        return true;
    });
}
void Application::UpdateMergeProgress(){
    if(!mergeProgress_)return;
    auto snapshot=mergeProgress_->Read();
    const auto elapsed=mergeRunning_?GetTickCount64()-mergeStarted_:mergeElapsed_;
    const auto time=Elapsed(elapsed);if(mergeElapsedLabel_->Text()!=time)mergeElapsedLabel_->Text(time);
    if(snapshot.revision==mergeRevision_&&!mergeCancelling_)return;mergeRevision_=snapshot.revision;
    RefreshQueueRows(&snapshot);
    const auto& event=snapshot.current;const bool active=event.fileIndex<queue_.size();
    std::wstring title,detail=StepName(event.stage,snapshot.phase);
    if(snapshot.outcome==MergeOutcome::Running){
        title=snapshot.phase==1?L"1 / 3  准备与转换文件":snapshot.phase==2?L"2 / 3  合并页面":L"3 / 3  写入与校验";
        if(mergeCancelling_){title=L"正在取消任务";detail=L"正在结束当前步骤；不会继续处理后续文件。";}
        else if(event.stage==ProgressStage::MergingPages&&event.total){
            detail=L"已合并 "+std::to_wstring(event.completed)+L" / "+std::to_wstring(event.total)+L" 页";
        }else if(event.stage==ProgressStage::TextLayout&&event.completed){detail+=L" · 已生成 "+std::to_wstring(event.completed)+L" 页";}
        else if(event.stage==ProgressStage::OfficeConversion&&!event.backend.empty()){detail=L"正在使用 "+event.backend+L" 转换；此步骤不提供精确百分比。";}
        mergeFileLabel_->Text(active?L"文件 "+std::to_wstring(event.fileIndex+1)+L" / "+std::to_wstring(queue_.size())+L"   "+queue_[event.fileIndex].path.filename().wstring():L"目标："+mergeOutput_.filename().wstring());
        mergeFileLabel_->ToolTip(active?queue_[event.fileIndex].path.wstring():mergeOutput_.wstring());
        const bool measurable=event.stage==ProgressStage::MergingPages&&event.total>0&&!mergeCancelling_;
        mergeProgressBar_->Indeterminate(!measurable);
        if(measurable)mergeProgressBar_->Value(static_cast<float>(event.completed)/static_cast<float>(event.total));
        mergeDetailLabel_->AccessibleName(L"合并进度："+detail);
        status_->Text(title+L" · "+detail);
    }else{
        title=snapshot.outcome==MergeOutcome::Complete?(mergeExport_?L"合并导出完成":L"合并预览已就绪"):
            snapshot.outcome==MergeOutcome::Cancelled?L"任务已取消":L"处理失败";
        detail=snapshot.message;
        mergeFileLabel_->Text(snapshot.outcome==MergeOutcome::Complete?std::to_wstring(queue_.size())+L" 个文件 · "+std::to_wstring(event.total)+L" 页":
            active?queue_[event.fileIndex].path.filename().wstring():L"未完成最终写入");
        mergeProgressBar_->Indeterminate(false).Value(snapshot.outcome==MergeOutcome::Complete?1.0f:0.0f);
        mergeDetailLabel_->AccessibleName(L"合并结果："+detail);mergeCancel_->Visible(false);
    }
    mergeTitle_->Text(title).AccessibleName(title);mergeDetailLabel_->Text(detail).ToolTip(detail);
    mergeTitle_->Foreground(snapshot.outcome==MergeOutcome::Failed?window_.VisualTheme().danger:Color{0,0,0,0});
}
void Application::FinishMergeProgress(MergeOutcome outcome,std::wstring message){
    if(!mergeProgress_)return;
    mergeElapsed_=GetTickCount64()-mergeStarted_;mergeRunning_=false;mergeCancelling_=false;
    mergeProgress_->Finish(outcome,std::move(message));
    mergeFrame_={};
    UpdateMergeProgress();QueueSelection();
    const auto state=mergeProgress_->Read();
    mergeStatus_->Text(outcome==MergeOutcome::Complete?std::to_wstring(queue_.size())+L" 个文件已处理 · 结果 "+std::to_wstring(state.current.total)+L" 页":
        outcome==MergeOutcome::Cancelled?L"本次任务已取消；可修改队列后重新运行。":L"未完成合并；请检查上方详情与失败文件。");
}
void Application::RequestCancel(){
    if(!busy_||!cancel_)return;
    cancel_->store(true);status_->Text(L"正在取消…");
    if(mergeRunning_){mergeCancelling_=true;mergeCancel_->Enabled(false);mergeCancel_->Text(L"正在取消…");UpdateMergeProgress();}
}
void Application::Merge(bool exportFile){
    if(queue_.empty()||busy_)return;SyncQueueFields();
    fs::path destination;if(exportFile){destination=Destination(L"合并文档.pdf");if(destination.empty())return;}
    auto inputs=queue_;ConversionOptions options;options.encoding=static_cast<TextEncoding>(encoding_->SelectedIndex());options.office=static_cast<OfficeBackend>(office_->SelectedIndex());options.imageA4=a4_->Checked();
    const bool bookmarks=bookmarks_->Checked();auto temporary=std::make_shared<TempDirectory>();
    const fs::path output=exportFile?destination:temporary->path/L"合并预览.pdf";
    auto action=[this,inputs,options,bookmarks,temporary,output,destination,exportFile]{
        BeginMergeProgress(inputs.size(),exportFile,output);const auto progress=mergeProgress_;
        Task(L"准备合并",[inputs,options,bookmarks,output,exportFile,progress](Engine& e,const Cancel& cancel){
            std::vector<MergeInput> converted;converted.reserve(inputs.size());
            for(size_t i=0;i<inputs.size();++i){
                CheckCancel(cancel);
                try{
                    auto value=e.converter.Convert(inputs[i].path,options,cancel,[&](const OperationProgress& event){progress->Conversion(i,event);});
                    converted.push_back({value.pdf,inputs[i].range,inputs[i].password});progress->Converted(i,value.backend);
                }catch(const Cancelled&){throw;}
                catch(const std::exception& ex){throw std::runtime_error(Utf8(inputs[i].path.filename().wstring())+": "+ex.what());}
            }
            Document::Merge(converted,output,bookmarks,cancel,{},[&](const OperationProgress& event){progress->Merge(event);});
            // Save publishes atomically. Do not reinterpret a successful export
            // as cancelled if the user presses Cancel after that commit point.
            if(!exportFile){e.document.Open(output);e.open=true;}
        },[this,temporary,destination,exportFile,progress]{
            if(progress!=mergeProgress_)return;
            FinishMergeProgress(MergeOutcome::Complete,exportFile?L"已导出："+destination.wstring():L"预览已生成；请检查分页、字体和顺序后另存为。");
            if(exportFile){status_->Text(L"合并导出完成："+destination.filename().wstring());exportButton_->Flash(StatusTone::Success);}
            else{preview_=temporary;previewFiles_=queue_.size();source_.clear();home_=false;name_->Text(L"合并预览.pdf");Mode(0);Page(0,true);UpdateTitle();status_->Text(L"合并预览已就绪，请检查分页与字体后保存；也可以返回合并列表调整。");}
        },[this,progress]{
            if(progress!=mergeProgress_)return;
            FinishMergeProgress(MergeOutcome::Failed,L"此文件需要 PDF 密码，请在右侧填写密码后重试；目标文件未替换。");
            window_.Alert(L"需要 PDF 密码",L"请为处理失败的文件填写打开密码后重新运行。");
        },[this,progress](std::wstring error){
            if(progress!=mergeProgress_)return;
            error=FriendlyConversionError(std::move(error));FinishMergeProgress(MergeOutcome::Failed,error);status_->Text(L"合并未完成，请查看上方错误详情。");
            window_.Alert(L"转换或合并失败",error);
        },[this,progress]{
            if(progress!=mergeProgress_)return;
            FinishMergeProgress(MergeOutcome::Cancelled,L"任务已取消，原文件和已有目标文件未改写。");status_->Text(L"任务已取消，原文件保持不变。");
        });
    };
    if(exportFile){for(const auto& doc:documents_)if(!doc->source.empty()&&RecentFiles::SamePath(destination,doc->source)){Fail(L"导出目标已在标签页中打开，请另选文件名。");return;}action();}
    else if(textEditor_.Active()){FinishText(true,[this]{Merge(false);});}
    else if(BeginDocument())action();
}
}