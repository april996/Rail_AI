#include "TTSModel.h"
#include "MessageQueue.h"
#include "AudioPlayer.h"
#include "TextProcessor.h"
#include "Utils.h"
#include "ZmqServer.h"
#include "CabinProtocol.h"

#include <thread>
#include <iostream>
#include <atomic>
#include <memory>
#include <cstring>
#include <exception>

// 修改原因：复用双队列伪流式合成+播放，增加 CABIN1 协议、PTT 打断（6688）、
// 丢弃中止后的过期分句。旧版纯文本 + END 握手保持不变。

zmq_component::ZmqServer server("tcp://*:7777");
zmq_component::ZmqServer status_server("tcp://*:6677");
std::atomic<bool> first_msg(true);
std::atomic<bool> g_tts_interrupt(false);
std::atomic<bool> g_drop_stale(false);
std::atomic<bool> g_status_pending(false);
std::atomic<bool> g_running(true);

void synthesis_worker(DoubleMessageQueue &queue, TTSModel &model) {
    while (g_running.load()) {
        std::string text = queue.pop_text();
        if (text.empty()) {
            if (queue.stopped()) {
                break;
            }
            continue;
        }

        if (g_tts_interrupt.load() || g_drop_stale.load()) {
            if (cabin::CabinProtocol::is_end_marker(text)) {
                g_drop_stale.store(false);
                first_msg.store(true);
            }
            continue;
        }

        bool is_end = cabin::CabinProtocol::is_end_marker(text);
        if (is_end) {
            first_msg.store(true);
            text = cabin::CabinProtocol::strip_end_marker(text);
        }

        int32_t audio_len = 0;
        if (!text.empty()) {
            std::cout << "[TTS infer] Inferring text: " << text << std::endl;
            int16_t* wavData = model.infer(text, audio_len);

            if (g_tts_interrupt.load() || g_drop_stale.load()) {
                if (wavData) {
                    model.free_data(wavData);
                }
                continue;
            }

            if (wavData && audio_len > 0) {
                auto audio_data = std::make_unique<int16_t[]>(audio_len);
                memcpy(audio_data.get(), wavData, audio_len * sizeof(int16_t));
                queue.push_audio(std::move(audio_data), audio_len, is_end);
                model.free_data(wavData);
            }
        } else if (is_end) {
            auto empty_audio = std::make_unique<int16_t[]>(0);
            queue.push_audio(std::move(empty_audio), 0, true);
        }
    }
}

void playback_worker(DoubleMessageQueue &queue, AudioPlayer &player) {
    while (g_running.load()) {
        auto msg = queue.pop_audio();
        if (msg.data == nullptr && queue.stopped()) {
            break;
        }

        if (g_tts_interrupt.load() || g_drop_stale.load()) {
            if (msg.is_last && g_status_pending.exchange(false)) {
                try {
                    status_server.send("[tts -> voice]play interrupted");
                } catch (const std::exception &e) {
                    std::cerr << "[tts] status send: " << e.what() << std::endl;
                }
            }
            continue;
        }

        if (msg.data && msg.length > 0) {
            player.play(msg.data.get(), static_cast<int>(msg.length * sizeof(int16_t)), 1.0f);
        }

        if (msg.is_last || player.interrupted()) {
            if (g_status_pending.exchange(false)) {
                try {
                    status_server.send(player.interrupted()
                                           ? "[tts -> voice]play interrupted"
                                           : "[tts -> voice]play end success");
                } catch (const std::exception &e) {
                    std::cerr << "[tts] status send: " << e.what() << std::endl;
                }
            }
            player.reset_interrupt();
        }
    }
}

void status_worker() {
    while (g_running.load()) {
        try {
            std::string req = status_server.receive();
            std::cout << "[voice -> tts] received: " << req << std::endl;
            g_status_pending.store(true);
        } catch (const std::exception &e) {
            std::cerr << "[tts] status server: " << e.what() << std::endl;
        }
    }
}

void interrupt_worker(DoubleMessageQueue &queue, AudioPlayer &player) {
    zmq_component::ZmqServer interrupt_server("tcp://*:6688");
    while (g_running.load()) {
        try {
            std::string req = interrupt_server.receive();
            cabin::CabinMessage msg = cabin::CabinProtocol::decode(req);
            std::cout << "[voice -> tts] interrupt: " << req << std::endl;
            if (!msg.is_v1 ||
                msg.type == cabin::CABIN_MSG_TTS_INTERRUPT ||
                msg.type == cabin::CABIN_MSG_PTT_DOWN ||
                req.find("INTERRUPT") != std::string::npos) {
                g_tts_interrupt.store(true);
                g_drop_stale.store(true);
                queue.clear();
                player.interrupt();
                first_msg.store(true);
                if (g_status_pending.exchange(false)) {
                    try {
                        status_server.send("[tts -> voice]play interrupted");
                    } catch (...) {
                    }
                }
            }
            interrupt_server.send("INTERRUPTED");
        } catch (const std::exception &e) {
            std::cerr << "[tts] interrupt server: " << e.what() << std::endl;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <model_path>" << std::endl;
        return 1;
    }

    try {
        TTSModel model(argv[1]);
        AudioPlayer player;
        DoubleMessageQueue queue;

        std::thread synthesis_thread(synthesis_worker, std::ref(queue), std::ref(model));
        std::thread playback_thread(playback_worker, std::ref(queue), std::ref(player));
        std::thread interrupt_thread(interrupt_worker, std::ref(queue), std::ref(player));
        std::thread status_thread(status_worker);
        interrupt_thread.detach();
        status_thread.detach();

        while (true) {
            std::string text = server.receive();
            server.send("Echo: received");
            std::cout << "[llm -> tts] received: " << text << std::endl;

            cabin::CabinMessage msg = cabin::CabinProtocol::decode(text);
            if (msg.type == cabin::CABIN_MSG_TTS_INTERRUPT) {
                g_tts_interrupt.store(true);
                g_drop_stale.store(true);
                queue.clear();
                player.interrupt();
                continue;
            }

            std::string body = msg.is_v1 ? msg.payload : text;
            if (body.empty() && msg.type == cabin::CABIN_MSG_TTS_END) {
                body = "END";
            }

            if (g_drop_stale.load()) {
                if (cabin::CabinProtocol::is_end_marker(body) || msg.type == cabin::CABIN_MSG_TTS_END) {
                    g_drop_stale.store(false);
                    g_tts_interrupt.store(false);
                    player.reset_interrupt();
                }
                continue;
            }

            g_tts_interrupt.store(false);

            if (!body.empty() && body.find("<think>") == std::string::npos) {
                if (msg.type == cabin::CABIN_MSG_TTS_END && body.find("END") == std::string::npos) {
                    body.append(" END");
                }
                queue.push_text(body);
            }
        }

        g_running.store(false);
        queue.stop();
        synthesis_thread.join();
        playback_thread.join();
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
