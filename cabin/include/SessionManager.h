#pragma once

#include "CabinProtocol.h"
#include "RoleRouter.h"

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// 修改原因：现有 LLM 进程是单用户、无历史隔离。V1 用 SessionManager 在同一 RKLLM
// 句柄上按 session_id 隔离对话上下文，多学员串行推理，不引入多 GPU / 微服务。

namespace cabin {

enum SessionState {
    SESSION_IDLE = 0,
    SESSION_LISTENING,
    SESSION_THINKING,
    SESSION_SPEAKING
};

struct SessionContext {
    std::string session_id;
    std::string role;
    SessionState state;
    bool interrupt_requested;
    std::vector<std::pair<std::string, std::string> > history;
    std::string last_user_text;
    std::string last_assistant_text;

    SessionContext()
        : session_id("default"),
          role("instructor"),
          state(SESSION_IDLE),
          interrupt_requested(false) {}
};

class SessionManager {
public:
    static SessionManager &instance();

    SessionContext acquire(const std::string &session_id, const std::string &role);
    void set_role(const std::string &session_id, const std::string &role);
    void set_state(const std::string &session_id, SessionState state);
    void append_user(const std::string &session_id, const std::string &text);
    void append_assistant(const std::string &session_id, const std::string &text);
    void request_interrupt(const std::string &session_id);
    bool consume_interrupt(const std::string &session_id);
    void clear_history(const std::string &session_id);
    std::size_t session_count() const;

    std::string build_prompt(const std::string &session_id, const std::string &user_text);

    void set_max_turns(std::size_t max_turns);

private:
    SessionManager();
    SessionManager(const SessionManager &);
    SessionManager &operator=(const SessionManager &);

    SessionContext &ensure_locked(const std::string &session_id, const std::string &role);
    void trim_locked(SessionContext &ctx);

    mutable std::mutex mutex_;
    std::map<std::string, SessionContext> sessions_;
    std::size_t max_turns_;
};

}  // namespace cabin
