#pragma once

#include <string>

// V1 仅保留扩展点，不实现以下能力：
// AEC、WebRTC、RAG、知识库、评分系统、教员台、多 GPU 调度、
// 微服务拆分、EventBus 重构、数据库持久化。

namespace cabin_ext {

inline std::string rag_query(const std::string & /*session_id*/,
                             const std::string & /*query*/) {
    return std::string();
}

inline void knowledge_lookup(const std::string & /*topic*/) {}

inline void score_utterance(const std::string & /*session_id*/,
                            const std::string & /*user_text*/,
                            const std::string & /*assistant_text*/) {}

inline void instructor_console_publish(const std::string & /*event*/,
                                       const std::string & /*payload*/) {}

inline void event_bus_publish(const std::string & /*topic*/,
                              const std::string & /*payload*/) {}

inline void persist_turn(const std::string & /*session_id*/,
                         const std::string & /*role*/,
                         const std::string & /*user_text*/,
                         const std::string & /*assistant_text*/) {}

inline void aec_process(const float * /*mic*/,
                        const float * /*ref*/,
                        float * /*out*/,
                        int /*n*/) {}

inline void webrtc_ingest(const void * /*frame*/, int /*bytes*/) {}

inline void multi_gpu_schedule(const std::string & /*session_id*/) {}

inline void microservice_dispatch(const std::string & /*service*/,
                                  const std::string & /*payload*/) {}

}  // namespace cabin_ext
