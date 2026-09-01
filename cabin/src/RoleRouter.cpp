#include "RoleRouter.h"

#include "CabinExtensions.h"

namespace cabin {

std::string (*RoleRouter::rag_hook_)(const std::string &, const std::string &, const std::string &) = 0;

static std::string to_lower_ascii(const std::string &s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        char c = out[i];
        if (c >= 'A' && c <= 'Z') {
            out[i] = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

CabinRole RoleRouter::parse(const std::string &name) {
    const std::string n = to_lower_ascii(name);
    if (n == "dispatcher" || n == "dd" || n == "tiaodu" || n == "调度" || n == "调度员") {
        return CABIN_ROLE_DISPATCHER;
    }
    if (n == "passenger" || n == "psg" || n == "chengke" || n == "乘客") {
        return CABIN_ROLE_PASSENGER;
    }
    return CABIN_ROLE_INSTRUCTOR;
}

const char *RoleRouter::to_string(CabinRole role) {
    switch (role) {
        case CABIN_ROLE_DISPATCHER:
            return "dispatcher";
        case CABIN_ROLE_PASSENGER:
            return "passenger";
        default:
            return "instructor";
    }
}

std::string RoleRouter::normalize(const std::string &name) {
    return to_string(parse(name));
}

std::string RoleRouter::system_prompt(CabinRole role) {
    switch (role) {
        case CABIN_ROLE_DISPATCHER:
            return "你是地铁行车调度员。只用标准调度口语与司机对话，下达运行、停车、折返、清客、限速等指令。"
                   "不要输出思考过程，不要解释自己是模型。每次回答不超过40字。";
        case CABIN_ROLE_PASSENGER:
            return "你是地铁车厢乘客。用口语向司机反映车门、报站、拥挤或紧急情况。"
                   "不要输出思考过程，不要解释自己是模型。每次回答不超过30字。";
        case CABIN_ROLE_INSTRUCTOR:
        default:
            return "你是地铁驾驶教学模拟舱的AI教员。针对学员语音，用简短口语纠正操作、讲解规章和驾驶要领。"
                   "不要输出思考过程，不要使用Markdown。每次回答不超过80字。";
    }
}

std::string RoleRouter::build_infer_prompt(
    CabinRole role,
    const std::vector<std::pair<std::string, std::string> > &history,
    const std::string &user_text) {
    std::string prompt = system_prompt(role);
    prompt.append("\n");

    const std::string rag = apply_rag_hook("", to_string(role), user_text);
    if (!rag.empty()) {
        prompt.append("参考：");
        prompt.append(rag);
        prompt.append("\n");
    }

    const size_t n = history.size();
    size_t begin = 0;
    if (n > 4) {
        begin = n - 4;
    }
    for (size_t i = begin; i < n; ++i) {
        if (history[i].first == "assistant") {
            prompt.append("教员：");
        } else {
            prompt.append("学员：");
        }
        prompt.append(history[i].second);
        prompt.append("\n");
    }
    prompt.append("学员：");
    prompt.append(user_text);
    prompt.append("\n教员：");
    return prompt;
}

void RoleRouter::set_rag_hook(std::string (*hook)(const std::string &,
                                                  const std::string &,
                                                  const std::string &)) {
    rag_hook_ = hook;
}

std::string RoleRouter::apply_rag_hook(const std::string &session_id,
                                       const std::string &role,
                                       const std::string &query) {
    if (rag_hook_) {
        return rag_hook_(session_id, role, query);
    }
    return cabin_ext::rag_query(session_id, query);
}

}  // namespace cabin
