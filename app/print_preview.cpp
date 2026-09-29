// 打印预览：排版参数（页码、方向、每面页数、奇偶、小册子、灰度……）改动后，在工作线程重新渲染当前纸面。
// 预览与实际打印共用 LayoutSheet / PlanPrint，保证版面一致；纸张取默认打印机的默认纸张与不可打印边距。
#include "application.h"
#include "ui.h"
#include "core/platform.h"
#include "core/signature.h"
#include <lumen/Dialog.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <thread>

namespace lpdf {
using namespace lumen;
namespace {
// 纸面预览：深色底板上按比例居中显示一面纸，带投影。
class SheetPreview final:public Control{
public:
    SheetPreview(float w,float h):w_(w),h_(h){}
    void Image(Bitmap b){pixels_=std::move(b);bitmap_.Reset();Invalidate();}
    void Clear(){pixels_={};bitmap_.Reset();Invalidate();}
    const Bitmap& Pixels()const noexcept{return pixels_;}
    std::wstring AutomationName()const override{return L"打印预览";}
protected:
    Size Measure(Size,const Theme&)override{return {w_,h_};}
    void Draw(Painter& p,const Theme&)override{
        p.FillRoundedRect(absolute_,8,Color::Hex(0x262626));
        const auto box=absolute_.Inset(16,16);
        if(pixels_.bgra.empty()){
            const float f=std::min(box.w/210.0f,box.h/297.0f),w=210*f,h=297*f;
            p.FillRoundedRect({box.x+(box.w-w)/2,box.y+(box.h-h)/2,w,h},2,Color::Hex(0x363636));
            return;
        }
        if(!bitmap_||device_!=p.DeviceIdentity()){
            bitmap_.Attach(p.CreateBitmapBgra(pixels_.width,pixels_.height,pixels_.bgra.data(),pixels_.stride));device_=p.DeviceIdentity();
        }
        if(!bitmap_)return;
        const float f=std::min(box.w/pixels_.width,box.h/pixels_.height);
        const float w=pixels_.width*f,h=pixels_.height*f,x=box.x+(box.w-w)/2,y=box.y+(box.h-h)/2;
        p.FillRoundedRect({x+2,y+3,w,h},2,Color::Hex(0x121212));
        p.DrawBitmap(bitmap_.Get(),{x,y,w,h},true);
    }
    bool HitTransparent()const noexcept override{return true;}
private:
    float w_,h_;Bitmap pixels_;Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap_;void* device_{};
};
constexpr int kPerSheet[]={1,2,4,6,9,16};
constexpr int kPreviewLongSide=760;   // 约为预览框高度的 2 倍，高 DPI 下仍清晰
}

struct PrintPreviewState {
    ComboBox *range{},*orient{},*nup{},*parity{},*side{};TextBox* custom{};
    CheckBox *annotations{},*gray{},*reverse{},*fit{},*rotate{},*booklet{};
    SheetPreview* view{};Label *position{},*summary{},*paper{};Button *prev{},*next{};
    PrintSettings settings;std::vector<PrintSheet> sheets;int sheet{};bool landscape{};std::wstring error;
    bool open{true},ready{};int renders{};
    std::shared_ptr<std::atomic<uint64_t>> latest=std::make_shared<std::atomic<uint64_t>>(0);
};

void Application::PrintOptions(){
    if(!loaded_||busy_||window_.DialogActive())return;
    const int count=static_cast<int>(info_.pages.size());if(count<=0)return;
    auto st=std::make_shared<PrintPreviewState>();st->settings=printSettings_;printPreview_=st;
    // 预览 + 选项并排需要比“宽”档（560）更宽的卡片。
    auto dialog=std::make_unique<Dialog>();
    dialog->Title(L"打印预览").Message(L"预览与实际打印使用同一套排版计算。下一步在系统对话框中确认打印机与份数；在那里更改纸张大小或方向，输出会与预览不同。");
    dialog->CardWidth(780);
    auto content=[this,st](Panel& p){
        const auto& s=st->settings;
        auto& main=p.Add<Row>();main.Spacing(18);main.AlignCross(CrossAlign::Start);
        auto& left=main.Add<Column>();left.Spacing(6);
        st->view=&left.Add<SheetPreview>(290.0f,352.0f);
        auto& nav=left.Add<Row>();nav.Spacing(6);nav.AlignCross(CrossAlign::Center);
        st->prev=&nav.Add<Button>(L"‹",ButtonKind::Subtle);st->prev->AccessibleName(L"上一面").ToolTip(L"上一面");
        st->position=&nav.Add<Label>(L"",TextRole::Caption);st->position->Grow();
        st->next=&nav.Add<Button>(L"›",ButtonKind::Subtle);st->next->AccessibleName(L"下一面").ToolTip(L"下一面");

        auto& right=main.Add<Column>();right.Spacing(8);right.Grow();
        auto row=[&](const wchar_t* label)->Row&{
            auto& r=right.Add<Row>();r.Spacing(8);r.AlignCross(CrossAlign::Center);
            r.Add<Label>(label,TextRole::Caption).Secondary(true).MinSize({56,30});return r;
        };
        st->range=&row(L"页码").Add<ComboBox>().AddItems({L"全部页面",L"当前页（第 "+std::to_wstring(page_+1)+L" 页）",L"自定义页码"}).SelectedIndex(std::clamp(s.rangeMode,0,2));
        st->range->Grow();
        auto& customRow=row(L"");
        st->custom=&customRow.Add<TextBox>();st->custom->Placeholder(L"例如 1-3,5,8").Text(s.range);st->custom->Grow();
        st->orient=&row(L"方向").Add<ComboBox>().AddItems({L"自动（按版面选择）",L"纵向",L"横向"}).SelectedIndex(std::clamp(s.orientation,0,2));
        st->orient->Grow();
        int at=0;for(int i=0;i<6;++i)if(kPerSheet[i]==s.pagesPerSheet)at=i;
        st->nup=&row(L"每面").Add<ComboBox>().AddItems({L"1 页",L"2 页",L"4 页",L"6 页",L"9 页",L"16 页"}).SelectedIndex(at);st->nup->Grow();
        st->parity=&row(L"范围").Add<ComboBox>().AddItems({L"全部页面",L"仅奇数页（文档页码）",L"仅偶数页（文档页码）"}).SelectedIndex(std::clamp(s.parity,0,2));st->parity->Grow();
        auto& bookletRow=row(L"小册子");
        st->booklet=&bookletRow.Add<CheckBox>(L"");st->booklet->Checked(s.booklet);st->booklet->AccessibleName(L"小册子排版").ToolTip(L"对折装订：每张纸两面各放两页，自动排序（覆盖每面页数）");
        st->side=&bookletRow.Add<ComboBox>().AddItems({L"双面打印机",L"仅正面（手动双面第 1 步）",L"仅背面（手动双面第 2 步）"}).SelectedIndex(std::clamp(s.bookletSide,0,2));st->side->Grow();
        auto pair=[&](CheckBox*& a,const wchar_t* la,bool va,const wchar_t* ta,CheckBox*& b,const wchar_t* lb,bool vb,const wchar_t* tb){
            auto& r=right.Add<Row>();r.Spacing(16);
            a=&r.Add<CheckBox>(la);a->Checked(va);a->ToolTip(ta);a->MinSize({150,28});
            b=&r.Add<CheckBox>(lb);b->Checked(vb);b->ToolTip(tb);
        };
        pair(st->annotations,L"打印批注",s.annotations,L"取消后只打印页面内容；表单中可见的内容始终保留",st->gray,L"灰度",s.grayscale,L"按亮度转为灰度");
        pair(st->reverse,L"逆序",s.reverse,L"从最后一页开始打印",st->fit,L"适合纸张",s.fitToPaper,L"取消后只缩小超出纸张的页面，不放大");
        auto& r3=right.Add<Row>();
        st->rotate=&r3.Add<CheckBox>(L"自动旋转页面");st->rotate->Checked(s.autoRotate);st->rotate->ToolTip(L"页面与格子方向不一致时旋转 90° 以占满空间");
        st->summary=&right.Add<Label>(L"",TextRole::Caption);
        st->paper=&right.Add<Label>(L"正在读取默认打印机的纸张…",TextRole::Caption);st->paper->Secondary(true);

        // 控件全部创建后再连接事件，避免初始化时回调访问未创建的控件。
        auto refresh=[this]{PrintPreviewRefresh();};
        st->prev->OnClick([this]{PrintPreviewStep(-1);});st->next->OnClick([this]{PrintPreviewStep(1);});
        for(auto* c:{st->range,st->orient,st->nup,st->parity,st->side})c->OnSelectionChanged([refresh](ptrdiff_t,ptrdiff_t){refresh();});
        for(auto* c:{st->annotations,st->gray,st->reverse,st->fit,st->rotate,st->booklet})c->OnToggled([refresh](bool){refresh();});
        st->custom->OnTextChanged([refresh](std::wstring_view){refresh();});
        st->ready=true;
    };
    content(*dialog);
    dialog->PrimaryButton(L"选择打印机…").CloseButton(L"取消");
    dialog->OnResult([this,st](DialogResult result){
        if(result==DialogResult::Primary&&printPreview_==st){AcceptPrintPreview(false);return;}
        st->open=false;if(printPreview_==st)printPreview_.reset();
    });
    window_.ShowDialog(std::move(dialog));
    PrintPreviewRefresh();
    // 纸张：先用上次的结果，同时在工作线程重新查询默认打印机（网络打印机可能较慢）。
    auto post=window_.Dispatcher();auto alive=alive_;
    worker_->Submit([this,post,alive,st](Engine&){
        auto paper=std::make_shared<PaperInfo>(QueryPaper());
        post.Post([this,alive,st,paper]{
            if(!alive->load())return;
            const bool changed=!printPaper_||printPaper_->width!=paper->width||printPaper_->height!=paper->height||printPaper_->printer!=paper->printer
                ||printPaper_->left!=paper->left||printPaper_->top!=paper->top||printPaper_->right!=paper->right||printPaper_->bottom!=paper->bottom;
            printPaper_=*paper;
            if(st->open&&printPreview_==st)PrintPreviewRefresh(changed);
        });
    },false);
}

void Application::PrintPreviewStep(int delta){
    auto st=printPreview_;if(!st||!st->open||st->sheets.empty())return;
    st->sheet=std::clamp(st->sheet+delta,0,static_cast<int>(st->sheets.size())-1);
    PrintPreviewRefresh();
}

void Application::PrintPreviewRefresh(bool render){
    auto st=printPreview_;if(!st||!st->open||!st->ready)return;
    const int count=static_cast<int>(info_.pages.size());
    auto& s=st->settings;
    s.pagesPerSheet=kPerSheet[std::clamp<ptrdiff_t>(st->nup->SelectedIndex(),0,5)];
    s.parity=static_cast<int>(std::clamp<ptrdiff_t>(st->parity->SelectedIndex(),0,2));
    s.bookletSide=static_cast<int>(std::clamp<ptrdiff_t>(st->side->SelectedIndex(),0,2));
    s.orientation=static_cast<int>(std::clamp<ptrdiff_t>(st->orient->SelectedIndex(),0,2));
    s.rangeMode=static_cast<int>(std::clamp<ptrdiff_t>(st->range->SelectedIndex(),0,2));
    s.range=std::wstring(st->custom->Text());
    s.annotations=st->annotations->Checked();s.grayscale=st->gray->Checked();s.reverse=st->reverse->Checked();
    s.fitToPaper=st->fit->Checked();s.autoRotate=st->rotate->Checked();s.booklet=st->booklet->Checked();
    st->custom->Enabled(s.rangeMode==2);st->nup->Enabled(!s.booklet);st->side->Enabled(s.booklet);
    st->error.clear();s.pages.clear();st->sheets.clear();
    const std::wstring total=L"（共 "+std::to_wstring(count)+L" 页）";
    if(count<=0)st->error=L"文档没有页面";
    else if(s.rangeMode==1)s.pages={std::clamp(page_,0,count-1)};
    else if(s.rangeMode==2){
        const bool blank=std::all_of(s.range.begin(),s.range.end(),[](wchar_t c){return iswspace(c)!=0;});
        if(blank)st->error=L"请输入要打印的页码，例如 1-3,5"+total;
        else try{s.pages=ParsePageRange(s.range,count);}catch(const std::exception&){st->error=L"页码格式不正确，例如 1-3,5,8"+total;}
    }else{s.pages.resize(static_cast<size_t>(count));std::iota(s.pages.begin(),s.pages.end(),0);}
    if(st->error.empty()){
        try{st->sheets=PlanPrint(s);}catch(const std::exception&){st->error=L"无法排版这些页面";}
        if(st->error.empty()&&st->sheets.empty())st->error=L"按奇偶页筛选后没有要打印的页面";
    }
    st->landscape=count>0&&ResolveLandscape(s,info_.pages);
    const int sheets=static_cast<int>(st->sheets.size());
    st->sheet=std::clamp(st->sheet,0,std::max(0,sheets-1));
    st->position->Text(sheets?L"第 "+std::to_wstring(st->sheet+1)+L" / "+std::to_wstring(sheets)+L" 面":L"—");
    st->prev->Enabled(st->sheet>0);st->next->Enabled(st->sheet+1<sheets);
    if(!st->error.empty())st->summary->Text(st->error);
    else{
        int printed=0,blank=0;for(const auto& sheet:st->sheets)for(int p:sheet)(p>=0?printed:blank)++;
        std::wstring text=L"打印 "+std::to_wstring(printed)+L" 页，共 "+std::to_wstring(sheets)+L" 面";
        if(s.booklet)text+=s.bookletSide==0?L"（双面，"+std::to_wstring((sheets+1)/2)+L" 张纸）":L"（"+std::to_wstring(sheets)+L" 张纸的"+(s.bookletSide==1?L"正面）":L"背面）");
        if(blank)text+=L" · 含 "+std::to_wstring(blank)+L" 个空白位";
        st->summary->Text(text);
    }
    std::wstring paper;
    if(!printPaper_)paper=L"正在读取默认打印机的纸张…";
    else{
        paper=L"纸张："+PaperName(*printPaper_)+(st->landscape?L" 横向":L" 纵向")+(s.orientation==0?L"（自动）":L"");
        paper+=printPaper_->fromPrinter?L" · "+printPaper_->printer:printPaper_->printer.empty()?L" · 未找到打印机，按 A4 预览":L" · 无法读取打印机纸张，按 A4 预览";
    }
    st->paper->Text(paper);
    if(render)PrintPreviewRender();
}

void Application::PrintPreviewRender(){
    auto st=printPreview_;if(!st||!st->open||!st->view)return;
    if(st->sheets.empty()){++*st->latest;st->view->Clear();return;}
    if(!printPaper_)return;   // 纸张查询完成后会再次刷新
    const uint64_t generation=++*st->latest;
    const auto settings=st->settings;const auto sheet=st->sheets[static_cast<size_t>(st->sheet)];
    const auto paper=Oriented(*printPaper_,st->landscape);
    auto post=window_.Dispatcher();auto alive=alive_;auto latest=st->latest;
    worker_->Submit([post,alive,latest,generation,settings,sheet,paper,st](Engine& e){
        if(latest->load()!=generation||!e.open)return;   // 已有更新的请求：跳过过时的渲染
        auto bitmap=std::make_shared<Bitmap>();std::wstring error;
        try{*bitmap=RenderSheetPreview(e.document,settings,sheet,paper,kPreviewLongSide);}
        catch(const std::exception& ex){try{error=Wide(ex.what());}catch(...){error=L"未知错误";}}
        post.Post([alive,st,generation,bitmap,error]{
            if(!alive->load()||!st->open||st->latest->load()!=generation)return;
            if(error.empty()){st->view->Image(std::move(*bitmap));++st->renders;}
            else{st->view->Clear();st->summary->Text(L"预览渲染失败："+error);}
        });
    },false);
}

void Application::AcceptPrintPreview(bool closeDialog){
    auto st=printPreview_;if(!st)return;
    PrintPreviewRefresh(false);
    printSettings_=st->settings;
    const bool ok=!st->sheets.empty();const std::wstring why=st->error;
    st->open=false;printPreview_.reset();
    if(closeDialog&&window_.DialogActive())window_.CloseDialog();
    AfterDialog([this,ok,why]{
        if(!ok){window_.Alert(L"没有可打印页面",why.empty()?L"请检查页码范围与奇偶页设置。":why);return;}
        PrintExecute();
    });
}

// 打印预览冒烟：LPDF_PRINT_PREVIEW_SMOKE 指定输出目录。依次检查默认 / 4 合 1 / 小册子 / 自定义页码 2 合 1 的
// 纸面数与方向，保存预览 PNG 与 report.txt，最后以“Microsoft Print to PDF”打印到 print.pdf 后退出。
void Application::PrintPreviewSmoke(const fs::path& dir,int step,int waited,int seen){
    auto later=[this](float delay,std::function<void()> fn){
        auto post=window_.Dispatcher();auto alive=alive_;
        std::thread([post,alive,delay,fn]{std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(delay*1000)));post.Post([alive,fn]{if(alive->load())fn();});}).detach();
    };
    auto report=[dir](const std::wstring& line){FILE* f=nullptr;if(_wfopen_s(&f,(dir/L"report.txt").c_str(),L"a, ccs=UTF-8")==0&&f){fwprintf(f,L"%ls\n",line.c_str());fclose(f);}};
    auto fail=[this,report](const std::wstring& why){report(L"FAIL "+why+L" | 状态栏："+std::wstring(status_->Text()));closing_=true;window_.Close();};
    auto st=printPreview_;
    // 等待本步的预览渲染完成（renders 超过进入本步时的计数）。
    auto waitRender=[&]()->bool{
        if(st&&st->renders>seen&&!st->sheets.empty())return true;
        if(waited>60){fail(L"预览渲染超时，步骤 "+std::to_wstring(step));return false;}
        later(.25f,[this,dir,step,waited,seen]{PrintPreviewSmoke(dir,step,waited+1,seen);});return false;
    };
    auto sheetLine=[&](const wchar_t* name,const fs::path& png){
        SaveSignaturePng(st->view->Pixels(),dir/png);
        report(std::wstring(name)+L".sheets="+std::to_wstring(st->sheets.size()));
        report(std::wstring(name)+L".landscape="+(st->landscape?L"1":L"0"));
        report(std::wstring(name)+L".summary="+std::wstring(st->summary->Text()));
    };
    auto go=[&](int nextStep){const int base=st?st->renders:0;later(.05f,[this,dir,nextStep,base]{PrintPreviewSmoke(dir,nextStep,0,base);});};
    if(!loaded_||busy_){later(.3f,[this,dir,step,waited,seen]{PrintPreviewSmoke(dir,step,waited,seen);});return;}
    switch(step){
    case 0:
        PrintOptions();
        if(!printPreview_||!window_.DialogActive()){fail(L"打印预览对话框未打开");return;}
        report(L"paper="+std::wstring(printPreview_->paper->Text()));
        return go(1);
    case 1:
        if(!st){fail(L"预览状态丢失");return;}
        if(!waitRender())return;
        report(L"paper="+std::wstring(st->paper->Text()));
        sheetLine(L"default",L"default.png");
        st->nup->SelectedIndex(2);PrintPreviewRefresh();                      // 每面 4 页
        return go(2);
    case 2:
        if(!st||!waitRender())return;
        sheetLine(L"nup4",L"nup4.png");
        st->booklet->Checked(true);PrintPreviewRefresh();                     // 小册子
        return go(3);
    case 3:
        if(!st||!waitRender())return;
        sheetLine(L"booklet",L"booklet.png");
        if(GetEnvironmentVariableW(L"LPDF_PRINT_PREVIEW_HOLD",nullptr,0))return;   // 截图用：停在小册子预览
        PrintPreviewStep(1);report(L"booklet.step="+std::wstring(st->position->Text()));
        st->booklet->Checked(false);st->nup->SelectedIndex(1);st->range->SelectedIndex(2);
        st->custom->Text(L"9-");PrintPreviewRefresh(false);                  // 非法页码
        report(L"invalid.error="+std::wstring(st->error.empty()?L"":L"1"));
        report(L"invalid.sheets="+std::to_wstring(st->sheets.size()));
        st->custom->Text(L"1-3");st->gray->Checked(true);PrintPreviewRefresh(); // 2 合 1，第 1-3 页
        return go(4);
    case 4:{
        if(!st||!waitRender())return;
        sheetLine(L"final",L"final.png");
        const auto output=dir/L"print.pdf";
        SetEnvironmentVariableW(L"LPDF_PRINT_TO",output.c_str());
        AcceptPrintPreview(true);
        report(L"accepted=1");
        return later(1.0f,[this,dir]{PrintPreviewSmoke(dir,5,0,0);});
    }
    case 5:
        if(busy_||window_.DialogActive()){
            if(waited>80){fail(L"打印任务超时");return;}
            later(.25f,[this,dir,waited]{PrintPreviewSmoke(dir,5,waited+1,0);});return;
        }
        report(L"status="+std::wstring(status_->Text()));
        report(L"done=1");
        later(.5f,[this]{closing_=true;window_.Close();});
        return;
    default:return;
    }
}
}
