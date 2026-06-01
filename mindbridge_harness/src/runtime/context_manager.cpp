#include "mindbridge/runtime/context_manager.hpp"
#include "mindbridge/runtime/utf8.hpp"

#include <sstream>

namespace mindbridge {

namespace {

std::string clip(const std::string& text, size_t limit) {
    return truncate_utf8(text, limit);
}

std::string join_lines(const std::vector<std::string>& values, size_t each_limit) {
    std::ostringstream out;
    for (const auto& value : values) {
        out << "- " << clip(value, each_limit) << "\n";
    }
    return out.str();
}

}  // namespace

ContextBuildOutput ContextManager::build(const ContextBuildInput& input) {
    const size_t prefix_budget = 3200;
    size_t memory_budget = 1600;
    size_t relevant_budget = 1200;
    size_t rag_budget = 2000;
    size_t history_budget = 3000;

    std::string prefix = clip(input.prefix.text, prefix_budget);
    std::string memory = clip(input.memory_json.dump(), memory_budget);
    std::string relevant = clip(join_lines(input.relevant_memory, 400), relevant_budget);
    std::string rag = clip(join_lines(input.rag_contexts, 600), rag_budget);
    std::string history = clip(input.history, history_budget);
    std::string request = input.current_request;

    std::ostringstream prompt;
    prompt << "[Prefix]\n" << prefix << "\n";
    prompt << "[Memory]\n" << memory << "\n";
    if (!relevant.empty()) {
        prompt << "[RelevantMemory]\n" << relevant << "\n";
    }
    if (!rag.empty()) {
        prompt << "[RAGContexts]\n" << rag << "\n";
    }
    prompt << "[History]\n" << history << "\n";
    prompt << "[CurrentRequest]\n" << request << "\n";

    std::string built = prompt.str();
    // Budget reduction order: relevant -> history -> memory -> prefix
    if (built.size() > input.max_chars) {
        relevant_budget = std::min(relevant_budget, static_cast<size_t>(400));
        relevant = clip(relevant, relevant_budget);
    }
    prompt.str("");
    prompt.clear();
    prompt << "[Prefix]\n" << prefix << "\n";
    prompt << "[Memory]\n" << memory << "\n";
    if (!relevant.empty()) {
        prompt << "[RelevantMemory]\n" << relevant << "\n";
    }
    if (!rag.empty()) {
        prompt << "[RAGContexts]\n" << rag << "\n";
    }
    prompt << "[History]\n" << history << "\n";
    prompt << "[CurrentRequest]\n" << request << "\n";
    built = prompt.str();
    if (built.size() > input.max_chars) {
        history_budget = std::min(history_budget, static_cast<size_t>(1200));
        history = clip(history, history_budget);
    }
    prompt.str("");
    prompt.clear();
    prompt << "[Prefix]\n" << prefix << "\n";
    prompt << "[Memory]\n" << memory << "\n";
    if (!relevant.empty()) {
        prompt << "[RelevantMemory]\n" << relevant << "\n";
    }
    if (!rag.empty()) {
        prompt << "[RAGContexts]\n" << rag << "\n";
    }
    prompt << "[History]\n" << history << "\n";
    prompt << "[CurrentRequest]\n" << request << "\n";
    built = prompt.str();
    if (built.size() > input.max_chars) {
        memory_budget = std::min(memory_budget, static_cast<size_t>(800));
        memory = clip(memory, memory_budget);
    }
    prompt.str("");
    prompt.clear();
    prompt << "[Prefix]\n" << prefix << "\n";
    prompt << "[Memory]\n" << memory << "\n";
    if (!relevant.empty()) {
        prompt << "[RelevantMemory]\n" << relevant << "\n";
    }
    if (!rag.empty()) {
        prompt << "[RAGContexts]\n" << rag << "\n";
    }
    prompt << "[History]\n" << history << "\n";
    prompt << "[CurrentRequest]\n" << request << "\n";
    built = prompt.str();
    if (built.size() > input.max_chars) {
        prefix = clip(prefix, 1600);
    }

    prompt.str("");
    prompt.clear();
    prompt << "[Prefix]\n" << prefix << "\n";
    prompt << "[Memory]\n" << memory << "\n";
    if (!relevant.empty()) {
        prompt << "[RelevantMemory]\n" << relevant << "\n";
    }
    if (!rag.empty()) {
        prompt << "[RAGContexts]\n" << rag << "\n";
    }
    prompt << "[History]\n" << history << "\n";
    prompt << "[CurrentRequest]\n" << request << "\n";
    built = prompt.str();

    nlohmann::json metadata = prompt_prefix_metadata(input.prefix);
    metadata["memory_chars"] = memory.size();
    metadata["relevant_memory_chars"] = relevant.size();
    metadata["rag_chars"] = rag.size();
    metadata["history_chars"] = history.size();
    metadata["current_request_chars"] = request.size();
    metadata["prompt_chars"] = built.size();
    metadata["budget_max_chars"] = input.max_chars;

    return {built, metadata};
}

}  // namespace mindbridge
