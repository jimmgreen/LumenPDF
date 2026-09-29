// 朗读：用 Windows 自带语音从当前页 / 选中文字开始朗读，跟读高亮当前句并自动翻到它；可暂停、跳句、调语速、选声音。
// 取文字在引擎线程（按句切分带逐行框），语音在 SpeechPlayer 自己的线程；这里只在界面线程上编排。
#include "application.h"
#include "speech.h"
#include "ui.h"
#include <chrono>
#include <cstdio>
#include <thread>

namespace lpdf {
using namespace lumen;

SpeechPlayer& Application::Speech(){
    if(speech_)return *speech_;
    speech_=std::make_unique<SpeechPlayer>();
    auto post=window_.Dispatcher();auto alive=alive_;
    speech_->started=[this,post,alive](uint64_t session,int index,std::wstring voice){post.Post([this,alive,session,index,voice]{if(alive->load())ReadSentence(session,index,voice);});};
    speech_->finished=[this,post,alive](uint64_t session){post.Post([this,alive,session]{if(alive->load())ReadFinished(session);});};
    speech_->failed=[this,post,alive](uint64_t session,std::wstring message){post.Post([this,alive,session,message]{
        if(!alive->load()||!aloud_.active||session!=aloud_.session)return;
        StopReading(true);status_->Text(L"朗读失败："+message);
    });};
    // 测试：LPDF_SPEECH_WAV 把语音写进文件，不出声。
    wchar_t wav[MAX_PATH]{};if(GetEnvironmentVariableW(L"LPDF_SPEECH_WAV",wav,MAX_PATH))speech_->OutputFile(wav);
    speech_->Rate(settings_.speechRate);speech_->Voice(Wide(settings_.speechVoice));
    return *speech_;
}

bool Application::CanReadAloud()const{return loaded_&&!home_&&mode_!=3&&!window_.DialogActive();}

void Application::ReadAloud(bool toEnd){
    if(!CanReadAloud()||info_.pages.empty())return;
    if(textEditor_.Active()){FinishText(true,[this,toEnd]{ReadAloud(toEnd);});return;}
    if(mode_==2)Mode(0);
    StopReading(true);
    aloud_.active=true;aloud_.toEnd=toEnd;aloud_.document=activeDocument_?activeDocument_->id:0;aloudPages_.clear();
    status_->Text(L"正在准备朗读…");
    ReadPage(std::clamp(page_,0,static_cast<int>(info_.pages.size())-1));
}

void Application::ReadSelection(){
    if(!CanReadAloud()||!canvas_->HasTextSelection())return;
    const auto selection=canvas_->Selected();
    StopReading(true);
    aloud_.active=true;aloud_.selection=true;aloud_.page=selection.page;aloud_.document=activeDocument_?activeDocument_->id:0;aloudPages_.clear();
    const uint64_t ticket=++aloud_.ticket;auto post=window_.Dispatcher();auto alive=alive_;
    worker_->Submit([this,post,alive,selection,ticket](Engine& e){
        std::wstring text,error;
        if(e.open)try{text=SelectedText(e.document,selection);}catch(const std::exception& ex){try{error=Wide(ex.what());}catch(...){error=L"无法读取页面文字";}}
        post.Post([this,alive,ticket,text,error]{
            if(!alive->load()||!aloud_.active||ticket!=aloud_.ticket)return;
            auto sentences=SplitSentences(text);
            if(!error.empty()||sentences.empty()){StopReading(true);status_->Text(error.empty()?L"选中的内容里没有可朗读的文字":L"朗读失败："+error);return;}
            aloud_.sentences.clear();for(const auto& s:sentences)aloud_.sentences.push_back({s,{}});
            aloud_.session=Speech().Play(std::move(sentences));
        });
    },false);
}

void Application::ReadPage(int page){
    aloud_.page=page;
    const uint64_t ticket=++aloud_.ticket;auto post=window_.Dispatcher();auto alive=alive_;
    worker_->Submit([this,post,alive,page,ticket](Engine& e){
        std::vector<ReadingSentence> sentences;std::wstring error;
        if(e.open)try{sentences=e.document.ReadingSentences(page);}catch(const std::exception& ex){try{error=Wide(ex.what());}catch(...){error=L"无法读取页面文字";}}
        post.Post([this,alive,page,ticket,sentences=std::move(sentences),error]()mutable{if(alive->load())ReadFetched(page,ticket,std::move(sentences),error);});
    },false);
}

void Application::ReadFetched(int page,uint64_t ticket,std::vector<ReadingSentence> sentences,const std::wstring& error){
    if(!aloud_.active||ticket!=aloud_.ticket)return;
    if((activeDocument_?activeDocument_->id:0)!=aloud_.document){StopReading(true);return;}
    if(!error.empty()){StopReading(true);status_->Text(L"朗读失败："+error);return;}
    if(sentences.empty()){
        // 没有文字层的页面（扫描件、空白页）：朗读到文末时跳过，否则提示。
        if(aloud_.toEnd&&page+1<static_cast<int>(info_.pages.size())){++aloud_.skipped;ReadPage(page+1);return;}
        const bool skipped=aloud_.skipped>0;StopReading(true);
        status_->Text(skipped?L"朗读完毕（后面的页面没有可朗读的文字）":L"这一页没有可朗读的文字（扫描件需要先识别文字）");
        return;
    }
    aloud_.sentences=std::move(sentences);aloud_.index=-1;
    std::vector<std::wstring> texts;texts.reserve(aloud_.sentences.size());for(const auto& s:aloud_.sentences)texts.push_back(s.text);
    aloud_.session=Speech().Play(std::move(texts));
}

void Application::ReadSentence(uint64_t session,int index,const std::wstring& voice){
    if(!aloud_.active||session!=aloud_.session)return;
    if((activeDocument_?activeDocument_->id:0)!=aloud_.document){StopReading(true);return;}
    aloud_.index=index;aloud_.voice=voice;
    if(!aloud_.selection&&index>=0&&index<static_cast<int>(aloud_.sentences.size())){
        if(aloudPages_.empty()||aloudPages_.back()!=aloud_.page)aloudPages_.push_back(aloud_.page);
        canvas_->SpeechHighlight(aloud_.page,aloud_.sentences[index].boxes,mode_!=2);
    }
    ReadStatus();
}

void Application::ReadFinished(uint64_t session){
    if(!aloud_.active||session!=aloud_.session)return;
    if(aloud_.toEnd&&!aloud_.selection&&aloud_.page+1<static_cast<int>(info_.pages.size())){ReadPage(aloud_.page+1);return;}
    StopReading(true);status_->Text(L"朗读完毕");
}

void Application::ReadPause(){
    if(!aloud_.active||!speech_)return;
    speech_->Pause(!speech_->Paused());ReadStatus();
}

void Application::ReadSkip(int delta){
    if(!aloud_.active||!speech_)return;
    // 本页第一句再往前：回到上一页的开头（朗读到文末时）。
    if(delta<0&&aloud_.index<=0&&aloud_.toEnd&&!aloud_.selection&&aloud_.page>0){aloud_.skipped=0;ReadPage(aloud_.page-1);return;}
    speech_->Skip(delta);
}

void Application::StopReading(bool quiet){
    const bool was=aloud_.active;
    if(speech_)speech_->Stop();
    const uint64_t ticket=aloud_.ticket+1;aloud_=Aloud{};aloud_.ticket=ticket;
    canvas_->SpeechHighlight(-1,{},false);
    if(was&&!quiet)status_->Text(L"朗读已停止");
}

void Application::ReadRate(int rate){
    settings_.speechRate=std::clamp(rate,-10,10);SaveSettings();
    Speech().Rate(settings_.speechRate);
    if(aloud_.active)ReadStatus();else status_->Text(L"朗读语速："+RateName(settings_.speechRate)+L"（下一句起生效）");
}

void Application::ReadVoice(const std::wstring& id){
    settings_.speechVoice=Utf8(id);SaveSettings();Speech().Voice(id);
    status_->Text(id.empty()?L"朗读声音：自动（中文用中文语音，英文用英文语音）":L"朗读声音已更改（下一句起生效）");
}

std::wstring Application::RateName(int rate){
    if(rate<=-5)return L"很慢";if(rate<0)return L"慢";if(rate==0)return L"正常";if(rate<5)return L"快";return L"很快";
}

void Application::ReadStatus(){
    if(!aloud_.active)return;
    const bool paused=speech_&&speech_->Paused();
    std::wstring text=paused?L"朗读已暂停":L"朗读中";
    if(aloud_.selection)text+=L" · 选中文字";else text+=L" · 第 "+std::to_wstring(aloud_.page+1)+L" 页";
    if(aloud_.index>=0)text+=L" 第 "+std::to_wstring(aloud_.index+1)+L"/"+std::to_wstring(aloud_.sentences.size())+L" 句";
    if(!aloud_.voice.empty())text+=L" · "+aloud_.voice.substr(0,aloud_.voice.find(L" - "));
    text+=L" · 语速"+RateName(settings_.speechRate)+(paused?L"  ·  Ctrl+Shift+C 继续 · Esc 停止":L"  ·  Ctrl+Shift+C 暂停 · Esc 停止");
    status_->Text(text);
}

void Application::ShowReadMenu(){
    if(!loaded_)return;
    Menu menu;
    if(!aloud_.active){
        menu.AddItem(L"朗读本页",[this]{ReadAloud(false);}).Shortcut(L"Ctrl+Shift+V").Disabled(!CanReadAloud());
        menu.AddItem(L"从本页朗读到文末",[this]{ReadAloud(true);}).Shortcut(L"Ctrl+Shift+B").Disabled(!CanReadAloud());
        if(canvas_->HasTextSelection())menu.AddItem(L"朗读选中文字",[this]{ReadSelection();});
    }else{
        const bool paused=speech_&&speech_->Paused();
        menu.AddItem(paused?L"继续朗读":L"暂停朗读",[this]{ReadPause();}).Shortcut(L"Ctrl+Shift+C");
        menu.AddItem(L"上一句",[this]{ReadSkip(-1);});
        menu.AddItem(L"下一句",[this]{ReadSkip(1);});
        menu.AddItem(L"停止朗读",[this]{StopReading();}).Shortcut(L"Esc");
    }
    menu.AddSeparator();
    menu.AddHeader(L"语速");
    for(int rate:{-6,-3,0,3,6})menu.AddItem(RateName(rate),[this,rate]{ReadRate(rate);}).RadioGroup(L"rate").Checked(RateName(settings_.speechRate)==RateName(rate));
    menu.AddHeader(L"声音");
    const std::wstring current=Wide(settings_.speechVoice);
    menu.AddItem(L"自动（按文字选中文 / 英文语音）",[this]{ReadVoice(L"");}).RadioGroup(L"voice").Checked(current.empty());
    for(const auto& v:InstalledVoices())menu.AddItem(v.name,[this,id=v.id]{ReadVoice(id);}).RadioGroup(L"voice").Checked(current==v.id);
    menu.PopupTo(*moreButton_);
}

// 冒烟：LPDF_SPEECH_SMOKE 指定输出目录（测试同时设置 LPDF_SPEECH_WAV，语音写入文件、不出声）。
// LPDF_SPEECH_HOLD=1：第一句开始后暂停并停住（供截图）。
void Application::SpeechSmoke(const fs::path& dir,int step,int waited){
    auto report=[dir](const std::wstring& line){FILE* f=nullptr;if(_wfopen_s(&f,(dir/L"report.txt").c_str(),L"a, ccs=UTF-8")==0&&f){fwprintf(f,L"%ls\n",line.c_str());fclose(f);}};
    auto later=[this,dir](float delay,int next,int count){
        auto post=window_.Dispatcher();auto alive=alive_;
        std::thread([post,alive,delay,next,count,this,dir]{std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(delay*1000)));post.Post([alive,next,count,this,dir]{if(alive->load())SpeechSmoke(dir,next,count);});}).detach();
    };
    auto fail=[this,report](const std::wstring& why){report(L"FAIL "+why+L" | 状态栏："+std::wstring(status_->Text()));closing_=true;window_.Close();};
    auto has=[this](const wchar_t* text){return std::wstring(status_->Text()).find(text)!=std::wstring::npos?L"1":L"0";};
    if(!loaded_||busy_){later(.3f,step,waited);return;}
    if(waited>150){fail(L"等待超时，步骤 "+std::to_wstring(step));return;}
    wchar_t hold[4]{};const bool holding=GetEnvironmentVariableW(L"LPDF_SPEECH_HOLD",hold,4)>0;
    switch(step){
    case 0:
        Mode(0);canvas_->FitWidth();Speech().Rate(10);   // 只改本次播放的语速，不写入用户设置
        report(L"pages="+std::to_wstring(info_.pages.size()));
        ReadAloud(true);
        return later(.2f,1,0);
    case 1:
        if(aloud_.index<0)return later(.1f,1,waited+1);
        report(L"first.page="+std::to_wstring(aloud_.page));report(L"highlight="+std::to_wstring(canvas_->SpeechPage()));
        report(L"reading.status="+std::wstring(has(L"朗读中")));
        ReadPause();report(L"paused="+std::wstring(speech_->Paused()?L"1":L"0"));report(L"paused.status="+std::wstring(has(L"已暂停")));
        if(holding)return;
        ReadPause();report(L"resumed="+std::wstring(speech_->Paused()?L"0":L"1"));
        return later(.3f,2,0);
    case 2:
        if(aloud_.active)return later(.25f,2,waited+1);
        {std::wstring pages;for(int p:aloudPages_)pages+=(pages.empty()?L"":L",")+std::to_wstring(p);report(L"visited="+pages);}
        report(L"finished.status="+std::wstring(has(L"朗读完毕")));report(L"cleared="+std::wstring(canvas_->SpeechPage()<0?L"1":L"0"));
        report(L"followed="+std::wstring(page_==static_cast<int>(info_.pages.size())-1?L"1":L"0"));
        Page(0,true);ReadAloud(false);
        return later(.2f,3,0);
    case 3:
        if(aloud_.index<0)return later(.1f,3,waited+1);
        ReadSkip(2);
        return later(.2f,4,0);
    case 4:
        if(aloud_.index<2)return later(.05f,4,waited+1);
        report(L"skip.index="+std::to_wstring(aloud_.index));
        StopReading();
        report(L"stopped="+std::wstring(!aloud_.active&&canvas_->SpeechPage()<0?L"1":L"0"));report(L"stopped.status="+std::wstring(has(L"朗读已停止")));
        report(L"dirty="+std::wstring(info_.dirty?L"1":L"0"));
        report(L"done=1");closing_=true;window_.Close();
        return;
    default:return;
    }
}
}
