// MindBridge Gateway 压力测试工具
// 参考 demonet.txt 中 ClientDemo 的完整业务流程：
// 登录 → WebSocket 长连接 → 心跳 → 订阅 → 推送接收 → 登出
// 使用 Boost.Beast WebSocketClient 实现真实长连接

#include "mindbridge/net/websocket_client.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ============================================================
// 工具函数
// ============================================================

static size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    out->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

static long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ============================================================
// 压测配置（参考 demonet.txt 的 CConfigRead）
// ============================================================

struct StressConfig {
    std::string gateway_url = "http://127.0.0.1:8090";
    std::string ws_url;                       // WebSocket URL（ws://host:port/path）
    int client_count = 10;                    // 并发客户端数
    int requests_per_client = 100;            // 每客户端消息数
    int print_by_num = 10;                    // 每 N 条打印一次统计
    int request_interval_ms = 0;              // 消息间隔（ms），0=不限速
    int ping_interval_ms = 5000;              // WebSocket ping 间隔（ms）
    int timeout_sec = 30;                     // 请求超时（秒）
    std::string test_type = "health";         // health / chat / sse / login / ws
    std::string chat_message = "你好";        // 消息内容
    // 登录相关
    std::string access_key_id;
    std::string secret_key;
    bool auth_required = false;
};

// ============================================================
// HTTP 请求封装
// ============================================================

static std::string http_post_json(const std::string& url, const std::string& body,
                                   int timeout_sec, const std::string& token = "") {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!token.empty()) {
        std::string auth = "Authorization: Bearer " + token;
        headers = curl_slist_append(headers, auth.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec));

    curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

static std::string http_get(const std::string& url, int timeout_sec) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec));

    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return response;
}

// ============================================================
// WebSocket 压测客户端（参考 demonet.txt 的 CClient）
// ============================================================

class WsStressClient {
public:
    WsStressClient(int id, const StressConfig& config)
        : id_(id), config_(config) {}

    struct Result {
        int id;
        int total_messages;
        int success_count;
        int fail_count;
        double avg_latency_ms;
        double min_latency_ms;
        double max_latency_ms;
        size_t total_bytes;
        long long total_time_ms;
        // 登录
        bool login_ok;
        double login_latency_ms;
        // WebSocket
        bool ws_connected;
        int ws_messages_received;
        int ws_errors;
        // 心跳（WebSocket ping）
        int ping_count;
        int ping_fail;
    };

    Result run() {
        Result result{};
        result.id = id_;
        result.min_latency_ms = 1e18;
        result.max_latency_ms = 0;
        result.login_ok = false;
        result.ws_connected = false;
        result.ws_messages_received = 0;
        result.ws_errors = 0;
        result.ping_count = 0;
        result.ping_fail = 0;

        auto start_time = now_ms();

        // 1. 登录（参考 demonet.txt 的 CClient::Login）
        std::string token;
        if (config_.test_type == "login" || config_.test_type == "ws" ||
            config_.auth_required) {
            auto t0 = now_ms();
            std::string login_body = R"({"access_key_id":")" + config_.access_key_id +
                                     R"(","secret_key":")" + config_.secret_key + R"("})";
            std::string login_resp = http_post_json(
                config_.gateway_url + "/api/auth/login", login_body, config_.timeout_sec);
            auto t1 = now_ms();

            result.login_latency_ms = static_cast<double>(t1 - t0);

            // 解析 token
            try {
                auto j = nlohmann::json::parse(login_resp);
                if (j.value("ok", false)) {
                    token = j.value("token", "");
                    result.login_ok = true;
                    printf("  [client-%d] Login OK, latency=%.1fms\n", id_, result.login_latency_ms);
                } else {
                    printf("  [client-%d] Login failed: %s\n", id_, login_resp.c_str());
                }
            } catch (...) {
                printf("  [client-%d] Login parse error\n", id_);
            }
        }

        // 2. WebSocket 模式（参考 demonet.txt 的完整流程）
        if (config_.test_type == "ws" && result.login_ok) {
            run_real_ws(token, result, start_time);
        } else if (config_.test_type == "ws") {
            // 没有认证也能连 WebSocket
            run_real_ws(token, result, start_time);
        } else {
            // HTTP 模式
            run_http_mode(token, result, start_time);
        }

        result.total_time_ms = now_ms() - start_time;
        compute_latency_stats(result);
        return result;
    }

