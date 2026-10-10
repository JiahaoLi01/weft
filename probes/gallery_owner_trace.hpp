#pragma once
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
struct GalleryProfileControl {std::atomic<bool> enabled{false};const long long run_id=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();};
// Each file belongs to one thread. No per-event file writes; a long run
// retains its latest bounded window, including the user's last interactions.
class GalleryOwnerTrace {
    struct Event {std::string name,args;double start{},duration{};};
    std::filesystem::path path_;std::shared_ptr<GalleryProfileControl> control_;
    std::deque<Event> events_;std::size_t dropped_{},emitted_{},inputs_{},publications_{};unsigned tid_;
public:
    explicit GalleryOwnerTrace(std::filesystem::path path,unsigned tid=1,std::shared_ptr<GalleryProfileControl> control={}):path_(std::move(path)),control_(std::move(control)),tid_(tid){}
    bool enabled()const{return !path_.empty() && (!control_ || control_->enabled.load(std::memory_order_relaxed));}
    void emit(const std::string& name,std::chrono::steady_clock::time_point start,double duration,const std::string& args="{}"){
        if(!enabled())return;if(events_.size()==65536){events_.pop_front();++dropped_;}++emitted_;inputs_+=name.starts_with("input scene=");publications_+=name=="owner publication";
        events_.push_back({name,args,std::chrono::duration<double,std::micro>(start.time_since_epoch()).count(),duration*1000});
    }
    struct Scope {
        GalleryOwnerTrace& trace;std::string name,args;bool active;std::chrono::steady_clock::time_point start;
        Scope(GalleryOwnerTrace& t,std::string n,std::string a,bool allowed=true):trace(t),name(std::move(n)),args(std::move(a)),active(allowed && t.enabled()),start(active?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{}){}
        ~Scope(){if(active)trace.emit(name,start,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(),args);}
    };
    ~GalleryOwnerTrace(){if(!emitted_)return;std::ofstream stream(path_,std::ios::binary);if(!stream)return;stream.imbue(std::locale::classic());stream<<std::setprecision(17)<<"{\"traceEvents\":[";bool comma{};for(const auto& e:events_){if(comma)stream<<',';comma=true;stream<<"{\"ph\":\"X\",\"pid\":1,\"tid\":"<<tid_<<",\"name\":"<<json_string(e.name)<<",\"ts\":"<<e.start<<",\"dur\":"<<e.duration<<",\"args\":"<<e.args<<'}';}stream<<"],\"run_id\":"<<(control_?control_->run_id:0)<<",\"clock\":\"steady_clock_us\",\"event_limit\":65536,\"retained_window\":\"latest\",\"emitted_events\":"<<emitted_<<",\"sampled_inputs\":"<<inputs_<<",\"sampled_publications\":"<<publications_<<",\"dropped_events\":"<<dropped_<<'}';}
};
