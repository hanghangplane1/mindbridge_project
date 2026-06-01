#include "PluginAPI.h"
#include "json.hpp"

#include <curl/curl.h>
#include <cstdlib>
#include <cstring>
#include <string>

using json = nlohmann::json;

namespace {

static PluginTool methods[] = {
    {"send_alert_email",
     "Send an alert email via SMTP. Falls back to simulation when SMTP env is missing.",
     "{\"type\":\"object\",\"properties\":{\"to\":{\"type\":\"string\"},\"subject\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"}},\"required\":[\"to\",\"subject\",\"body\"]}"},
};

struct UploadStatus {
    size_t bytes_read = 0;
    std::string payload;
};

size_t payload_source(char* ptr, size_t size, size_t nmemb, void* userp) {
    auto* upload = static_cast<UploadStatus*>(userp);
    const size_t buffer_size = size * nmemb;
    if (upload->bytes_read >= upload->payload.size()) {
        return 0;
    }
    const size_t left = upload->payload.size() - upload->bytes_read;
    const size_t copy = left < buffer_size ? left : buffer_size;
    memcpy(ptr, upload->payload.data() + upload->bytes_read, copy);
    upload->bytes_read += copy;
    return copy;
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

bool send_email(const std::string& to, const std::string& subject, const std::string& body, std::string* error) {
    const char* smtp_url = std::getenv("MINDCARE_SMTP_URL");
    const char* smtp_user = std::getenv("MINDCARE_SMTP_USER");
    const char* smtp_pass = std::getenv("MINDCARE_SMTP_PASS");
    const char* smtp_from = std::getenv("MINDCARE_SMTP_FROM");

    if (!smtp_url || !smtp_user || !smtp_pass || !smtp_from) {
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        *error = "failed to init curl";
        return false;
    }

    std::string payload = "To: " + to + "\r\n"
                          "From: " + std::string(smtp_from) + "\r\n"
                          "Subject: " + subject + "\r\n"
                          "\r\n" +
                          body + "\r\n";
    UploadStatus upload{0, payload};

    struct curl_slist* recipients = nullptr;
    recipients = curl_slist_append(recipients, to.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, smtp_url);
    curl_easy_setopt(curl, CURLOPT_USERNAME, smtp_user);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, smtp_pass);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, smtp_from);
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source);
    curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    const CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        *error = curl_easy_strerror(rc);
        return false;
    }
    return true;
}

}  // namespace

const char* GetNameImpl() { return "email-tools"; }
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
        if (name != "send_alert_email") {
            response = err_text("unknown tool");
        } else {
            const auto args = request["params"]["arguments"];
            const std::string to = args.value("to", "");
            const std::string subject = args.value("subject", "");
            const std::string body = args.value("body", "");
            std::string err;
            if (to.empty() || subject.empty() || body.empty()) {
                response = err_text("missing to/subject/body");
            } else if (send_email(to, subject, body, &err)) {
                response = ok_text("email sent");
            } else {
                response = ok_text("email simulated (smtp env missing or send failed: " + err + ")");
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
