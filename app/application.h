#pragma once
#include <set>
#include "canvas.h"
#include "inline_editor.h"
#include "text_format_bar.h"
#include "worker.h"
#include "document_tabs.h"
#include "merge_progress.h"
#include "recent_files.h"
#include "recent_view.h"
#include "print_job.h"
#include "clipboard_image.h"
#include "app_settings.h"
#include <memory>
#include "core/signature.h"
#include <lumen/ProgressBar.h>
#include <array>
#include <map>
#include <optional>
#include <lumen/Window.h>
#include <lumen/Button.h>
#include <lumen/Label.h>
#include <lumen/TextBox.h>
#include <lumen/ListView.h>
#include <lumen/ComboBox.h>
#include <lumen/CheckBox.h>
#include <lumen/TreeView.h>
#include <lumen/Segmented.h>
namespace lpdf {
class SpeechPlayer;
std::wstring FriendlyConversionError(std::wstring error);
// 外部修改检测用的文件戳：最后写入时间 + 大小。
struct FileStamp { uint64_t time{}, size{}; bool operator==(const FileStamp&) const = default; };
class Application {
public:
    Application();
    ~Application();
    int Run(const fs::path& initial={},bool smoke=false,const fs::path& mergeFile={});
    // 右键“使用 LumenPDF 合并”转来的文件：短时间内到达的文件汇总后一次加入合并列表。
    void AcceptMerge(const fs::path& path);
    // 其它进程（再次双击 PDF）转来的打开请求；路径为空时只激活窗口。
    void AcceptExternal(const fs::path& path);
private:
    void Build();
    void ApplyIcon();
    void Mode(int);
    void ChooseTool(Tool);
    // 当前界面（主页 / 阅读 / 批注 / 页面 / 合并）的默认状态栏提示。
    std::wstring ContextHint()const;
    void SetHand(bool on);
    void ToolKey(Tool tool);
    void CycleTool(std::initializer_list<Tool> order,Tool fallback);
    void SpaceKey(bool down,bool repeat);
    void DeleteSelectedPages();
    void ShowZoomMenu();
    void SyncSearchButtons();
    void RemoveQueueItem();
    void PageSelectionHint(size_t count);
    void Open(const fs::path&,std::wstring password={});
    void ChooseOpen();
    void Save(bool as=true,std::function<void()> after={});
    void Guard(std::function<void()> action);
    void Task(std::wstring caption,std::function<void(Engine&,const Cancel&)> work,
              std::function<void()> done={},std::function<void()> password={},std::function<void(std::wstring)> failed={},std::function<void()> cancelled={});
    void Refresh(DocumentInfo,std::vector<Annotation>,int page);
    void Page(int,bool scroll=false);
    void Select(int,int);
    void SetBusy(bool,std::wstring={});
    void Fail(std::wstring);
    void Wire(PdfCanvas*);
    void BeginText(int,Annotation,std::optional<Point> caret={});
    void FormatText(TextFormat,float);
    void FitText();
    void FinishText(bool apply,std::function<void()> after={});
    void TextChanged();
    void Annotate(int,Tool,Rect,std::vector<Point>);
    void Search();
    void SearchNextPage(int page,int total,uint64_t generation,Cancel token);
    void SearchSettings();
    void PageToolsMenu();
    void CropSelectedPages();
    void PrintOptions();
    void PrintExecute();
    void PrintPreviewStep(int delta);
    void PrintPreviewRefresh(bool render=true);
    void PrintPreviewRender();
    void AcceptPrintPreview(bool closeDialog);
    void PrintPreviewSmoke(const fs::path& dir,int step,int waited=0,int seen=0);
    bool PasteShortcut();
    void RotateView(int delta);          // 临时视图旋转：delta=±90 / 180；0 = 恢复
    void ApplyViewRotation();            // 按当前视图方向真正旋转全部页面（可撤销），随后视图归零
    void RotationSmoke(const fs::path& dir,int step);
    // 框选放大 / 放大镜（zoom_tools.cpp）
    bool ZoomToolsAvailable()const;
    void ToggleZoomBox();
    void ZoomBackKey();
    void ToggleMagnifier();
    std::wstring MagnifierHint()const;
    void WireZoomTools();
    void ZoomSmoke(const fs::path& dir,int step);
    // 朗读（read_aloud.cpp）
    struct Aloud {bool active{},toEnd{},selection{};int page{-1},index{-1},skipped{};uint64_t session{},ticket{},document{};std::vector<ReadingSentence> sentences;std::wstring voice;};
    Aloud aloud_;
    std::vector<int> aloudPages_;
    std::unique_ptr<SpeechPlayer> speech_;
    SpeechPlayer& Speech();
    bool CanReadAloud()const;
    void ReadAloud(bool toEnd);
    void ReadSelection();
    void ReadPage(int page);
    void ReadFetched(int page,uint64_t ticket,std::vector<ReadingSentence> sentences,const std::wstring& error);
    void ReadSentence(uint64_t session,int index,const std::wstring& voice);
    void ReadFinished(uint64_t session);
    void ReadPause();
    void ReadSkip(int delta);
    void StopReading(bool quiet=false);
    void ReadRate(int rate);
    void ReadVoice(const std::wstring& id);
    static std::wstring RateName(int rate);
    void ReadStatus();
    void ShowReadMenu();
    void SpeechSmoke(const fs::path& dir,int step,int waited=0);
    void PasteFromClipboard(PasteIntent requested);
    void ClipboardSmoke(const fs::path& dir,int step,int waited=0);
    void Hit(int);
    void ShowPageNumber(int page);
    void GoToPageText();
    void LoadRecent();
    void RememberRecent(const fs::path&);
    void RefreshRecent();
    void Home(bool show);
    void ForgetRecent(const fs::path&);
    void Copy(int,std::optional<Rect> = {});
    void Queue(std::vector<fs::path>);
    void QueueChanged();
    void QueueSelection();
    void MoveQueue(int);
    void Merge(bool exportFile);
    void SyncQueueFields();
    void RefreshQueueRows(const MergeSnapshot* progress=nullptr);
    void ResetMergeStatus();
    void BeginMergeProgress(size_t,bool,const fs::path&);
    void UpdateMergeProgress();
    void FinishMergeProgress(MergeOutcome,std::wstring);
    void RequestCancel();
    void Props();
    void BuildAnnotationProperties();
    void ShowProperties(const Annotation&,bool existing);
    void EditNote(int,Annotation,bool retry=false);
    void EditStamp(int,Annotation);
    // 填写与签名（signature_tools.cpp）：签名库、放置签名 / 日期 / ✓ / ✗。
    struct Placement { enum Kind { None, Signature, Date, Check, Cross } kind{None}; fs::path file; std::wstring text; int width{}, height{}; };
    void SignatureMenu();
    void DrawSignature();
    void TypeSignature();
    void ImportSignature();
    void ManageSignatures();
    void StoreSignature(SignatureKind kind,const Bitmap& bitmap);
    void ArmPlacement(Placement placement);
    bool PlacePending(int page,Rect r);
    fs::path SignatureFolder()const;
    void SignatureSmoke(const fs::path& output,int step);
    void FillField(int page,FormField field);
    void EditFieldText(int page,FormField field,std::wstring draft,std::wstring error={});
    void AfterDialog(std::function<void()> action,std::function<void()> fallback={},int attempt=0);
    void ApplyField(int page,FormField field,std::wstring value,bool next);
    void ResetFormFields();
    void FormSmoke(const fs::path& output,int step);
    void EnterRedaction();
    void ExitRedaction(bool discard=false);
    void RedactionStatus(std::wstring lead);
    void AddRedactions(std::vector<RedactionMark> marks,const wchar_t* what);
    void RemoveRedaction(size_t index);
    void RedactSelection(const PdfCanvas::TextSelection& selection);
    void RedactSearchHits();
    void CheckRedactionMarks();
    void ShowRedactionDialog();
    void ExportRedaction(const fs::path& dest,RedactionOptions options,std::function<void()> after={});
    void RedactionSmoke(const fs::path& output,int step);
    void ShowCompressDialog();
    void ShowSplitDialog();
    void ShowDecorateDialog();
    void AddBookmark();
    void RenameBookmark();
    void DeleteBookmark();
    void MoveBookmark(int delta);
    void IndentBookmark(int delta);
    void ShowOutlineMenu(lumen::Point point);
    void CommitOutline(std::vector<OutlineItem> items,std::wstring caption,size_t select);
    size_t OutlineSelection()const;
    void DeleteAnnotation(int,int);
    void SelectCreated(int,bool keepTool);
    void PickAnnotationColor(bool fill);
    void Backup();
    void Print();
    PrintSettings printSettings_;
    std::shared_ptr<struct PrintPreviewState> printPreview_;std::optional<PaperInfo> printPaper_;
    void SavePosition();
    bool RestorePosition();
    void FollowLink(const Link& link);
    void PushHistory();
    void Navigate(int direction);
    void CopySelection(const PdfCanvas::TextSelection& selection);
    void HighlightSelection(const PdfCanvas::TextSelection& selection);
    void ShowCanvasMenu(lumen::Point point);
    void RefreshDefaultApp();
    void MakeDefault();
    void WatchDefault();
    lumen::Window::TimerId defaultWatch_{};
    int defaultWatchTicks_{};
    void ToggleMergeMenu();
    void RepairShell();
    void FlushMerge();
    void ClosePreview(bool backToMerge);
    bool InPreview()const;
    void UpdateTitle();
    void SetTone(PageTone tone,bool save=true);
    void ShowToneMenu();
    void ShowPageMenu(lumen::Point point,int page);
    void SidePanel(int index);
    void RebuildOutline();
    void SyncOutline(int page);
    void RequestAnnotationList();
    void LoadSettings();
    void SaveSettings();
    void ApplyLayout(int layout,bool save=true);
    void ViewSwitch(lumen::Panel& parent,int bar);
    void ChooseView(int view);
    void SyncViewSwitch();
    void ShowMoreMenu();
    void ShowSettings();
    // 在线升级（update_ui.cpp）
    struct UpdateSession;
    std::shared_ptr<UpdateSession> updateSession_;
    UpdateSession& Updates();
    void ScheduleUpdateCheck();
    std::wstring UpdateMenuLabel();
    void CheckForUpdates(bool manual);
    void ShowUpdateOffer(bool manual,int attempt=0);
    void StartUpdateDownload();
    void ShowUpdateProgress();
    void ConfirmUpdateInstall(int attempt=0);
    void RunPendingUpdate();
    void ShowAbout();
    void ShowSecurityDialog();
    void ExportImages();
    void ExportText();
    void DocumentProperties();
    void EditPageLabels();
    void FullScreen();
    void ReadingMode();
    // 演示（F5）：黑底全屏逐页，隐藏全部界面，鼠标静止 2 秒自动隐藏；Esc / F5 退出并恢复原来的布局。
    void Present(bool on);
    void PresentCursor();
    void SetPaged(bool on,bool save=true);
    void SetSidebarWidth(float width,bool save);
    // 外部修改：当前文件在磁盘上被其它程序改写后询问（或按设置自动）重新载入，保持阅读位置。
    void StampSource();
    void CheckExternalChange();
    void ReloadFromDisk(uint64_t id,FileStamp stamp);
    struct PresentState {bool fullScreen{};int split{};uint64_t reference{};bool sidebar{true},preview{};};
    PresentState presentState_;
    bool presenting_{},cursorHidden_{};
    long cursorX_{},cursorY_{};
    unsigned long long cursorMoved_{};
    lumen::Window::TimerId presentTimer_{};
    std::map<uint64_t,FileStamp> stamps_;
    std::optional<std::pair<uint64_t,FileStamp>> pendingStamp_;
    float sidebarWidth_{260};
    void FilterAnnotations();
    void ExportAnnotationSummary();
    bool fullScreen_{},readingMode_{},autoScroll_{};
    LONG_PTR savedWindowStyle_{};
    WINDOWPLACEMENT savedWindowPlacement_{sizeof(WINDOWPLACEMENT)};
    lumen::Connection autoScrollFrame_;
    lumen::Column* annotationPanel_{};
    lumen::TextBox* annotationFilter_{};
    lumen::ComboBox* annotationSort_{};
    std::vector<fs::path> Pick(bool multiple=false,const wchar_t* filter=nullptr);
    fs::path Destination(std::wstring name);
    std::vector<int> SelectedPages();
    lumen::App app_;
    lumen::Window window_;
    std::shared_ptr<Worker> worker_;
    struct DocumentSession {
        uint64_t id{},returnTo{};
        std::shared_ptr<Worker> worker;
        DocumentInfo info;
        std::vector<Annotation> annotations;
        fs::path source,recovery,restored;
        std::shared_ptr<TempDirectory> preview;
        size_t previewFiles{};
        std::wstring title,query,lastQuery;
        std::vector<SearchHit> hits;
        SearchOptions searchOptions;
        int page{},annotation{-1},hit{-1},mode{},side{},layout{};
        bool hand{},hideAnnotations{};
        PdfCanvas::ViewState view;
        std::vector<PdfCanvas::ViewState> back,forward;
        std::shared_ptr<std::atomic_bool> backupPending=std::make_shared<std::atomic_bool>(false);
    };
    std::vector<std::shared_ptr<DocumentSession>> documents_;
    std::shared_ptr<DocumentSession> activeDocument_;
    std::set<uint64_t> formHinted_;
    bool redacting_{};std::vector<RedactionMark> redactMarks_;uint64_t redactDoc_{};std::vector<PageInfo> redactGeometry_;
    std::vector<std::pair<std::shared_ptr<Worker>,fs::path>> retiredWorkers_;
    uint64_t nextDocumentId_{1},referenceId_{};
    DocumentTabs* documentTabs_{};
    lumen::Row* documentBar_{};
    lumen::Button* splitButton_{};
    DocumentSplit* splitBody_{};
    lumen::Column *primaryPane_{},*referencePane_{};
    lumen::Row* referenceTools_{};
    lumen::Label* primaryCaption_{};
    lumen::Button* referenceTitle_{};
    lumen::TextBox* referencePage_{};
    lumen::Label* referenceCount_{};
    PdfCanvas* referenceCanvas_{};
    DocumentInfo referenceInfo_;
    std::shared_ptr<std::atomic_uint64_t> referenceGeneration_=std::make_shared<std::atomic_uint64_t>(1);
    bool syncingTabs_{},switchingDocuments_{},openDrainScheduled_{};
    int splitMode_{},pendingSplitMode_{};
    std::deque<fs::path> pendingOpen_;
    std::shared_ptr<DocumentSession> FindDocument(uint64_t id)const;
    void CaptureDocument();
    bool BeginDocument();
    void NewDocument();
    void ActivateDocument(uint64_t id);
    void CycleDocument(int direction);
    void CloseDocument(uint64_t id);
    void DropDocument(uint64_t id);
    void SyncDocumentTabs();
    void ConfirmWorkspaceExit();
    void ConfirmWorkspaceExitAt(std::vector<uint64_t> ids,size_t index);
    void OpenFiles(std::vector<fs::path> files);
    void DrainOpenFiles();
    void ShowSplitMenu();
    void SetSplit(int mode,uint64_t reference=0);
    void LoadReference(bool preserve=false);
    void SaveReferencePosition();
    void ChooseReference();
    void WireReference();
    void CopyReference(PdfCanvas::TextSelection selection);
    void WorkspaceSmokeTick();
    int workspaceSmokeStage_{},workspaceSmokeTicks_{},workspaceSmokeExit_{};
    uint64_t smokeDocumentA_{},smokeDocumentB_{};
    InlineEditor textEditor_;
    TextFormatBar textFormatBar_;
    TextFormat defaultTextFormat_;
    std::optional<Annotation> textDraft_,textOriginal_;
    int textPage_{};
    bool textSync_{},textGeometryChanged_{};
    float textMinHeight_{24};
    lumen::Button *finishText_{},*cancelText_{},*editText_{},*deleteText_{};
    std::shared_ptr<std::atomic_bool> alive_=std::make_shared<std::atomic_bool>(true);
    std::shared_ptr<std::atomic_uint64_t> generation_=std::make_shared<std::atomic_uint64_t>(1);
    Cancel cancel_;
    DocumentInfo info_;
    std::vector<Annotation> annotations_;
    std::vector<MergeInput> queue_;
    struct MergeRowView {InputFileKind kind{InputFileKind::Other};std::wstring title,secondary;bool failed{};};
    std::vector<MergeRowView> queueRows_;
    ptrdiff_t queueSelection_{-1};
    std::shared_ptr<MergeProgressState> mergeProgress_;
    lumen::Connection mergeFrame_;
    float mergeFrameDelay_{};
    uint64_t mergeStarted_{},mergeElapsed_{},mergeRevision_{~uint64_t{}};
    bool mergeRunning_{},mergeCancelling_{},mergeExport_{};
    fs::path mergeOutput_;
    lumen::Row* mergeBody_{};
    lumen::Column* mergeProgressPanel_{};
    lumen::Label *mergeTitle_{},*mergeFileLabel_{},*mergeDetailLabel_{},*mergeElapsedLabel_{};
    lumen::ProgressBar* mergeProgressBar_{};
    lumen::Button *addFiles_{},*mergeCancel_{};
    std::vector<SearchHit> hits_;
    SearchOptions searchOptions_{false,false,true,false};
    Cancel searchCancel_;
    lumen::ListView* searchResults_{};
    lumen::Column* searchPanel_{};
    fs::path source_,recoveryRoot_,recoveryFile_,restoredFile_;
    std::shared_ptr<TempDirectory> preview_;
    int mode_{},page_{},annotation_{-1},hit_{-1};
    bool busy_{},closing_{},loaded_{},queueUpdating_{},smoke_{};
    lumen::Column *welcomePane_{},*propEditor_{},*propEmpty_{};
    lumen::Control* propViewport_{};
    lumen::Button* imageRotate_{};
    lumen::Button *previewButton_{},*exportButton_{},*queueUp_{},*queueDown_{},*queueRemove_{};
    lumen::Column *readPane_{},*mergePane_{},*properties_{};
    lumen::Row *tools_{},*readerTools_{},*annotationTools_{},*pageTools_{},*header_{};
    PdfCanvas *canvas_{},*thumbs_{};
    lumen::ListView *outline_{},*queueList_{};
    lumen::Label *name_{},*status_{},*pageLabel_{},*propLabel_{},*mergeStatus_{},*hitLabel_{},*titleSep_{},*pageHint_{};
    lumen::Button *zoomButton_{},*prevHit_{},*nextHit_{};
    std::wstring lastQuery_;
    lumen::TextBox* pageBox_{};
    RecentFiles recent_;
    ReadingPositions positions_;
    std::vector<PdfCanvas::ViewState> back_,forward_;
    std::wstring statusBeforeLink_,linkStatus_;
    lumen::Button* defaultApp_{};
    lumen::TreeView* outlineTree_{};
    lumen::Row* outlineBar_{};
    lumen::ListView* outlineEdit_{};
    lumen::Button* outlineArrange_{};
    bool outlineArranging_{};
    void TextDialog(std::wstring title,std::wstring message,std::wstring initial,std::wstring action,std::function<void(std::wstring)> then);
    lumen::ListView* annotList_{};
    lumen::Segmented* sideTabs_{};
    lumen::Button* toneButton_{};
    lumen::Label* dirtyMark_{};
    struct AnnotationRow {int page{};Annotation annotation;};
    std::vector<AnnotationRow> annotRows_,allAnnotRows_;
    uint64_t annotRowsGeneration_{~uint64_t{}};
    bool annotRowsLoading_{},outlineSync_{};
    int sidePanel_{};
    std::optional<std::pair<int,int>> pendingSelect_;
    PageTone tone_{PageTone::Normal};
    fs::path settingsFile_;
    AppSettings settings_;
    lumen::Button* viewButtons_[2][3]{};  // [阅读工具栏, 页面工具栏][单页, 双页, 网格]
    int lastSpread_{1};
    lumen::Button* moreButton_{};
    std::vector<fs::path> pendingMerge_;
    bool mergeFlushScheduled_{};
    lumen::Label* defaultHint_{};
    lumen::Row* previewBar_{};
    lumen::Label* previewText_{};
    size_t previewFiles_{};
    RecentView* recentView_{};
    lumen::TextBox* recentFilter_{};
    lumen::Label* recentCount_{};
    lumen::Button *homeButton_{},*clearRecent_{};
    bool home_{};
    lumen::Row* viewGroup_{};
    bool keepSearch_{};
    lumen::TextBox *search_{},*content_{},*size_{},*opacity_{},*range_{},*password_{};
    lumen::ComboBox *encoding_{},*office_{};
    lumen::Column *fontGroup_{},*colorGroup_{},*strokeGroup_{},*fillGroup_{},*arrowGroup_{},*contentGroup_{};
    lumen::Label *propHint_{};
    lumen::TextBox *color_{},*fillColor_{},*lineWidth_{};
    lumen::ComboBox *lineStyle_{},*startEnding_{},*endEnding_{};
    lumen::CheckBox *fill_{};
    lumen::Button *applyProps_{};
    std::vector<std::pair<lumen::Button*,uint32_t>> colorButtons_;
    Tool currentTool_{Tool::Select},lastMarkup_{Tool::Highlight},lastDrawing_{Tool::Rectangle};
    lumen::Button* handButton_{};
    lumen::Row* sidebar_{};         // 侧栏外壳（内容列 + 拖动条），显示 / 隐藏作用于整体
    lumen::Column* sideColumn_{};   // 侧栏内容列，宽度由拖动条或设置决定
    lumen::Row* footer_{};
    std::array<AnnotationStyle,ToolCount> toolStyles_{};
    std::array<float,ToolCount> toolOpacities_{1,1,1,.45f,1,1,1,1,1,1,1,1,.9f};
    float defaultFont_{12};
    lumen::CheckBox *a4_{},*bookmarks_{};
    lumen::Button *undo_{},*redo_{},*save_{},*cancelButton_{};
    std::vector<lumen::Button*> modes_;
    std::vector<std::pair<lumen::Button*,Tool>> toolButtons_;
    Placement placement_;
    lumen::Button* signButton_{};
};
}