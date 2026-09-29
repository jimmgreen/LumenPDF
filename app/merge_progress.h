#pragma once
#include "core/progress.h"
#include <mutex>
#include <vector>
namespace lpdf {
enum class MergeRowState { Waiting, Converting, Prepared, Inspecting, Flattening, Merging, Merged, Failed, Cancelled, NotProcessed };
enum class MergeOutcome { Running, Complete, Failed, Cancelled };
struct MergeRowProgress {
    MergeRowState state{MergeRowState::Waiting};
    uint64_t done{},pages{};
    std::wstring backend;
};
struct MergeSnapshot {
    std::vector<MergeRowProgress> rows;
    OperationProgress current;
    int phase{1};
    uint64_t revision{};
    MergeOutcome outcome{MergeOutcome::Running};
    std::wstring message;
};
// Worker updates plain data only. The UI samples one consistent snapshot on its
// own timer, coalescing rapid per-page reports without queuing thousands of UI jobs.
class MergeProgressState {
public:
    explicit MergeProgressState(size_t files){data_.rows.resize(files);data_.current.fileCount=files;data_.current.fileIndex=files?0:NoProgressFile;}
    void Conversion(size_t index,const OperationProgress& event){
        std::lock_guard lock(mutex_);if(index>=data_.rows.size()||data_.outcome!=MergeOutcome::Running)return;
        data_.phase=1;data_.current=event;data_.current.fileIndex=index;data_.current.fileCount=data_.rows.size();
        auto& row=data_.rows[index];row.state=event.stage==ProgressStage::ConversionReady?MergeRowState::Prepared:MergeRowState::Converting;
        if(!event.backend.empty())row.backend=event.backend;
        ++data_.revision;
    }
    void Converted(size_t index,std::wstring backend){
        std::lock_guard lock(mutex_);if(index>=data_.rows.size())return;
        data_.rows[index].state=MergeRowState::Prepared;data_.rows[index].backend=std::move(backend);++data_.revision;
    }
    void Merge(const OperationProgress& event){
        std::lock_guard lock(mutex_);if(data_.outcome!=MergeOutcome::Running)return;
        data_.current=event;
        data_.phase=event.stage==ProgressStage::Inspecting?1:
            (event.stage==ProgressStage::Flattening||event.stage==ProgressStage::MergingPages||event.stage==ProgressStage::Bookmarks)?2:3;
        if(event.fileIndex<data_.rows.size()){
            auto& row=data_.rows[event.fileIndex];
            if(event.fileTotal)row.pages=event.fileTotal;
            if(event.stage==ProgressStage::Inspecting)row.state=MergeRowState::Inspecting;
            if(event.stage==ProgressStage::Flattening)row.state=MergeRowState::Flattening;
            if(event.stage==ProgressStage::MergingPages){
                row.done=event.fileCompleted;
                row.state=event.fileTotal&&row.done==event.fileTotal?MergeRowState::Merged:MergeRowState::Merging;
            }
        }
        ++data_.revision;
    }
    void Finish(MergeOutcome outcome,std::wstring message={}){
        std::lock_guard lock(mutex_);data_.outcome=outcome;data_.message=std::move(message);
        if(outcome==MergeOutcome::Complete){
            for(auto& row:data_.rows){row.state=MergeRowState::Merged;row.done=row.pages;}
        }else{
            const size_t active=data_.current.fileIndex;
            if(active<data_.rows.size())data_.rows[active].state=outcome==MergeOutcome::Cancelled?MergeRowState::Cancelled:MergeRowState::Failed;
            for(auto& row:data_.rows)if(row.state==MergeRowState::Waiting)row.state=MergeRowState::NotProcessed;
        }
        ++data_.revision;
    }
    MergeSnapshot Read()const{std::lock_guard lock(mutex_);return data_;}
private:
    mutable std::mutex mutex_;
    MergeSnapshot data_;
};
}
