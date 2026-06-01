#include "mindbridge/harness/trace_recorder.hpp"

namespace mindbridge {

TraceRecorder::TraceRecorder(std::string path) : path_(std::move(path)) {
    out_.open(path_, std::ios::app);
}

TraceRecorder::~TraceRecorder() {
    if (out_.is_open()) {
        out_.close();
    }
}

void TraceRecorder::append(const nlohmann::json& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (out_.is_open()) {
        out_ << event.dump() << '\n';
        out_.flush();
    }
}

}  // namespace mindbridge
