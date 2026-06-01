#include "PluginAPI.h"
#include "json.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

using json = nlohmann::json;

namespace {

static PluginTool methods[] = {
    {"write_record",
     "Append an evaluation record to CSV file.",
     "{\"type\":\"object\",\"properties\":{\"userId\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"},\"emotionLabel\":{\"type\":\"string\"},\"riskLevel\":{\"type\":\"string\"},\"confidence\":{\"type\":\"number\"},\"timestamp\":{\"type\":\"integer\"}},\"required\":[\"userId\",\"content\",\"emotionLabel\",\"riskLevel\"]}"},
};

std::string escape_csv(const std::string& v) {
    std::string out = "\"";
    for (char c : v) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

json ok_text(const std::string& text) {
    return json{
        {"content", json::array({{{"type", "text"}, {"text", text}}})},
        {"isError", false},
    };
}

json err_text(const std::string& text) {
    return json{
        {"content", json::array({{{"type", "text"}, {"text", text}}})},
        {"isError", true},
    };
}

}  // namespace

const char* GetNameImpl() { return "excel-tools"; }
const char* GetVersionImpl() { return "1.0.0"; }
PluginType GetTypeImpl() { return PLUGIN_TYPE_TOOLS; }
int InitializeImpl() { return 1; }
void ShutdownImpl() {}
int GetToolCountImpl() { return sizeof(methods) / sizeof(methods[0]); }
const PluginTool* GetToolImpl(int index) {
    if (index < 0 || index >= GetToolCountImpl()) return nullptr;
    return &methods[index];
}

char* HandleRequestImpl(const char* req) {
    json response;
    try {
        auto request = json::parse(req);
        std::string name = request["params"]["name"].get<std::string>();
        if (name != "write_record") {
            response = err_text("unknown tool");
        } else {
            auto args = request["params"]["arguments"];
            const std::string user_id = args.value("userId", "");
            const std::string content = args.value("content", "");
            const std::string emotion = args.value("emotionLabel", "");
            const std::string risk = args.value("riskLevel", "");
            const double confidence = args.value("confidence", 0.0);
            const long long ts = args.value("timestamp", 0LL);
            const char* out_file = std::getenv("MINDCARE_RECORD_FILE");
            std::string file = out_file ? out_file : "/tmp/mindcare_records.csv";

            std::ifstream fin(file);
            const bool has_header = fin.good() && fin.peek() != std::ifstream::traits_type::eof();
            fin.close();

            std::ofstream fout(file, std::ios::app);
            if (!fout.is_open()) {
                response = err_text("failed to open record file: " + file);
            } else {
                if (!has_header) {
                    fout << "userId,content,emotionLabel,riskLevel,confidence,timestamp\n";
                }
                fout << escape_csv(user_id) << ","
                     << escape_csv(content) << ","
                     << escape_csv(emotion) << ","
                     << escape_csv(risk) << ","
                     << confidence << ","
                     << ts << "\n";
                response = ok_text("record appended: " + file);
            }
        }
    } catch (const std::exception& e) {
        response = err_text(std::string("error: ") + e.what());
    }

    std::string result = response.dump();
    char* out = new char[result.size() + 1];
    std::strcpy(out, result.c_str());
    return out;
}

static PluginAPI plugin = {
    GetNameImpl, GetVersionImpl, GetTypeImpl, InitializeImpl, HandleRequestImpl, ShutdownImpl,
    GetToolCountImpl, GetToolImpl, nullptr, nullptr, nullptr, nullptr};

extern "C" PLUGIN_API PluginAPI* CreatePlugin() { return &plugin; }
extern "C" PLUGIN_API void DestroyPlugin(PluginAPI*) {}
