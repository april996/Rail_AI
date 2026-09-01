#pragma once

#include <string>

// 地铁驾驶教学模拟舱 V1 通信协议。
// 修改原因：在不改现有 ZMQ REQ/REP 端口与纯文本兼容路径的前提下，
// 为多学员会话、角色路由、PTT 打断提供可解析信封。
// 格式：CABIN1|<type>|<session_id>|<role>|<payload>
// 非 CABIN1| 前缀的消息一律按旧版纯文本处理（session=default, role=instructor）。

namespace cabin {

enum CabinMsgType {
    CABIN_MSG_ASR_TEXT = 0,
    CABIN_MSG_PTT_DOWN,
    CABIN_MSG_PTT_UP,
    CABIN_MSG_TTS_TEXT,
    CABIN_MSG_TTS_END,
    CABIN_MSG_TTS_INTERRUPT,
    CABIN_MSG_PLAY_END,
    CABIN_MSG_LLM_ACK,
    CABIN_MSG_LLM_ABORT,
    CABIN_MSG_UNKNOWN
};

struct CabinMessage {
    bool is_v1;
    CabinMsgType type;
    std::string session_id;
    std::string role;
    std::string payload;

    CabinMessage()
        : is_v1(false),
          type(CABIN_MSG_ASR_TEXT),
          session_id("default"),
          role("instructor") {}
};

class CabinProtocol {
public:
    static const char *kPrefix;
    static const char *kDefaultSession;
    static const char *kDefaultRole;

    static const char *type_to_string(CabinMsgType type);
    static CabinMsgType parse_type(const std::string &name);

    static std::string encode(CabinMsgType type,
                              const std::string &session_id,
                              const std::string &role,
                              const std::string &payload);

    static CabinMessage decode(const std::string &raw);

    static bool is_end_marker(const std::string &text);
    static std::string strip_end_marker(const std::string &text);
};

}  // namespace cabin