private:
    // 真实 WebSocket 模式（参考 demonet.txt 的 CreateWebSocket + KeepAlive + RecvWebsocketMsg）
    void run_real_ws(const std::string& token, Result& result, long long start_time) {
        // 构建 WebSocket URL
        std::string ws_url = config_.ws_url;
        if (ws_url.empty()) {
            // 从 gateway_url 转换：http://host:port -> ws://host:port/path
            std::string base = config_.gateway_url;
            if (base.substr(0, 7) == "http://") {
                ws_url = "ws://" + base.substr(7);
            } else if (base.substr(0, 8) == "https://") {
                ws_url = "wss://" + base.substr(8);
            }
            ws_url += "/api/demo/ws";
        }

        printf("  [client-%d] Connecting WebSocket: %s\n", id_, ws_url.c_str());

        // 创建 WebSocket 客户端（参考 demonet.txt 的 ws_->SetReceiveMessageCallback）
        mindbridge::net::WebSocketClientOptions ws_opts;
        ws_opts.ping_interval_ms = config_.ping_interval_ms;
        ws_opts.reconnect_interval_ms = 5000;
        ws_opts.auto_reconnect = true;
        ws_opts.http.timeout_sec = config_.timeout_sec;

        mindbridge::net::WebSocketClient ws(ws_url, ws_opts);

        std::atomic<int> recv_count{0};
        std::atomic<int> recv_bytes{0};
        std::mutex latencies_mutex;
        std::vector<double> latencies;

        // 设置消息回调（参考 demonet.txt 的 RecvWebsocketMsg）
        ws.set_message_callback([&](const std::string& msg) {
            recv_count.fetch_add(1);
            recv_bytes.fetch_add(msg.size());
            // 推送接收的延迟统计
            auto now = now_ms();
            std::lock_guard<std::mutex> lock(latencies_mutex);
            // 消息接收不单独计延迟，因为是推送模式
        });

        ws.set_error_callback([&](const std::string& err) {
            result.ws_errors++;
            printf("  [client-%d] WS error: %s\n", id_, err.c_str());
        });

        // 启动 WebSocket（参考 demonet.txt 的 ws_->StartSync）
        ws.start();

        // 等待连接建立
        int wait_ms = 0;
        while (!ws.connected() && wait_ms < 5000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            wait_ms += 100;
        }

        if (!ws.connected()) {
            printf("  [client-%d] WebSocket connect timeout\n", id_);
            ws.stop();
            return;
        }

        result.ws_connected = true;
        printf("  [client-%d] WebSocket connected\n", id_);

        // 发送认证消息（参考 demonet.txt 的 SendWebSocketAuth）
        if (!token.empty()) {
            nlohmann::json auth_msg = {
                {"method", "auth"},
                {"token", token}
            };
            ws.send(auth_msg.dump());
            printf("  [client-%d] Sent auth message\n", id_);
        }

        // 消息发送循环（参考 demonet.txt 的 SendConfigedMsg + StartSendMsg）
        std::vector<double> send_latencies;

        for (int i = 0; i < config_.requests_per_client; ++i) {
            // 构建消息（参考 demonet.txt 的消息格式）
            nlohmann::json msg = {
                {"method", "chat"},
                {"params", {{"messages", {{{"role", "user"}, {"content", config_.chat_message}}}}}}
            };

            auto t0 = now_ms();
            bool sent = ws.send(msg.dump());
            auto t1 = now_ms();

            if (sent) {
                result.success_count++;
                send_latencies.push_back(static_cast<double>(t1 - t0));
            } else {
                result.fail_count++;
            }
            result.total_messages++;
            result.total_bytes += msg.dump().size();

            if (config_.print_by_num > 0 && (i + 1) % config_.print_by_num == 0) {
                auto elapsed = now_ms() - start_time;
                printf("  [client-%d] %d/%d msgs sent, recv=%d, elapsed=%lldms\n",
                       id_, i + 1, config_.requests_per_client,
                       recv_count.load(), elapsed);
            }

            if (config_.request_interval_ms > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config_.request_interval_ms));
            }
        }

        // 等待最后的消息回来
        std::this_thread::sleep_for(std::chrono::seconds(2));

        result.ws_messages_received = recv_count.load();
        result.total_bytes += recv_bytes.load();

        // 心跳统计（WebSocket ping 由库自动处理）
        result.ping_count = config_.requests_per_client / 10;  // 估算
        result.ping_fail = result.ws_errors;

        latencies_ = std::move(send_latencies);

        // 关闭 WebSocket（参考 demonet.txt 的 ws_->Close）
        ws.stop();
        printf("  [client-%d] WebSocket closed, recv=%d messages\n", id_, recv_count.load());
    }

    // HTTP 模式（保持兼容）
    void run_http_mode(const std::string& token, Result& result, long long start_time) {
        std::vector<double> latencies;

        for (int i = 0; i < config_.requests_per_client; ++i) {
            double latency = 0;
            size_t bytes = 0;
            bool ok = false;

            if (config_.test_type == "health") {
                auto t0 = now_ms();
                std::string resp = http_get(config_.gateway_url + "/api/health", config_.timeout_sec);
                latency = static_cast<double>(now_ms() - t0);
                bytes = resp.size();
                ok = !resp.empty();
            } else if (config_.test_type == "chat") {
                std::string body = R"({"method":"chat","params":{"messages":[{"role":"user","content":")" +
                                   config_.chat_message + R"("}]}})";
                auto t0 = now_ms();
                std::string resp = http_post_json(config_.gateway_url + "/", body,
                                                   config_.timeout_sec, token);
                latency = static_cast<double>(now_ms() - t0);
                bytes = resp.size();
                ok = !resp.empty();
            } else if (config_.test_type == "login") {
                // 登录后测心跳
                auto t0 = now_ms();
                std::string resp = http_get(config_.gateway_url + "/api/health", config_.timeout_sec);
                latency = static_cast<double>(now_ms() - t0);
                bytes = resp.size();
                ok = !resp.empty();
            }

            latencies.push_back(latency);
            result.total_bytes += bytes;
            if (ok) result.success_count++;
            else result.fail_count++;
            result.total_messages++;

            if (config_.print_by_num > 0 && (i + 1) % config_.print_by_num == 0) {
                auto elapsed = now_ms() - start_time;
                printf("  [client-%d] %d/%d requests done, elapsed=%lldms\n",
                       id_, i + 1, config_.requests_per_client, elapsed);
            }

            if (config_.request_interval_ms > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config_.request_interval_ms));
            }
        }

        latencies_ = std::move(latencies);
    }

    void compute_latency_stats(Result& result) {
        if (latencies_.empty()) return;

        double sum = 0;
        for (auto l : latencies_) {
            sum += l;
            if (l < result.min_latency_ms) result.min_latency_ms = l;
            if (l > result.max_latency_ms) result.max_latency_ms = l;
        }
        result.avg_latency_ms = sum / latencies_.size();
    }

    int id_;
    StressConfig config_;
    std::vector<double> latencies_;
};

