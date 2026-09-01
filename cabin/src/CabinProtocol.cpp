#include "CabinProtocol.h"

#include <sstream>
#include <vector>

namespace cabin {

const char *CabinProtocol::kPrefix = "CABIN1|";
const char *CabinProtocol::kDefaultSession = "default";
const char *CabinProtocol::kDefaultRole = "instructor";

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

static std::vector<std::string> split_limited(const std::string &s, char delim, int max_parts) {
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == delim && static_cast<int>(parts.size()) < max_parts - 1) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(s[i]);
        }
    }
    parts.push_back(cur);
    return parts;
}

const char *CabinProtocol::type_to_string(CabinMsgType type) {
    switch (type) {
        case CABIN_MSG_ASR_TEXT:
            return "ASR_TEXT";
        case CABIN_MSG_PTT_DOWN:
            return "PTT_DOWN";
        case CABIN_MSG_PTT_UP:
            return "PTT_UP";
        case CABIN_MSG_TTS_TEXT:
            return "TTS_TEXT";
        case CABIN_MSG_TTS_END:
            return "TTS_END";
        case CABIN_MSG_TTS_INTERRUPT:
            return "TTS_INTERRUPT";
        case CABIN_MSG_PLAY_END:
            return "PLAY_END";
        case CABIN_MSG_LLM_ACK:
            return "LLM_ACK";
        case CABIN_MSG_LLM_ABORT:
            return "LLM_ABORT";
        default:
            return "UNKNOWN";
    }
}

CabinMsgType CabinProtocol::parse_type(const std::string &name) {
    const std::string n = to_lower_ascii(name);
    if (n == "asr_text") {
        return CABIN_MSG_ASR_TEXT;
    }
    if (n == "ptt_down") {
        return CABIN_MSG_PTT_DOWN;
    }
    if (n == "ptt_up") {
        return CABIN_MSG_PTT_UP;
    }
    if (n == "tts_text") {
        return CABIN_MSG_TTS_TEXT;
    }
    if (n == "tts_end") {
        return CABIN_MSG_TTS_END;
    }
    if (n == "tts_interrupt") {
        return CABIN_MSG_TTS_INTERRUPT;
    }
    if (n == "play_end") {
        return CABIN_MSG_PLAY_END;
    }
    if (n == "llm_ack") {
        return CABIN_MSG_LLM_ACK;
    }
    if (n == "llm_abort") {
        return CABIN_MSG_LLM_ABORT;
    }
    return CABIN_MSG_UNKNOWN;
}

std::string CabinProtocol::encode(CabinMsgType type,
                                  const std::string &session_id,
                                  const std::string &role,
                                  const std::string &payload) {
    std::ostringstream oss;
    oss << kPrefix << type_to_string(type) << "|"
        << (session_id.empty() ? kDefaultSession : session_id) << "|"
        << (role.empty() ? kDefaultRole : role) << "|"
        << payload;
    return oss.str();
}

CabinMessage CabinProtocol::decode(const std::string &raw) {
    CabinMessage msg;
    const std::string prefix = kPrefix;
    if (raw.size() < prefix.size() || raw.compare(0, prefix.size(), prefix) != 0) {
        msg.is_v1 = false;
        msg.type = CABIN_MSG_ASR_TEXT;
        msg.session_id = kDefaultSession;
        msg.role = kDefaultRole;
        msg.payload = raw;
        return msg;
    }

    const std::string body = raw.substr(prefix.size());
    const std::vector<std::string> parts = split_limited(body, '|', 4);
    msg.is_v1 = true;
    msg.type = parts.size() > 0 ? parse_type(parts[0]) : CABIN_MSG_UNKNOWN;
    msg.session_id = parts.size() > 1 && !parts[1].empty() ? parts[1] : kDefaultSession;
    msg.role = parts.size() > 2 && !parts[2].empty() ? parts[2] : kDefaultRole;
    msg.payload = parts.size() > 3 ? parts[3] : std::string();
    return msg;
}

bool CabinProtocol::is_end_marker(const std::string &text) {
    return text.find("END") != std::string::npos;
}

std::string CabinProtocol::strip_end_marker(const std::string &text) {
    const size_t pos = text.find("END");
    if (pos == std::string::npos) {
        return text;
    }
    return text.substr(0, pos);
}

}  // namespace cabin
