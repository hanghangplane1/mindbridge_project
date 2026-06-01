#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

using json = nlohmann::json;

namespace {

size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    out->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

size_t sse_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* pending = static_cast<std::string*>(userp);
    pending->append(static_cast<char*>(contents), size * nmemb);
    size_t pos = 0;
    while ((pos = pending->find("\n\n")) != std::string::npos) {
        std::string event = pending->substr(0, pos);
        pending->erase(0, pos + 2);
        if (event.rfind("data: ", 0) == 0) {
            std::cout << event.substr(6) << std::endl;
        }
    }
    return size * nmemb;
}

std::string post_json(const std::string& url, const std::string& body, long timeout_sec = 60L) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        return std::string("{\"error\":\"") + curl_easy_strerror(rc) + "\"}";
    }
    return response;
}

std::string get_text(const std::string& url, long timeout_sec = 10L) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        return std::string("{\"error\":\"") + curl_easy_strerror(rc) + "\"}";
    }
    return response;
}

void stream_json(const std::string& url, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) return;
    std::string pending;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &pending);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string server = "http://localhost:5000";
    if (argc > 1) {
        server = argv[1];
    }
    std::string context_id = "default";
    int request_id = 0;

    std::cout << "MindCare Client connected to " << server << std::endl;
    std::cout << "Commands: /chat <text>, /stream <text>, /card, /history, /context <id>, /quit" << std::endl;

    std::string line;
    while (true) {
        std::cout << "[" << context_id << "]> ";
        if (!std::getline(std::cin, line)) break;
        if (line == "/quit" || line == "/exit") break;
        if (line.rfind("/context ", 0) == 0) {
            context_id = line.substr(9);
            continue;
        }
        if (line == "/card") {
            std::cout << get_text(server + "/.well-known/agent-card.json") << std::endl;
            continue;
        }
        if (line == "/history") {
            json req = {
                {"jsonrpc", "2.0"},
                {"id", std::to_string(++request_id)},
                {"method", "tasks/get"},
                {"params", {{"id", context_id}}},
            };
            std::cout << post_json(server, req.dump()) << std::endl;
            continue;
        }
        if (line.rfind("/chat ", 0) == 0) {
            std::string text = line.substr(6);
            json req = {
                {"jsonrpc", "2.0"},
                {"id", std::to_string(++request_id)},
                {"method", "message/send"},
                {"params",
                 {
                     {"message",
                      {
                          {"role", "user"},
                          {"contextId", context_id},
                          {"parts", {{{"kind", "text"}, {"text", text}}}},
                      }},
                     {"historyLength", 5},
                 }},
            };
            std::cout << post_json(server, req.dump()) << std::endl;
            continue;
        }
        if (line.rfind("/stream ", 0) == 0) {
            std::string text = line.substr(8);
            json req = {
                {"jsonrpc", "2.0"},
                {"id", std::to_string(++request_id)},
                {"method", "message/stream"},
                {"params",
                 {
                     {"message",
                      {
                          {"role", "user"},
                          {"contextId", context_id},
                          {"parts", {{{"kind", "text"}, {"text", text}}}},
                      }},
                     {"historyLength", 5},
                 }},
            };
            stream_json(server, req.dump());
            continue;
        }
        std::cout << "Unknown command" << std::endl;
    }

    return 0;
}
