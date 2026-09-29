#include "speech.h"
#include <windows.h>
#include <sapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <climits>
#include <cwctype>
#include <cstdio>

namespace lpdf {
using Microsoft::WRL::ComPtr;

namespace {
struct ComScope {
    HRESULT hr;
    ComScope():hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED)){}
    ~ComScope(){if(SUCCEEDED(hr))CoUninitialize();}
};
std::wstring Hex(HRESULT hr){wchar_t text[16]{};swprintf_s(text,L"0x%08lX",static_cast<unsigned long>(hr));return text;}
std::wstring TakeString(LPWSTR s){std::wstring out=s?s:L"";if(s)CoTaskMemFree(s);return out;}
bool ChineseLanguage(const std::wstring& list){
    // Attributes\Language 是以分号分隔的十六进制 LANGID，例如 "804" 或 "409;9"。
    size_t start=0;
    while(start<=list.size()){
        const size_t end=std::min(list.find(L';',start),list.size());
        const auto part=list.substr(start,end-start);
        if(!part.empty()){wchar_t* stop=nullptr;const unsigned long id=wcstoul(part.c_str(),&stop,16);if(stop!=part.c_str()&&PRIMARYLANGID(static_cast<LANGID>(id))==LANG_CHINESE)return true;}
        start=end+1;
    }
    return false;
}
std::wstring ShortName(std::wstring name){
    for(const wchar_t* tail:{L" Desktop"})if(const auto at=name.find(tail);at!=std::wstring::npos)name.erase(at,wcslen(tail));
    return name;
}
bool Terminator(wchar_t c){return c==0x3002||c==0xFF01||c==0xFF1F||c==0xFF1B||c==0x2026||c==L'!'||c==L'?'||c==L';';}
bool Closing(wchar_t c){return c==0x201D||c==0x2019||c==L'"'||c==L'\''||c==0x300D||c==0x300F||c==L')'||c==0xFF09||c==0x300B;}
bool Cjk(wchar_t c){return (c>=0x2E80&&c<=0x9FFF)||(c>=0xF900&&c<=0xFAFF)||(c>=0xFF00&&c<=0xFFEF)||(c>=0x3000&&c<=0x303F);}
}

bool ContainsChinese(std::wstring_view text){
    for(wchar_t c:text)if((c>=0x4E00&&c<=0x9FFF)||(c>=0x3400&&c<=0x4DBF)||(c>=0xF900&&c<=0xFAFF))return true;
    return false;
}

std::vector<std::wstring> SplitSentences(std::wstring_view text){
    std::vector<std::wstring> out;std::wstring current;
    auto finish=[&]{
        while(!current.empty()&&iswspace(current.back()))current.pop_back();
        size_t lead=0;while(lead<current.size()&&iswspace(current[lead]))++lead;current.erase(0,lead);
        bool speakable=false;for(wchar_t ch:current)if(iswalnum(ch)||ch>=0x2E80){speakable=true;break;}
        if(speakable)out.push_back(current);
        current.clear();
    };
    for(size_t i=0;i<text.size();++i){
        wchar_t c=text[i];
        if(c==L'\r')continue;
        if(c==L'\n'){
            if(i+1<text.size()&&text[i+1]==L'\n'){finish();continue;}   // 空行 = 段落
            if(!current.empty()){
                const wchar_t last=current.back();
                if(last==L'-'&&current.size()>1&&iswalpha(current[current.size()-2]))current.pop_back();
                else if(!Cjk(last)&&last!=L' ')current+=L' ';
            }
            continue;
        }
        if(c==L'\t')c=L' ';
        if(c==L' '&&current.empty())continue;
        current+=c;
        const bool period=(c==L'.'||c==0xFF0E)&&(i+1>=text.size()||iswspace(text[i+1]));
        if(Terminator(c)||period){
            while(i+1<text.size()&&Closing(text[i+1]))current+=text[++i];
            finish();
        }else if(current.size()>=240&&(c==0xFF0C||c==L','||c==L' '||c==0x3001))finish();
    }
    finish();
    return out;
}

