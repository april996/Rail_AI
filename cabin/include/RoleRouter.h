#pragma once

#include <string>
#include <utility>
#include <vector>

// 修改原因：教学模拟舱需要按 Instructor / Dispatcher / Passenger 切换系统 Prompt，
// 但不能改 RKLLM 单实例部署方式。RoleRouter 只负责拼装 Prompt，推理仍走现有 rkllm_run。

namespace cabin {

enum CabinRole {
    CABIN_ROLE_INSTRUCTOR = 0,
    CABIN_ROLE_DISPATCHER,
    CABIN_ROLE_PASSENGER
};

class RoleRouter {
public:
    static CabinRole parse(const std::string &name);
    static const char *to_string(CabinRole role);
    static std::string normalize(const std::string &name);

    static std::string system_prompt(CabinRole role);

    // history 元素 first=user|assistant，second=内容
    static std::string build_infer_prompt(
        CabinRole role,
        const std::vector<std::pair<std::string, std::string> > &history,
        const std::string &user_text);

    // 扩展点：RAG / 知识库检索注入。V1 默认空实现。
    static void set_rag_hook(std::string (*hook)(const std::string &session_id,
                                                 const std::string &role,
                                                 const std::string &query));
    static std::string apply_rag_hook(const std::string &session_id,
                                      const std::string &role,
                                      const std::string &query);

private:
    static std::string (*rag_hook_)(const std::string &, const std::string &, const std::string &);
};

}  // namespace cabin
