#pragma once
#include "core/conversion.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
namespace lpdf {
struct Engine { Document document; Converter converter; bool open{}; };
class Worker {
public:
    Worker():thread_([this]{Run();}){}
    ~Worker(){Stop();thread_.join();}
    void Stop(){
        {std::lock_guard lock(mutex_);stopping_=true;important_.clear();render_.clear();}
        wake_.notify_one();
    }
    bool Finished()const{return finished_.load();}
    void Submit(std::function<void(Engine&)> job,bool priority=true){
        {std::lock_guard lock(mutex_);if(stopping_)return;(priority?important_:render_).push_back(std::move(job));}
        wake_.notify_one();
    }
private:
    void Run(){
        { Engine engine;
        for(;;){
            std::function<void(Engine&)> job;
            {std::unique_lock lock(mutex_);wake_.wait(lock,[&]{return stopping_||!important_.empty()||!render_.empty();});
                if(stopping_)break;
                auto& queue=important_.empty()?render_:important_;job=std::move(queue.front());queue.pop_front();}
            try{job(engine);}catch(...){/* Each job reports its own failures. Keep the worker alive. */}
        }
        } // Engine and MuPDF context die on their owning thread.
        finished_.store(true);
    }
    std::atomic_bool finished_{};
    std::mutex mutex_;std::condition_variable wake_;
    std::deque<std::function<void(Engine&)>> important_,render_;
    bool stopping_{};std::thread thread_;
};
}