std::vector<SpeechVoice> InstalledVoices(){
    ComScope com;std::vector<SpeechVoice> out;std::vector<std::wstring> seen;
    auto scan=[&](const wchar_t* category,bool oneCore){
        ComPtr<ISpObjectTokenCategory> cat;
        if(FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory,nullptr,CLSCTX_ALL,IID_PPV_ARGS(&cat))))return;
        if(FAILED(cat->SetId(category,FALSE)))return;
        ComPtr<IEnumSpObjectTokens> tokens;
        if(FAILED(cat->EnumTokens(nullptr,nullptr,&tokens))||!tokens)return;
        ULONG count=0;tokens->GetCount(&count);
        for(ULONG i=0;i<count;++i){
            ComPtr<ISpObjectToken> token;if(FAILED(tokens->Item(i,&token))||!token)continue;
            SpeechVoice v;v.oneCore=oneCore;
            LPWSTR s=nullptr;if(SUCCEEDED(token->GetId(&s)))v.id=TakeString(s);
            s=nullptr;if(SUCCEEDED(token->GetStringValue(nullptr,&s)))v.name=TakeString(s);
            std::wstring shortName;
            ComPtr<ISpDataKey> attributes;
            if(SUCCEEDED(token->OpenKey(L"Attributes",&attributes))&&attributes){
                s=nullptr;if(SUCCEEDED(attributes->GetStringValue(L"Language",&s)))v.language=TakeString(s);
                s=nullptr;if(SUCCEEDED(attributes->GetStringValue(L"Name",&s)))shortName=ShortName(TakeString(s));
            }
            if(v.id.empty())continue;
            if(v.name.empty())v.name=shortName.empty()?v.id:shortName;
            if(!shortName.empty()){if(std::find(seen.begin(),seen.end(),shortName)!=seen.end())continue;seen.push_back(shortName);}
            v.chinese=ChineseLanguage(v.language);
            out.push_back(std::move(v));
        }
    };
    scan(SPCAT_VOICES,false);
    scan(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices",true);
    return out;
}

SpeechPlayer::SpeechPlayer():thread_([this]{Run();}){}
SpeechPlayer::~SpeechPlayer(){
    {std::lock_guard lock(mutex_);quit_=true;++session_;}
    wake_.notify_all();if(thread_.joinable())thread_.join();
}
uint64_t SpeechPlayer::Play(std::vector<std::wstring> sentences,int from){
    uint64_t session;
    {std::lock_guard lock(mutex_);
     queue_=std::move(sentences);session=++session_;index_=std::clamp(from,0,std::max(0,static_cast<int>(queue_.size())));
     jump_=-1;paused_=false;active_=index_<static_cast<int>(queue_.size());}
    wake_.notify_all();return session;
}
void SpeechPlayer::Stop(){{std::lock_guard lock(mutex_);++session_;queue_.clear();index_=-1;jump_=-1;paused_=false;active_=false;}wake_.notify_all();}
void SpeechPlayer::Pause(bool paused){{std::lock_guard lock(mutex_);if(!active_)return;paused_=paused;}wake_.notify_all();}
bool SpeechPlayer::Paused()const{std::lock_guard lock(mutex_);return paused_;}
bool SpeechPlayer::Active()const{std::lock_guard lock(mutex_);return active_;}
void SpeechPlayer::Skip(int delta){
    {std::lock_guard lock(mutex_);if(!active_||index_<0)return;
     jump_=std::clamp(index_+delta,0,static_cast<int>(queue_.size()));paused_=false;}
    wake_.notify_all();
}
void SpeechPlayer::Rate(int rate){std::lock_guard lock(mutex_);rate_=std::clamp(rate,-10,10);}
int SpeechPlayer::Rate()const{std::lock_guard lock(mutex_);return rate_;}
void SpeechPlayer::Voice(std::wstring id){std::lock_guard lock(mutex_);voice_=std::move(id);}
void SpeechPlayer::OutputFile(std::filesystem::path wav){{std::lock_guard lock(mutex_);wav_=std::move(wav);outputChanged_=true;}wake_.notify_all();}

