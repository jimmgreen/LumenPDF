// AcroForm 表单填写：单击高亮字段——复选框 / 单选直接切换，下拉 / 列表弹出选项，文本框弹出编辑框（可“保存并下一项”），
// 签名域用签名库里的签名填入。所有文档修改在工作线程执行，每次填写一步撤销。
#include "application.h"
#include "ui.h"
#include "core/signature.h"
#include <lumen/Dialog.h>
#include <lumen/Menu.h>
#include <algorithm>
#include <chrono>
#include <thread>

namespace lpdf {
using namespace lumen;
namespace {
std::wstring FieldTitle(const FormField& f){
    if(!f.label.empty())return f.label;
    if(!f.name.empty())return f.name;
    return L"表单字段";
}
}

void Application::FillField(int p,FormField f){
    if(!loaded_||busy_||window_.DialogActive())return;
    if(textEditor_.Active()){FinishText(true,[this,p,f]{FillField(p,f);});return;}
    if(f.readOnly){status_->Text(L"“"+FieldTitle(f)+L"”是只读字段");return;}
    switch(f.type){
    case FieldType::CheckBox:case FieldType::Radio:{
        const int id=f.id;const bool radio=f.type==FieldType::Radio;
        if(radio&&f.checked){status_->Text(L"已选中“"+FieldTitle(f)+L"”");return;}
        Task(radio?L"选择选项":L"切换复选框",[p,id](Engine& e,const Cancel&){e.document.ToggleField(p,id);},
            [this,f,radio]{status_->Text(radio?L"已选择：“"+FieldTitle(f)+L"”":(f.checked?L"已取消勾选：“":L"已勾选：“")+FieldTitle(f)+L"”");});
        return;
    }
    case FieldType::ComboBox:case FieldType::ListBox:{
        Menu menu;menu.AddHeader(FieldTitle(f));
        for(size_t i=0;i<f.options.size();++i){
            const auto value=f.exports[i];const auto shown=f.options[i];
            menu.AddItem(shown.empty()?L"（空）":shown,[this,p,f,value]{ApplyField(p,f,value,false);}).Radio(true).Checked(f.value==value||f.value==shown);
        }
        if(f.options.empty())menu.AddItem(L"没有可选项",{}).Disabled(true);
        if(f.type==FieldType::ComboBox&&f.editable){menu.AddSeparator();menu.AddItem(L"输入其它值…",[this,p,f]{EditFieldText(p,f,f.value);}).Glyph(ui::Text);}
        if(!f.value.empty()){menu.AddSeparator();menu.AddItem(L"清空",[this,p,f]{ApplyField(p,f,L"",false);}).Glyph(icon::kClose);}
        const auto box=canvas_->FieldScreenRect(p,f.bounds);
        menu.Popup(window_,{box.x,box.y+box.h+2});
        return;
    }
    case FieldType::Text:EditFieldText(p,f,f.value);return;
    case FieldType::Signature:{
        // 签名域：用签名库中的签名（透明图片批注）填入字段框内；这不是证书数字签名。
        Menu menu;menu.AddHeader(L"在签名域中放置签名");
        const auto entries=ListSignatures(SignatureFolder());
        for(const auto& e:entries){
            const auto file=e.file;
            menu.AddItem(e.kind==SignatureKind::Drawn?L"手写签名":e.kind==SignatureKind::Typed?L"文字签名":L"图片签名",[this,p,f,file]{
                auto box=f.bounds;
                // 留出少量边距，签名在框内按比例居中（图片批注自身保持比例，这里只做居中）。
                try{
                    const auto b=LoadSignaturePng(file);
                    const float inset=std::min(box.w,box.h)*.06f;box={box.x+inset,box.y+inset,box.w-inset*2,box.h-inset*2};
                    const float aspect=b.height>0?static_cast<float>(b.width)/b.height:3.0f;
                    float w=box.w,h=w/aspect;if(h>box.h){h=box.h;w=h*aspect;}
                    box={box.x+(box.w-w)/2,box.y+(box.h-h)/2,w,h};
                }catch(...){window_.Alert(L"签名文件无法读取",L"请在“签名 → 管理签名”中删除后重新创建。");return;}
                const float opacity=toolOpacities_[static_cast<size_t>(Tool::Image)];const auto style=toolStyles_[static_cast<size_t>(Tool::Image)];
                Task(L"放置签名",[p,box,file,opacity,style](Engine& e,const Cancel&){e.document.AddAnnotation(p,Tool::Image,box,{},file,{},12,opacity,style);},
                    [this,p]{SelectCreated(p,false);status_->Text(L"签名已放入签名域（签名图像，不是证书数字签名）");});
            }).Glyph(ui::Sign);
        }
        if(entries.empty())menu.AddItem(L"签名库为空",{}).Disabled(true);
        menu.AddSeparator();
        const bool full=entries.size()>=MaxSignatures;
        menu.AddItem(L"手写新签名…",[this]{DrawSignature();}).Disabled(full).Glyph(ui::Ink);
        menu.AddItem(L"输入文字签名…",[this]{TypeSignature();}).Disabled(full).Glyph(ui::Text);
        const auto box=canvas_->FieldScreenRect(p,f.bounds);
        menu.Popup(window_,{box.x,box.y+box.h+2});
        return;
    }
    default:status_->Text(L"这个表单按钮需要脚本支持，本程序不执行 PDF 脚本");return;
    }
}

void Application::EditFieldText(int p,FormField f,std::wstring draft,std::wstring error){
    if(!loaded_||busy_||window_.DialogActive())return;
    struct Controls{TextBox* text{};};
    auto c=std::make_shared<Controls>();
    DialogSpec dialog;dialog.title=FieldTitle(f);
    std::wstring message=error.empty()?L"":L"⚠ "+error+L"\n";
    message+=L"字段名：";message+=f.name.empty()?L"（未命名）":f.name;
    if(f.maxLength>0)message+=L"  ·  最多 "+std::to_wstring(f.maxLength)+L" 个字符";
    if(f.required)message+=L"  ·  必填";
    if(f.multiline)message+=L"  ·  多行（Enter 换行）";
    else message+=L"  ·  Enter 保存";
    dialog.message=message;dialog.size=DialogSize::Standard;dialog.default_button=f.multiline?DialogCommand::Auto:DialogCommand::Primary;
    dialog.content=[c,f,draft](Panel& panel){
        c->text=&panel.Add<TextBox>();
        c->text->Text(draft);c->text->Placeholder(f.type==FieldType::ComboBox?L"输入值":L"在此输入");c->text->AccessibleName(FieldTitle(f));
        if(f.multiline){c->text->Multiline(true);c->text->WordWrap(true);c->text->MinSize({0,120});}
        if(f.password)c->text->Password(true);
        c->text->Focus();
    };
    dialog.primary={L"保存",{}};dialog.secondary={L"保存并下一项",{}};dialog.close={L"取消",{}};
    dialog.on_result=[this,p,f,c](DialogResult result){
        if(result!=DialogResult::Primary&&result!=DialogResult::Secondary){status_->Text(L"已取消，表单未更改");return;}
        auto value=c->text->Text();
        if(!f.multiline)value.erase(std::remove_if(value.begin(),value.end(),[](wchar_t ch){return ch==L'\r'||ch==L'\n';}),value.end());
        if(f.maxLength>0&&value.size()>static_cast<size_t>(f.maxLength)){
            // 对话框关闭动画结束后才能再打开；保留已输入内容，把原因写在新对话框里。
            const auto why=L"内容过长：最多 "+std::to_wstring(f.maxLength)+L" 个字符，当前 "+std::to_wstring(value.size())+L" 个";
            AfterDialog([this,p,f,value,why]{EditFieldText(p,f,value,why);},[this,why]{status_->Text(why);});return;
        }
        ApplyField(p,f,value,result==DialogResult::Secondary);
    };
    window_.ShowDialog(std::move(dialog));
}

void Application::ApplyField(int p,FormField f,std::wstring value,bool next){
    auto following=std::make_shared<std::optional<FormField>>();
    const bool changed=value!=f.value;
    Task(L"填写表单",[p,f,value,next,changed,following](Engine& e,const Cancel&){
        if(changed)e.document.SetFieldValue(p,f.id,value);
        if(next)*following=e.document.NextFillableField(p,f.id);
    },[this,f,next,changed,following]{
        status_->Text(changed?L"已填写：“"+FieldTitle(f)+L"”":L"内容未变化");
        if(!next)return;
        if(!*following){status_->Text(L"没有其它可填写的字段了");return;}
        const auto target=**following;
        if(target.page!=page_)Page(target.page);
        canvas_->RevealRect(target.page,target.bounds);
        // 复选框 / 单选只定位不代为勾选；文本 / 下拉在上一个对话框关闭后打开。
        if(target.type==FieldType::CheckBox||target.type==FieldType::Radio||target.type==FieldType::Signature){
            status_->Text(L"下一项：“"+FieldTitle(target)+L"”  ·  单击即可填写");return;}
        AfterDialog([this,target]{FillField(target.page,target);});
    },{},[this,p,f,value,next](std::wstring error){
        const auto why=L"值未写入："+error;
        if(f.type==FieldType::Text||(f.type==FieldType::ComboBox&&f.editable))AfterDialog([this,p,f,value,why]{EditFieldText(p,f,value,why);},[this,why]{status_->Text(why);});
        else window_.Alert(L"无法填写这个字段",L"值未写入："+error);
    });
}

// 上一个对话框关闭动画结束（且没有任务在跑）后再执行；约 2 秒内仍不空闲则执行 fallback。
// 用后台线程投递普通消息轮询，不用定时器——对话框动画期间 WM_TIMER 会被推迟。
void Application::AfterDialog(std::function<void()> action,std::function<void()> fallback,int attempt){
    if(!window_.DialogActive()&&!busy_){action();return;}
    if(attempt>=40){if(fallback)fallback();return;}
    auto post=window_.Dispatcher();auto alive=alive_;
    std::thread([this,post,alive,action=std::move(action),fallback=std::move(fallback),attempt]()mutable{
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        post.Post([this,alive,action=std::move(action),fallback=std::move(fallback),attempt]()mutable{if(alive->load())AfterDialog(std::move(action),std::move(fallback),attempt+1);});
    }).detach();
}

void Application::ResetFormFields(){
    if(!loaded_||busy_||!info_.hasForm)return;
    window_.Confirm(L"重置表单？",L"所有表单字段恢复为默认值（通常为空）。可以撤销。",[this](bool yes){
        if(yes)Task(L"重置表单",[](Engine& e,const Cancel&){e.document.ResetForm();},[this]{status_->Text(L"表单已重置  ·  Ctrl+Z 撤销");});
    },L"重置",L"取消");
}

// 自动化冒烟（--smoke 且 LPDF_FORM_SMOKE=<输出 PDF>）：模拟画布单击回调，走 切换 / 选项 / 文本 / 下一项 的真实链路后另存。
void Application::FormSmoke(const fs::path& output,int step){
    // 对话框打开时 WM_TIMER 会被动画推迟，冒烟用后台线程按时投递普通消息推进。
    auto later=[this](float delay,std::function<void()> fn){
        auto post=window_.Dispatcher();auto alive=alive_;
        std::thread([post,alive,delay,fn]{std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(delay*1000)));post.Post([alive,fn]{if(alive->load())fn();});}).detach();
    };
    auto again=[this,later,output,step](float delay){later(delay,[this,output,step]{FormSmoke(output,step);});};
    auto next=[this,later,output,step](float delay){later(delay,[this,output,step]{FormSmoke(output,step+1);});};
    auto fail=[this,output](const wchar_t* why){
        status_->Text(std::wstring(L"表单冒烟失败：")+why);
        FILE* file=nullptr;if(_wfopen_s(&file,(output.wstring()+L".fail.txt").c_str(),L"w, ccs=UTF-8")==0&&file){fwprintf(file,L"%ls | 状态栏：%ls | busy=%d dialog=%d page=%d\n",why,std::wstring(status_->Text()).c_str(),busy_?1:0,window_.DialogActive()?1:0,page_);fclose(file);}
        closing_=true;window_.Close();};
    if(!loaded_||busy_){again(.3f);return;}
    auto fields=std::make_shared<std::vector<FormField>>();
    auto find=[fields](std::wstring_view name,int nth=0)->std::optional<FormField>{for(const auto& f:*fields)if(f.name==name&&nth--==0)return f;return std::nullopt;};
    // 每步先在工作线程读取第 1 页字段，再操作。
    Task(L"读取表单",[fields](Engine& e,const Cancel&){*fields=e.document.FormFields(0);},[this,output,step,fields,find,next,fail,later]{
        switch(step){
        case 0:{
            if(!info_.hasForm){fail(L"未识别表单");return;}
            auto agree=find(L"agree");auto pro=find(L"plan",1);
            if(!agree||!pro){fail(L"缺少字段");return;}
            FillField(0,*agree);
            later(.6f,[this,pro]{FillField(0,*pro);});
            return next(1.4f);
        }
        case 1:{
            auto agree=find(L"agree");auto pro=find(L"plan",1);
            if(!agree||!agree->checked||!pro||!pro->checked){fail(L"复选框 / 单选");return;}
            auto city=find(L"city");if(!city){fail(L"缺少下拉框");return;}
            ApplyField(0,*city,L"sh",false);   // 等价于在弹出菜单里选择 “Shanghai”
            return next(.8f);
        }
        case 2:{
            auto city=find(L"city");if(!city||city->value!=L"sh"){fail(L"下拉框");return;}
            auto name=find(L"name");if(!name){fail(L"缺少文本框");return;}
            // 文本框：打开编辑对话框验证其能正常显示，再按“保存并下一项”的同一路径提交。
            FillField(0,*name);
            if(!window_.DialogActive()){fail(L"文本对话框未打开");return;}
            window_.CloseDialog();
            ApplyField(0,*name,L"张三",true);   // 与点击“保存并下一项”相同：对话框仍在关闭时提交
            return next(1.5f);
        }
        case 3:{
            auto name=find(L"name");if(!name||name->value!=L"张三"){fail(L"文本框");return;}
            // “保存并下一项”应打开下一个字段（notes）的对话框。
            if(!window_.DialogActive()){fail(L"未打开下一项");return;}
            window_.CloseDialog();
            // 超长输入：应保留输入并重新打开对话框（不丢字）。
            ApplyField(0,*name,std::wstring(25,L'x'),false);
            return next(1.2f);
        }
        case 4:{
            auto name=find(L"name");if(!name||name->value!=L"张三"){fail(L"超长值被写入");return;}
            if(!window_.DialogActive()){fail(L"超长值未重新打开对话框");return;}
            window_.CloseDialog();
            return next(1.0f);
        }
        case 5:{
            if(window_.DialogActive()){fail(L"对话框未关闭");return;}
            auto account=find(L"account");
            if(!account||!account->readOnly){fail(L"缺少只读字段");return;}
            FillField(0,*account);if(window_.DialogActive()){fail(L"只读字段打开了编辑框");return;}
            Task(L"保存冒烟结果",[output](Engine& e,const Cancel&){e.document.Save(output);},[this]{closing_=true;window_.Close();});
            return;
        }
        default:return;
        }
    });
}
}