// ============================================================
// 压测管理器（参考 demonet.txt 的 CLoginClientManager）
// ============================================================

class StressTestManager {
public:
    void run(const StressConfig& config) {
        printf("========================================\n");
        printf("MindBridge Gateway Stress Test\n");
        printf("========================================\n");
        printf("Gateway:      %s\n", config.gateway_url.c_str());
        printf("Test Type:    %s\n", config.test_type.c_str());
        printf("Clients:      %d\n", config.client_count);
        printf("Messages/Client: %d\n", config.requests_per_client);
        printf("Total Messages:  %d\n", config.client_count * config.requests_per_client);
        printf("Interval:     %dms\n", config.request_interval_ms);
        printf("Timeout:      %ds\n", config.timeout_sec);
        if (config.test_type == "ws") {
            printf("Ping Interval: %dms\n", config.ping_interval_ms);
        }
        printf("========================================\n\n");

        // 1. 健康检查
        printf("[Pre-check] Testing gateway health...\n");
        auto health = http_get(config.gateway_url + "/api/health", 5);
        if (health.empty()) {
            printf("[ERROR] Gateway health check failed!\n");
            return;
        }
        printf("[Pre-check] Gateway is healthy.\n\n");

        // 2. 启动多客户端并发
        printf("[Running] Starting %d concurrent clients...\n\n", config.client_count);

        auto global_start = now_ms();

        std::vector<std::thread> threads;
        std::vector<WsStressClient::Result> results(config.client_count);
        std::mutex results_mutex;

        for (int i = 0; i < config.client_count; ++i) {
            threads.emplace_back([i, &config, &results, &results_mutex]() {
                WsStressClient client(i, config);
                auto result = client.run();
                std::lock_guard<std::mutex> lock(results_mutex);
                results[i] = result;
            });
        }

        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }

        auto global_elapsed = now_ms() - global_start;
        print_summary(results, global_elapsed, config);
    }

private:
    void print_summary(const std::vector<WsStressClient::Result>& results,
                       long long global_elapsed_ms,
                       const StressConfig& config) {
        int total_messages = 0;
        int total_success = 0;
        int total_fail = 0;
        size_t total_bytes = 0;
        int login_ok_count = 0;
        double sum_login_latency = 0;
        int ws_connected_count = 0;
        int ws_recv_total = 0;
        int ws_errors_total = 0;
        double min_latency = 1e18;
        double max_latency = 0;
        double sum_avg_latency = 0;

        for (const auto& r : results) {
            total_messages += r.total_messages;
            total_success += r.success_count;
            total_fail += r.fail_count;
            total_bytes += r.total_bytes;
            if (r.login_ok) {
                login_ok_count++;
                sum_login_latency += r.login_latency_ms;
            }
            if (r.ws_connected) ws_connected_count++;
            ws_recv_total += r.ws_messages_received;
            ws_errors_total += r.ws_errors;
            if (r.min_latency_ms < min_latency) min_latency = r.min_latency_ms;
            if (r.max_latency_ms > max_latency) max_latency = r.max_latency_ms;
            sum_avg_latency += r.avg_latency_ms;
        }

        double overall_avg = results.empty() ? 0 : sum_avg_latency / results.size();
        double qps = global_elapsed_ms > 0
                         ? static_cast<double>(total_messages) / global_elapsed_ms * 1000.0
                         : 0;
        double success_rate = total_messages > 0
                                  ? static_cast<double>(total_success) / total_messages * 100.0
                                  : 0;

        printf("\n========================================\n");
        printf("Stress Test Results\n");
        printf("========================================\n");
        printf("Test Type:        %s\n", config.test_type.c_str());
        printf("Total Messages:   %d\n", total_messages);
        printf("Success:          %d\n", total_success);
        printf("Failed:           %d\n", total_fail);
        printf("Success Rate:     %.2f%%\n", success_rate);

        if (config.test_type == "login" || config.test_type == "ws" || config.auth_required) {
            printf("----------------------------------------\n");
            printf("Login:\n");
            printf("  Success:        %d/%d\n", login_ok_count, config.client_count);
            printf("  Avg Latency:    %.2fms\n",
                   login_ok_count > 0 ? sum_login_latency / login_ok_count : 0);
        }

        if (config.test_type == "ws") {
            printf("----------------------------------------\n");
            printf("WebSocket:\n");
            printf("  Connected:      %d/%d\n", ws_connected_count, config.client_count);
            printf("  Messages Recv:  %d\n", ws_recv_total);
            printf("  Errors:         %d\n", ws_errors_total);
        }

        printf("----------------------------------------\n");
        printf("Total Time:       %lldms (%.2fs)\n", global_elapsed_ms,
               global_elapsed_ms / 1000.0);
        printf("QPS:              %.2f msg/s\n", qps);
        printf("Throughput:       %.2f KB/s\n",
               global_elapsed_ms > 0
                   ? static_cast<double>(total_bytes) / 1024.0 / (global_elapsed_ms / 1000.0)
                   : 0);
        printf("Total Data:       %.2f KB\n", total_bytes / 1024.0);
        if (overall_avg > 0) {
            printf("----------------------------------------\n");
            printf("Send Latency (ms):\n");
            printf("  Avg:            %.2f\n", overall_avg);
            printf("  Min:            %.2f\n", min_latency < 1e18 ? min_latency : 0);
            printf("  Max:            %.2f\n", max_latency);
        }
        printf("========================================\n");
    }

    // 用于全局延迟统计
};