void SpeechPlayer::Run(){
    ComScope com;
    ComPtr<ISpVoice> voice;
    const HRESULT created=CoCreateInstance(CLSID_SpVoice,nullptr,CLSCTX_ALL,IID_PPV_ARGS(&voice));
    ComPtr<ISpStream> stream;
    const auto voices=InstalledVoices();
    std::wstring currentVoice=L"\x1";int currentRate=INT_MIN;bool voicePaused=false;
    auto token=[](const std::wstring& id)->ComPtr<ISpObjectToken>{
        ComPtr<ISpObjectToken> t;
        if(FAILED(CoCreateInstance(CLSID_SpObjectToken,nullptr,CLSCTX_ALL,IID_PPV_ARGS(&t)))||FAILED(t->SetId(nullptr,id.c_str(),FALSE)))return {};
        return t;
    };
    std::unique_lock lock(mutex_);
    while(true){
        wake_.wait(lock,[&]{return quit_||outputChanged_||(active_&&!paused_&&index_>=0&&index_<static_cast<int>(queue_.size()));});
        if(quit_)break;
        if(outputChanged_){
            outputChanged_=false;const auto path=wav_;lock.unlock();
            if(voice){
                if(stream){voice->SetOutput(nullptr,TRUE);stream->Close();stream.Reset();}
                if(!path.empty()&&SUCCEEDED(CoCreateInstance(CLSID_SpStream,nullptr,CLSCTX_ALL,IID_PPV_ARGS(&stream)))){
                    WAVEFORMATEX format{WAVE_FORMAT_PCM,1,22050,44100,2,16,0};
                    if(FAILED(stream->BindToFile(path.c_str(),SPFM_CREATE_ALWAYS,&SPDFID_WaveFormatEx,&format,0))||FAILED(voice->SetOutput(stream.Get(),TRUE)))stream.Reset();
                }
            }
            lock.lock();continue;
        }
        const uint64_t session=session_;
        if(!voice){active_=false;lock.unlock();if(failed)failed(session,L"无法启动 Windows 语音（SAPI 错误 "+Hex(created)+L"）");lock.lock();continue;}
        const int index=index_;const std::wstring sentence=queue_[index];const std::wstring wanted=voice_;const int rate=rate_;
        lock.unlock();
        std::wstring id=wanted;
        if(id.empty()){
            const bool zh=ContainsChinese(sentence);
            for(const auto& v:voices)if(v.chinese==zh&&!v.oneCore){id=v.id;break;}
            if(id.empty())for(const auto& v:voices)if(v.chinese==zh){id=v.id;break;}
        }
        if(id!=currentVoice){
            auto t=id.empty()?ComPtr<ISpObjectToken>{}:token(id);
            if(!t||FAILED(voice->SetVoice(t.Get()))){voice->SetVoice(nullptr);id.clear();}
            currentVoice=id;
        }
        std::wstring name=L"系统默认语音";for(const auto& v:voices)if(v.id==currentVoice)name=v.name;
        if(rate!=currentRate){voice->SetRate(rate);currentRate=rate;}
        if(started)started(session,index,name);
        const HRESULT spoken=voice->Speak(sentence.c_str(),SPF_ASYNC|SPF_IS_NOT_XML|SPF_PURGEBEFORESPEAK,nullptr);
        bool interrupted=false;
        if(SUCCEEDED(spoken))for(;;){
            if(voice->WaitUntilDone(30)==S_OK)break;
            std::lock_guard guard(mutex_);
            if(quit_||session_!=session||jump_>=0){interrupted=true;break;}
            if(paused_!=voicePaused){if(paused_)voice->Pause();else voice->Resume();voicePaused=paused_;}
            if(rate_!=currentRate){voice->SetRate(rate_);currentRate=rate_;}
        }
        if(voicePaused){voice->Resume();voicePaused=false;}
        if(interrupted)voice->Speak(nullptr,SPF_PURGEBEFORESPEAK,nullptr);
        lock.lock();
        if(quit_)break;
        if(session_!=session)continue;   // Stop 或新一轮 Play
        if(FAILED(spoken)){active_=false;lock.unlock();if(failed)failed(session,L"语音朗读失败（"+Hex(spoken)+L"）");lock.lock();continue;}
        index_=jump_>=0?jump_:index_+1;jump_=-1;
        if(index_>=static_cast<int>(queue_.size())){active_=false;lock.unlock();if(finished)finished(session);lock.lock();}
    }
    lock.unlock();
    if(voice){voice->Speak(nullptr,SPF_PURGEBEFORESPEAK,nullptr);if(stream){voice->SetOutput(nullptr,TRUE);stream->Close();}}
    stream.Reset();voice.Reset();
}
}
