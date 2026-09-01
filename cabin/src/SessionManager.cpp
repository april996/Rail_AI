#include "SessionManager.h"

#include "CabinExtensions.h"

namespace cabin {

SessionManager &SessionManager::instance() {
    static SessionManager mgr;
    return mgr;
}

SessionManager::SessionManager() : max_turns_(3) {}

void SessionManager::set_max_turns(std::size_t max_turns) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_turns_ = max_turns == 0 ? 1 : max_turns;
}

SessionContext &SessionManager::ensure_locked(const std::string &session_id,
                                              const std::string &role) {
    const std::string sid = session_id.empty() ? CabinProtocol::kDefaultSession : session_id;
    std::map<std::string, SessionContext>::iterator it = sessions_.find(sid);
    if (it == sessions_.end()) {
        SessionContext ctx;
        ctx.session_id = sid;
        ctx.role = RoleRouter::normalize(role);
        sessions_[sid] = ctx;
        return sessions_[sid];
    }
    if (!role.empty()) {
        it->second.role = RoleRouter::normalize(role);
    }
    return it->second;
}

void SessionManager::trim_locked(SessionContext &ctx) {
    const std::size_t max_items = max_turns_ * 2;
    while (ctx.history.size() > max_items) {
        ctx.history.erase(ctx.history.begin());
    }
}

SessionContext SessionManager::acquire(const std::string &session_id, const std::string &role) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ensure_locked(session_id, role);
}

void SessionManager::set_role(const std::string &session_id, const std::string &role) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_locked(session_id, role).role = RoleRouter::normalize(role);
}

void SessionManager::set_state(const std::string &session_id, SessionState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_locked(session_id, "").state = state;
}

void SessionManager::append_user(const std::string &session_id, const std::string &text) {
    std::lock_guard<std::mutex> lock(mutex_);
    SessionContext &ctx = ensure_locked(session_id, "");
    ctx.last_user_text = text;
    ctx.history.push_back(std::make_pair(std::string("user"), text));
    ctx.interrupt_requested = false;
    ctx.state = SESSION_THINKING;
    trim_locked(ctx);
}

void SessionManager::append_assistant(const std::string &session_id, const std::string &text) {
    std::lock_guard<std::mutex> lock(mutex_);
    SessionContext &ctx = ensure_locked(session_id, "");
    ctx.last_assistant_text = text;
    ctx.history.push_back(std::make_pair(std::string("assistant"), text));
    ctx.state = SESSION_SPEAKING;
    trim_locked(ctx);
    cabin_ext::persist_turn(ctx.session_id, ctx.role, ctx.last_user_text, text);
    cabin_ext::score_utterance(ctx.session_id, ctx.last_user_text, text);
    cabin_ext::event_bus_publish("session.turn", ctx.session_id);
}

void SessionManager::request_interrupt(const std::string &session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    SessionContext &ctx = ensure_locked(session_id, "");
    ctx.interrupt_requested = true;
    ctx.state = SESSION_LISTENING;
}

bool SessionManager::consume_interrupt(const std::string &session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    SessionContext &ctx = ensure_locked(session_id, "");
    const bool v = ctx.interrupt_requested;
    ctx.interrupt_requested = false;
    return v;
}

void SessionManager::clear_history(const std::string &session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    SessionContext &ctx = ensure_locked(session_id, "");
    ctx.history.clear();
    ctx.last_user_text.clear();
    ctx.last_assistant_text.clear();
    ctx.state = SESSION_IDLE;
}

std::size_t SessionManager::session_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

std::string SessionManager::build_prompt(const std::string &session_id, const std::string &user_text) {
    std::lock_guard<std::mutex> lock(mutex_);
    SessionContext &ctx = ensure_locked(session_id, "");
    return RoleRouter::build_infer_prompt(RoleRouter::parse(ctx.role), ctx.history, user_text);
}

}  // namespace cabin