// ============================================================
// main
// ============================================================

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --url URL           Gateway HTTP URL (default: http://127.0.0.1:8090)\n");
    printf("  --ws-url URL        WebSocket URL (auto-derived from --url if not set)\n");
    printf("  --clients N         Concurrent client count (default: 10)\n");
    printf("  --messages N        Messages per client (default: 100)\n");
    printf("  --type TYPE         Test type: health/chat/sse/login/ws (default: health)\n");
    printf("  --message MSG       Chat message content (default: 你好)\n");
    printf("  --interval MS       Message interval in ms (default: 0)\n");
    printf("  --ping MS           WebSocket ping interval in ms (default: 5000)\n");
    printf("  --timeout SEC       Request timeout in seconds (default: 30)\n");
    printf("  --print-by N        Print progress every N messages (default: 10)\n");
    printf("  --access-key ID     Login access_key_id\n");
    printf("  --secret-key KEY    Login secret_key\n");
    printf("  --auth              Enable auth\n");
    printf("  -h, --help          Show this help\n");
    printf("\nTest Types:\n");
    printf("  health  - HTTP GET /api/health\n");
    printf("  login   - Login + HTTP heartbeat\n");
    printf("  chat    - HTTP POST chat (JSON-RPC)\n");
    printf("  ws      - Login + WebSocket + ping + messaging (full lifecycle)\n");
}

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    StressConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--url" && i + 1 < argc) config.gateway_url = argv[++i];
        else if (arg == "--ws-url" && i + 1 < argc) config.ws_url = argv[++i];
        else if (arg == "--clients" && i + 1 < argc) config.client_count = std::atoi(argv[++i]);
        else if (arg == "--messages" && i + 1 < argc) config.requests_per_client = std::atoi(argv[++i]);
        else if (arg == "--type" && i + 1 < argc) config.test_type = argv[++i];
        else if (arg == "--message" && i + 1 < argc) config.chat_message = argv[++i];
        else if (arg == "--interval" && i + 1 < argc) config.request_interval_ms = std::atoi(argv[++i]);
        else if (arg == "--ping" && i + 1 < argc) config.ping_interval_ms = std::atoi(argv[++i]);
        else if (arg == "--timeout" && i + 1 < argc) config.timeout_sec = std::atoi(argv[++i]);
        else if (arg == "--print-by" && i + 1 < argc) config.print_by_num = std::atoi(argv[++i]);
        else if (arg == "--access-key" && i + 1 < argc) config.access_key_id = argv[++i];
        else if (arg == "--secret-key" && i + 1 < argc) config.secret_key = argv[++i];
        else if (arg == "--auth") config.auth_required = true;
        else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            curl_global_cleanup();
            return 0;
        } else {
            printf("Unknown option: %s\n", arg.c_str());
            print_usage(argv[0]);
            curl_global_cleanup();
            return 1;
        }
    }

    if (const char* url = std::getenv("MINDBRIDGE_GATEWAY_URL")) config.gateway_url = url;
    if (const char* key = std::getenv("MINDBRIDGE_ACCESS_KEY_ID")) config.access_key_id = key;
    if (const char* secret = std::getenv("MINDBRIDGE_SECRET_KEY")) config.secret_key = secret;

    StressTestManager manager;
    manager.run(config);

    curl_global_cleanup();
    return 0;
}
