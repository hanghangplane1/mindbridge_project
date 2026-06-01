#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace mindbridge {
namespace auth {

struct AuthIdentity {
    std::string user_id;
    std::string display_name;
    std::string role{"user"};
    std::string auth_session_id;
};

struct LoginResult {
    bool ok{false};
    std::string error;
    std::string token;
    std::string expires_at;
    AuthIdentity identity;
};

class AuthService {
public:
    explicit AuthService(std::string db_path = ".mindbridge/mindbridge.db");

    void initialize();
    void bootstrap_from_env();
    LoginResult login(const std::string& access_key_id, const std::string& secret_key);
    std::optional<AuthIdentity> authenticate_token(const std::string& token);
    void logout(const std::string& token);

    std::string scoped_conversation_id(const AuthIdentity& identity,
                                       const std::string& client_context_id) const;
    void record_turn(const AuthIdentity& identity,
                     const std::string& conversation_id,
                     const std::string& role,
                     const std::string& content,
                     const std::string& run_id);
    void record_run(const AuthIdentity& identity,
                    const std::string& conversation_id,
                    const std::string& run_id);
    bool owns_run(const AuthIdentity& identity, const std::string& run_id) const;
    std::string latest_run_id_for(const AuthIdentity& identity) const;
    nlohmann::json list_conversations(const AuthIdentity& identity) const;
    nlohmann::json conversation_history(const AuthIdentity& identity,
                                        const std::string& conversation_id) const;

    static std::string hash_secret_for_bootstrap(const std::string& salt,
                                                 const std::string& secret);

private:
    std::string db_path_;
};

}  // namespace auth
}  // namespace mindbridge
