#pragma once
// 朗读：Windows 自带语音（SAPI 5，不联网）。独立线程持有语音对象，一次说一句；回调都在语音线程上发生，
// 由调用方转回界面线程。session 用来丢弃已被 Stop / 新一轮 Play 取代的回调。
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace lpdf {
struct SpeechVoice { std::wstring id, name, language; bool chinese{}, oneCore{}; };
// 已安装的语音：先桌面 SAPI 语音，再补充同名之外的 OneCore 语音（Windows 10/11 自带的中文语音多在这里）。
std::vector<SpeechVoice> InstalledVoices();
bool ContainsChinese(std::wstring_view text);
// 纯文字断句（朗读选中文字用）：。！？；… 与 . ! ? ; 后跟空白处断开，换行合并，过长句在逗号 / 空格处再切。
std::vector<std::wstring> SplitSentences(std::wstring_view text);

class SpeechPlayer {
public:
    SpeechPlayer();
    ~SpeechPlayer();
    SpeechPlayer(const SpeechPlayer&)=delete;SpeechPlayer& operator=(const SpeechPlayer&)=delete;
    std::function<void(uint64_t session,int index,std::wstring voice)> started;
    std::function<void(uint64_t session)> finished;
    std::function<void(uint64_t session,std::wstring message)> failed;
    uint64_t Play(std::vector<std::wstring> sentences,int from=0);   // 替换队列并立即开始，返回新 session
    void Stop();
    void Pause(bool paused);
    bool Paused()const;
    bool Active()const;
    void Skip(int delta);                 // 上一句 / 下一句（越过末尾即结束本轮）
    void Rate(int rate);                  // -10..10，当前句之后生效
    int Rate()const;
    void Voice(std::wstring id);          // 空 = 自动：含中文的句子用中文语音，其它用非中文语音
    void OutputFile(std::filesystem::path wav);   // 测试用：写入 WAV 文件而不出声（在 Play 前设置）
private:
    void Run();
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<std::wstring> queue_;
    int index_{-1},jump_{-1},rate_{};
    uint64_t session_{};
    bool quit_{},paused_{},active_{},outputChanged_{};
    std::wstring voice_;
    std::filesystem::path wav_;
    std::thread thread_;
};
}
