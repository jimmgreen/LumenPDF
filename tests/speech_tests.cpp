// 朗读：core 按句切分（带逐行框）、纯文字断句、已安装语音、SpeechPlayer 的顺序 / 自动选语音 / 停止 / 跳句 / 暂停。
// 语音一律输出到 WAV 文件，测试不会发出声音。
#include "app/speech.h"
#include "core/document.h"
#include "core/platform.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <fstream>
#include <map>
#include <sstream>
using namespace lpdf;
namespace {
int assertions=0;
void Require(bool okay,const std::string& why){++assertions;if(!okay)throw std::runtime_error(why);}
struct Recorder {
    std::mutex m;std::condition_variable cv;std::vector<std::pair<int,std::wstring>> started;int finished{};std::vector<std::wstring> failures;
    void Attach(SpeechPlayer& p){
        p.started=[this](uint64_t,int i,std::wstring v){std::lock_guard l(m);started.push_back({i,v});cv.notify_all();};
        p.finished=[this](uint64_t){std::lock_guard l(m);++finished;cv.notify_all();};
        p.failed=[this](uint64_t,std::wstring e){std::lock_guard l(m);failures.push_back(e);cv.notify_all();};
    }
    template<class F> bool Wait(F done,int ms=30000){std::unique_lock l(m);return cv.wait_for(l,std::chrono::milliseconds(ms),done);}
    void Reset(){std::lock_guard l(m);started.clear();finished=0;failures.clear();}
};
uintmax_t Size(const fs::path& p){std::error_code e;const auto n=fs::file_size(p,e);return e?0:n;}

// --ui：启动真实 LumenPDF（语音写入 WAV，不出声）：从第 1 页朗读到文末（跳过空白页、跟读高亮、自动翻页）、暂停 / 继续、跳句、停止。
int RunUi(const fs::path& exe,const fs::path& out){
    std::error_code error;fs::remove_all(out,error);fs::create_directories(out);
    std::wstring text;for(int i=1;i<=110;++i)text+=L"Reading line "+std::to_wstring(i)+L" of the LumenPDF speech test.\n";
    text+=L"最后一句是中文。\n";
    const auto raw=out/L"raw.pdf",fixture=out/L"speech.pdf";Document::TextToPdf(text,raw);
    int blank=-1,pages=0;
    {Document d;d.Open(raw);d.InsertBlank(0);d.Save(fixture);}
    {Document d;d.Open(fixture);pages=static_cast<int>(d.Info().pages.size());for(int p=0;p<pages;++p)if(d.ReadingSentences(p).empty())blank=p;}
    Require(pages>=3&&blank==1,"fixture: text, blank page, more text");
    const auto wav=out/L"speech.wav";
    SetEnvironmentVariableW(L"LPDF_SPEECH_SMOKE",out.c_str());SetEnvironmentVariableW(L"LPDF_SPEECH_WAV",wav.c_str());SetEnvironmentVariableW(L"LPDF_SPEECH_HOLD",nullptr);
    SetEnvironmentVariableW(L"LPDF_SMOKE_TIMEOUT",L"90");SetEnvironmentVariableW(L"LPDF_NO_DEFAULT_PROMPT",L"1");
    std::wstring command=L"\""+exe.wstring()+L"\" --smoke \""+fixture.wstring()+L"\"";
    STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
    Require(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,out.c_str(),&si,&pi)!=0,"cannot start LumenPDF");
    const DWORD wait=WaitForSingleObject(pi.hProcess,100000);if(wait!=WAIT_OBJECT_0)TerminateProcess(pi.hProcess,3);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    Require(fs::exists(out/L"report.txt"),"smoke wrote no report");
    std::string rawReport;{std::ifstream f(out/L"report.txt",std::ios::binary);rawReport.assign(std::istreambuf_iterator<char>(f),{});}
    if(rawReport.size()>=3&&static_cast<unsigned char>(rawReport[0])==0xEF)rawReport.erase(0,3);
    std::cout<<rawReport;
    std::map<std::wstring,std::wstring> r;std::wstringstream lines(Wide(rawReport));std::wstring line;
    while(std::getline(lines,line)){if(!line.empty()&&line.back()==L'\r')line.pop_back();const auto eq=line.find(L'=');if(eq!=std::wstring::npos)r[line.substr(0,eq)]=line.substr(eq+1);}
    Require(wait==WAIT_OBJECT_0,"LumenPDF speech smoke timed out");
    auto is=[&](const wchar_t* key,const std::wstring& value){Require(r[key]==value,Utf8(std::wstring(key)+L" = "+r[key]+L", expected "+value));};
    is(L"pages",std::to_wstring(pages));is(L"first.page",L"0");is(L"highlight",L"0");
    for(const wchar_t* key:{L"reading.status",L"paused",L"paused.status",L"resumed",L"finished.status",L"cleared",L"followed",L"stopped",L"stopped.status",L"done"})is(key,L"1");
    std::wstring expected=L"0";for(int p=2;p<pages;++p)expected+=L","+std::to_wstring(p);
    is(L"visited",expected);
    Require(!r[L"skip.index"].empty()&&std::stoi(r[L"skip.index"])>=2,"skip moves ahead");
    is(L"dirty",L"0");
    Require(Size(wav)>50000,"the whole document was spoken into the WAV file");
    std::cout<<"PASS read aloud UI: "<<assertions<<" assertions; read to end skipping the blank page, highlight + follow, pause/resume, skip, stop; WAV only, no sound.\n";
    return 0;
}
}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc==4&&std::wstring(argv[1])==L"--ui")return RunUi(fs::absolute(argv[2]),fs::absolute(argv[3]));
        if(argc!=2)throw std::runtime_error("usage: speech_tests <output>");
        const fs::path out=fs::absolute(argv[1]);std::error_code error;fs::remove_all(out,error);fs::create_directories(out);
        // —— core：按句切分 ——
        const auto fixture=out/L"read.pdf";
        Document::TextToPdf(L"LumenPDF read aloud test. Second sentence here!\n第三句是中文。第四句也是中文？\n\nA hyphen-\nated word ends. Pi is 3.14 today.",fixture);
        Document doc;doc.Open(fixture);
        const auto sentences=doc.ReadingSentences(0);
        for(const auto& s:sentences)std::cout<<"  ["<<Utf8(s.text)<<"] boxes="<<s.boxes.size()<<"\n";
        Require(sentences.size()>=6,"page one yields at least six sentences");
        auto find=[&](const std::wstring& t){for(size_t i=0;i<sentences.size();++i)if(sentences[i].text==t)return static_cast<int>(i);return -1;};
        Require(find(L"LumenPDF read aloud test.")==0,"first English sentence");
        Require(find(L"Second sentence here!")==1,"second sentence split at !");
        Require(find(L"第三句是中文。")>=0&&find(L"第四句也是中文？")>=0,"Chinese sentences split at 。 and ？");
        Require(find(L"A hyphenated word ends.")>=0,"line-end hyphen joined");
        Require(find(L"Pi is 3.14 today.")>=0,"decimal point does not split");
        const auto info=doc.Info().pages[0];
        for(const auto& s:sentences){
            Require(!s.boxes.empty(),"every sentence has boxes");
            for(const auto& b:s.boxes)Require(b.w>0&&b.h>0&&b.x>=info.originX-1&&b.y>=info.originY-1&&b.x+b.w<=info.originX+info.width+1&&b.y+b.h<=info.originY+info.height+1,"boxes lie on the page");
        }
        Require(sentences[find(L"第三句是中文。")].boxes[0].y>sentences[0].boxes[0].y,"reading order top to bottom");
        Require(sentences[find(L"A hyphenated word ends.")].boxes.size()==2,"sentence across two lines has two line boxes");
        Require(find(L"\uFFFD")<0,"no replacement characters read out");
        doc.InsertBlank(0);
        {int blank=-1;for(int p=0;p<static_cast<int>(doc.Info().pages.size());++p)if(doc.ReadingSentences(p).empty())blank=p;
         Require(doc.Info().pages.size()==2&&blank>=0,"a blank page yields no sentences");
         Require(doc.ReadingSentences(1-blank).size()==sentences.size(),"the text page still reads the same after inserting a page");}
        // —— 纯文字断句 ——
        const auto split=SplitSentences(L"Hello world. 你好，世界！\r\nLine one\ncontinues here? 「引号。」结束\n\nNew para");
        for(const auto& s:split)std::cout<<"  split["<<Utf8(s)<<"]\n";
        Require(split.size()>=5,"text split into sentences");
        Require(split[0]==L"Hello world."&&split[1]==L"你好，世界！"&&split[2]==L"Line one continues here?","terminators and line joins");
        Require(split[3]==L"「引号。」","closing bracket stays with its sentence");
        Require(split.back()==L"New para","blank line ends a paragraph");
        Require(ContainsChinese(L"abc中")&&!ContainsChinese(L"abc，"),"Chinese detection ignores punctuation");
        // —— 已安装语音 ——
        const auto voices=InstalledVoices();
        bool chinese=false,other=false;
        for(const auto& v:voices){std::cout<<"  voice: "<<Utf8(v.name)<<" | lang="<<Utf8(v.language)<<(v.chinese?" | zh":"")<<(v.oneCore?" | OneCore":"")<<"\n";chinese|=v.chinese;other|=!v.chinese;}
        Require(!voices.empty(),"at least one installed voice");
        // —— 播放：顺序与自动选语音 ——
        Recorder rec;
        {
            SpeechPlayer player;rec.Attach(player);player.Rate(10);player.OutputFile(out/L"auto.wav");
            player.Play({L"Hello world.",L"你好，世界。",L"Third sentence."});
            Require(rec.Wait([&]{return rec.finished==1||!rec.failures.empty();}),"playback finishes");
            Require(rec.failures.empty(),"no playback failure");
            Require(rec.started.size()==3&&rec.started[0].first==0&&rec.started[1].first==1&&rec.started[2].first==2,"sentences spoken in order");
            if(chinese&&other){
                bool zh=false,en=false;for(const auto& v:voices){if(v.name==rec.started[1].second)zh=v.chinese;if(v.name==rec.started[0].second)en=!v.chinese;}
                Require(zh&&en,"automatic voice: Chinese sentence uses a Chinese voice, English uses another");
            }
            Require(!player.Active(),"idle after finishing");
        }
        Require(Size(out/L"auto.wav")>8000,"speech rendered into the WAV file (no sound played)");
        // —— 停止 / 跳句 / 暂停 ——
        {
            rec.Reset();SpeechPlayer player;rec.Attach(player);player.Rate(0);player.OutputFile(out/L"control.wav");
            std::vector<std::wstring> many;for(int i=0;i<12;++i)many.push_back(L"This is sentence number "+std::to_wstring(i+1)+L" of the control test.");
            player.Play(many);
            Require(rec.Wait([&]{return !rec.started.empty();}),"playback starts");
            player.Skip(3);
            Require(rec.Wait([&]{return rec.started.size()>=2;}),"skip continues");
            Require(rec.started[1].first==3,"skip jumps three sentences ahead");
            player.Pause(true);Require(player.Paused(),"paused");
            const size_t before=rec.started.size();std::this_thread::sleep_for(std::chrono::milliseconds(700));
            {std::lock_guard l(rec.m);Require(rec.started.size()<=before+1&&rec.finished==0,"paused playback does not run on");}
            player.Pause(false);Require(!player.Paused(),"resumed");
            Require(rec.Wait([&]{return rec.started.size()>=before+2;}),"resumed playback continues");
            player.Stop();Require(!player.Active(),"stopped");
            const size_t at=rec.started.size();std::this_thread::sleep_for(std::chrono::milliseconds(500));
            {std::lock_guard l(rec.m);Require(rec.started.size()==at&&rec.finished==0,"nothing more after stop, no finished callback");}
            // Skip 越过末尾 = 本轮结束
            rec.Reset();player.Play({L"One.",L"Two."});
            Require(rec.Wait([&]{return !rec.started.empty();}),"second round starts");
            player.Skip(5);
            Require(rec.Wait([&]{return rec.finished==1;}),"skipping past the end finishes the round");
        }
        // —— 每个语音都能输出 ——
        int working=0;
        for(size_t i=0;i<voices.size();++i){
            rec.Reset();SpeechPlayer player;rec.Attach(player);player.Rate(10);
            const auto wav=out/(L"voice"+std::to_wstring(i)+L".wav");player.OutputFile(wav);player.Voice(voices[i].id);
            player.Play({voices[i].chinese?L"测试语音。":L"Voice test."});
            const bool done=rec.Wait([&]{return rec.finished==1||!rec.failures.empty();},15000);
            const bool ok=done&&rec.failures.empty()&&Size(wav)>2000&&!rec.started.empty()&&rec.started[0].second==voices[i].name;
            std::cout<<"  voice "<<Utf8(voices[i].name)<<": "<<(ok?"OK":"FALLBACK/FAIL")<<" ("<<Size(wav)<<" bytes)\n";
            if(ok)++working;
        }
        Require(working>=1,"at least one voice produces speech");
        std::cout<<"PASS speech: "<<assertions<<" assertions; "<<sentences.size()<<" sentences, "<<voices.size()<<" voices ("<<working<<" verified), order/auto-voice/skip/pause/stop.\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL speech after "<<assertions<<" assertions: "<<e.what()<<"\n";return 1;}
}
