#pragma once

#include <atomic>
#include <string>
#include <thread>

// 修改原因：现有 ASR 是“一直录音 + VAD 端点切句”。教学舱 V1 改为学员按住 PTT 录音、
// 松开后出识别结果。PttController 统一键盘 / evdev / GPIO / 命名管道输入，GPIO 等仅作扩展。

namespace cabin {

enum PttSourceType {
    PTT_SOURCE_AUTO = 0,
    PTT_SOURCE_EVDEV,
    PTT_SOURCE_GPIO,
    PTT_SOURCE_PIPE,
    PTT_SOURCE_TOGGLE,
    PTT_SOURCE_LEGACY
};

class PttController {
public:
    PttController();
    ~PttController();

    bool start();
    void stop();

    bool is_legacy_mode() const;
    bool is_pressed() const;
    bool consume_press_edge();
    bool consume_release_edge();

    const std::string &session_id() const;
    const std::string &role() const;
    const std::string &source_name() const;

    void inject_press();
    void inject_release();

    static std::string env_or(const char *key, const char *fallback);
    static PttSourceType parse_source(const std::string &name);

private:
    void worker_loop();
    void poll_evdev();
    void poll_gpio();
    void poll_pipe();
    void poll_toggle();
    void set_pressed(bool pressed);

    bool open_evdev();
    bool open_gpio();
    bool open_pipe();
    void restore_stdin();

    std::string session_id_;
    std::string role_;
    std::string source_name_;
    PttSourceType source_;
    int evdev_fd_;
    int gpio_fd_;
    int pipe_fd_;
    int ptt_key_code_;
    int stdin_saved_;
    bool stdin_raw_;
    bool running_;
    std::atomic<bool> pressed_;
    std::atomic<bool> press_edge_;
    std::atomic<bool> release_edge_;
    std::thread worker_;
};

}  // namespace cabin
