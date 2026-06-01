#include "mindbridge/apps/app_support.hpp"
#include "mindbridge/net/http_client.hpp"
#include "mindbridge/net/websocket_client.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace {

struct DemoConfig {
    std::string gateway_url{"http://127.0.0.1:8090"};
    std::string websocket_url{"ws://127.0.0.1:8090/api/demo/ws"};
    int client_count{2};
    int request_frequency_ms{1000};
    int duration_sec{10};
    bool enable_http{true};
    bool enable_sse{false};
    bool enable_websocket{true};
    bool enable_subscribe{true};
};

struct DemoStats {
    std::atomic<int> http_ok{0};
    std::atomic<int> http_error{0};
    std::atomic<int> sse_events{0};
    std::atomic<int> sse_error{0};
    std::atomic<int> ws_messages{0};
    std::atomic<int> ws_errors{0};
};

DemoConfig load_config(const std::string& path) {
    DemoConfig config;
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open config: " + path);
    }
    const json raw = json::parse(in);
    config.gateway_url = raw.value("gateway_url", config.gateway_url);
    config.websocket_url = raw.value("websocket_url", config.websocket_url);
    config.client_count = raw.value("client_count", config.client_count);
    config.request_frequency_ms = raw.value("request_frequency_ms", config.request_frequency_ms);
    config.duration_sec = raw.value("duration_sec", config.duration_sec);
    config.enable_http = raw.value("enable_http", config.enable_http);
    config.enable_sse = raw.value("enable_sse", config.enable_sse);
    config.enable_websocket = raw.value("enable_websocket", config.enable_websocket);
    config.enable_subscribe = raw.value("enable_subscribe", config.enable_subscribe);
    return config;
}

mindbridge::net::HttpClientOptions http_options_from_env() {
    mindbridge::net::HttpClientOptions options;
    options.verify_tls = mindbridge::app::env_or("MINDBRIDGE_NET_TLS_VERIFY", "true") != "false";
    options.ca_file = mindbridge::app::env_or("MINDBRIDGE_NET_CA_FILE");
    options.timeout_sec = 30;
    return options;
}

json message_request(int client_index, int sequence, bool stream) {
    const std::string id = "net-demo-" + std::to_string(client_index) + "-" + std::to_string(sequence);
    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", stream ? "message/stream" : "message/send"},
        {"params",
         {{"message",
           {{"role", "user"},
            {"contextId", "net-demo-client-" + std::to_string(client_index)},
            {"parts", json::array({{{"kind", "text"}, {"text", "MindBridge net demo smoke request"}}})}}}}},
        {"metadata", {{"request_id", id}, {"source", "mindbridge_net_demo"}}},
    };
}

void run_http_client(int client_index,
                     const DemoConfig& config,
                     DemoStats& stats,
                     std::atomic<bool>& running,
                     const mindbridge::net::HttpClientOptions& options) {
    int sequence = 0;
    while (running.load()) {
        try {
            const auto response =
                mindbridge::net::post_json(config.gateway_url, message_request(client_index, sequence, false).dump(), options);
            if (response.status_code >= 200 && response.status_code < 300) {
                ++stats.http_ok;
            } else {
                ++stats.http_error;
            }
        } catch (const std::exception& e) {
            ++stats.http_error;
            std::cerr << "[net_demo] http client " << client_index << " error: " << e.what() << std::endl;
        }
        ++sequence;
        std::this_thread::sleep_for(std::chrono::milliseconds(config.request_frequency_ms));
    }
}

void run_sse_client(int client_index,
                    const DemoConfig& config,
                    DemoStats& stats,
                    const mindbridge::net::HttpClientOptions& options) {
    try {
        mindbridge::net::post_sse(
            config.gateway_url,
            message_request(client_index, 0, true).dump(),
            [&](const std::string&) {
                ++stats.sse_events;
                return stats.sse_events.load() < 20;
            },
            options);
    } catch (const std::exception& e) {
        ++stats.sse_error;
        std::cerr << "[net_demo] sse client " << client_index << " error: " << e.what() << std::endl;
    }
}

std::unique_ptr<mindbridge::net::WebSocketClient> start_ws_client(int client_index,
                                                                  const DemoConfig& config,
                                                                  DemoStats& stats,
                                                                  const mindbridge::net::HttpClientOptions& http_options) {
    mindbridge::net::WebSocketClientOptions options;
    options.http = http_options;
    options.ping_interval_ms = config.request_frequency_ms;
    options.reconnect_interval_ms = 2000;
    options.auto_reconnect = true;

    auto client = std::make_unique<mindbridge::net::WebSocketClient>(config.websocket_url, options);
    client->set_message_callback([&stats, client_index](const std::string& message) {
        ++stats.ws_messages;
        std::cout << "[net_demo] ws client " << client_index << " recv: " << message << std::endl;
    });
    client->set_error_callback([&stats, client_index](const std::string& message) {
        ++stats.ws_errors;
        std::cerr << "[net_demo] ws client " << client_index << " error: " << message << std::endl;
    });
    client->start();
    client->send(json{{"type", "auth"}, {"client_id", "net-demo-client-" + std::to_string(client_index)}}.dump());
    if (config.enable_subscribe) {
        client->send(json{{"type", "subscribe"}, {"topic", "health"}}.dump());
    }
    return client;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string config_path =
        argc > 1 ? argv[1] : "mindbridge_harness/configs/net_demo.json";
    const DemoConfig config = load_config(config_path);
    const auto http_options = http_options_from_env();
    DemoStats stats;
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;
    std::vector<std::unique_ptr<mindbridge::net::WebSocketClient>> ws_clients;

    std::cout << "[net_demo] gateway=" << config.gateway_url
              << " websocket=" << config.websocket_url
              << " clients=" << config.client_count
              << " duration_sec=" << config.duration_sec << std::endl;

    for (int i = 0; i < config.client_count; ++i) {
        if (config.enable_http) {
            threads.emplace_back(run_http_client, i, std::cref(config), std::ref(stats),
                                 std::ref(running), std::cref(http_options));
        }
        if (config.enable_sse) {
            threads.emplace_back(run_sse_client, i, std::cref(config), std::ref(stats),
                                 std::cref(http_options));
        }
        if (config.enable_websocket) {
            ws_clients.push_back(start_ws_client(i, config, stats, http_options));
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(config.duration_sec);
    int heartbeat = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        for (std::size_t i = 0; i < ws_clients.size(); ++i) {
            ws_clients[i]->send(json{{"type", "heartbeat"},
                                     {"client_id", "net-demo-client-" + std::to_string(i)},
                                     {"sequence", heartbeat}}
                                    .dump());
        }
        ++heartbeat;
        std::this_thread::sleep_for(std::chrono::milliseconds(config.request_frequency_ms));
    }

    running.store(false);
    for (auto& client : ws_clients) {
        client->stop();
    }
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    const json summary = {
        {"http_ok", stats.http_ok.load()},
        {"http_error", stats.http_error.load()},
        {"sse_events", stats.sse_events.load()},
        {"sse_error", stats.sse_error.load()},
        {"ws_messages", stats.ws_messages.load()},
        {"ws_errors", stats.ws_errors.load()},
    };
    std::cout << summary.dump(2) << std::endl;
    return stats.http_error.load() == 0 && stats.sse_error.load() == 0 ? 0 : 1;
}
