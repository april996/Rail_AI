#include "PttController.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace cabin {

static int env_int(const char *key, int fallback) {
    const char *v = std::getenv(key);
    if (!v || !v[0]) {
        return fallback;
    }
    return std::atoi(v);
}

std::string PttController::env_or(const char *key, const char *fallback) {
    const char *v = std::getenv(key);
    if (!v || !v[0]) {
        return fallback ? std::string(fallback) : std::string();
    }
    return std::string(v);
}

PttSourceType PttController::parse_source(const std::string &name) {
    if (name == "evdev") {
        return PTT_SOURCE_EVDEV;
    }
    if (name == "gpio") {
        return PTT_SOURCE_GPIO;
    }
    if (name == "pipe") {
        return PTT_SOURCE_PIPE;
    }
    if (name == "toggle" || name == "keyboard") {
        return PTT_SOURCE_TOGGLE;
    }
    if (name == "legacy" || name == "always" || name == "vad") {
        return PTT_SOURCE_LEGACY;
    }
    return PTT_SOURCE_AUTO;
}

PttController::PttController()
    : session_id_(env_or("CABIN_SESSION_ID", "default")),
      role_(env_or("CABIN_ROLE", "instructor")),
      source_name_(env_or("CABIN_PTT_SOURCE", "auto")),
      source_(parse_source(source_name_)),
      evdev_fd_(-1),
      gpio_fd_(-1),
      pipe_fd_(-1),
      ptt_key_code_(env_int("CABIN_PTT_KEY", 57)),
      stdin_saved_(0),
      stdin_raw_(false),
      running_(false),
      pressed_(false),
      press_edge_(false),
      release_edge_(false) {
    if (env_or("CABIN_PTT_MODE", "") == "legacy") {
        source_ = PTT_SOURCE_LEGACY;
        source_name_ = "legacy";
    }
}

PttController::~PttController() {
    stop();
}

bool PttController::is_legacy_mode() const {
    return source_ == PTT_SOURCE_LEGACY;
}

bool PttController::is_pressed() const {
    return pressed_.load();
}

bool PttController::consume_press_edge() {
    return press_edge_.exchange(false);
}

bool PttController::consume_release_edge() {
    return release_edge_.exchange(false);
}

const std::string &PttController::session_id() const {
    return session_id_;
}

const std::string &PttController::role() const {
    return role_;
}

const std::string &PttController::source_name() const {
    return source_name_;
}

void PttController::inject_press() {
    set_pressed(true);
}

void PttController::inject_release() {
    set_pressed(false);
}

void PttController::set_pressed(bool pressed) {
    const bool prev = pressed_.exchange(pressed);
    if (pressed && !prev) {
        press_edge_.store(true);
        std::cerr << "[PTT] press session=" << session_id_ << " role=" << role_ << std::endl;
    } else if (!pressed && prev) {
        release_edge_.store(true);
        std::cerr << "[PTT] release session=" << session_id_ << " role=" << role_ << std::endl;
    }
}

bool PttController::start() {
    if (running_) {
        return true;
    }

    if (source_ == PTT_SOURCE_AUTO) {
        if (open_evdev()) {
            source_ = PTT_SOURCE_EVDEV;
            source_name_ = "evdev";
        } else if (open_gpio()) {
            source_ = PTT_SOURCE_GPIO;
            source_name_ = "gpio";
        } else if (open_pipe()) {
            source_ = PTT_SOURCE_PIPE;
            source_name_ = "pipe";
        } else {
            source_ = PTT_SOURCE_TOGGLE;
            source_name_ = "toggle";
        }
    } else if (source_ == PTT_SOURCE_EVDEV) {
        open_evdev();
    } else if (source_ == PTT_SOURCE_GPIO) {
        open_gpio();
    } else if (source_ == PTT_SOURCE_PIPE) {
        open_pipe();
    }

    if (source_ == PTT_SOURCE_LEGACY) {
        pressed_.store(true);
        std::cerr << "[PTT] legacy always-on mode, session=" << session_id_ << std::endl;
        return true;
    }

    running_ = true;
    worker_ = std::thread(&PttController::worker_loop, this);
    std::cerr << "[PTT] started source=" << source_name_
              << " session=" << session_id_
              << " role=" << role_ << std::endl;
    return true;
}

void PttController::stop() {
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
#ifndef _WIN32
    if (evdev_fd_ >= 0) {
        close(evdev_fd_);
        evdev_fd_ = -1;
    }
    if (gpio_fd_ >= 0) {
        close(gpio_fd_);
        gpio_fd_ = -1;
    }
    if (pipe_fd_ >= 0) {
        close(pipe_fd_);
        pipe_fd_ = -1;
    }
    restore_stdin();
#endif
}

#ifndef _WIN32
static bool evdev_has_key(int fd, int key_code) {
    unsigned char key_bits[(KEY_MAX / 8) + 1];
    std::memset(key_bits, 0, sizeof(key_bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
        return false;
    }
    return (key_bits[key_code / 8] & (1 << (key_code % 8))) != 0;
}
#endif

bool PttController::open_evdev() {
#ifndef _WIN32
    std::string path = env_or("CABIN_PTT_EVDEV", "");
    if (!path.empty()) {
        evdev_fd_ = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (evdev_fd_ >= 0) {
            return true;
        }
        std::cerr << "[PTT] open evdev failed: " << path << std::endl;
        return false;
    }

    DIR *dir = opendir("/dev/input");
    if (!dir) {
        return false;
    }
    std::vector<std::string> candidates;
    struct dirent *ent = 0;
    while ((ent = readdir(dir)) != 0) {
        if (std::strncmp(ent->d_name, "event", 5) == 0) {
            candidates.push_back(std::string("/dev/input/") + ent->d_name);
        }
    }
    closedir(dir);

    for (size_t i = 0; i < candidates.size(); ++i) {
        int fd = open(candidates[i].c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        if (evdev_has_key(fd, ptt_key_code_)) {
            evdev_fd_ = fd;
            std::cerr << "[PTT] evdev device=" << candidates[i] << std::endl;
            return true;
        }
        close(fd);
    }
#endif
    return false;
}

bool PttController::open_gpio() {
#ifndef _WIN32
    const std::string path = env_or("CABIN_PTT_GPIO", "");
    if (path.empty()) {
        return false;
    }
    gpio_fd_ = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (gpio_fd_ < 0) {
        std::cerr << "[PTT] open gpio failed: " << path << std::endl;
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool PttController::open_pipe() {
#ifndef _WIN32
    std::string path = env_or("CABIN_PTT_PIPE", "/tmp/cabin_ptt");
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        if (mkfifo(path.c_str(), 0666) != 0 && errno != EEXIST) {
            std::cerr << "[PTT] mkfifo failed: " << path << std::endl;
            return false;
        }
    }
    pipe_fd_ = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (pipe_fd_ < 0) {
        std::cerr << "[PTT] open pipe failed: " << path << std::endl;
        return false;
    }
    std::cerr << "[PTT] pipe=" << path << " write 1/0 or down/up" << std::endl;
    return true;
#else
    return false;
#endif
}

void PttController::restore_stdin() {
#ifndef _WIN32
    if (!stdin_raw_) {
        return;
    }
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) == 0) {
        t.c_lflag = static_cast<tcflag_t>(stdin_saved_);
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
    stdin_raw_ = false;
#endif
}

void PttController::poll_evdev() {
#ifndef _WIN32
    if (evdev_fd_ < 0) {
        return;
    }
    struct input_event ev;
    while (read(evdev_fd_, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
        if (ev.type == EV_KEY && ev.code == ptt_key_code_) {
            if (ev.value == 1) {
                set_pressed(true);
            } else if (ev.value == 0) {
                set_pressed(false);
            }
        }
    }
#endif
}

void PttController::poll_gpio() {
#ifndef _WIN32
    if (gpio_fd_ < 0) {
        return;
    }
    if (lseek(gpio_fd_, 0, SEEK_SET) < 0) {
        return;
    }
    char buf[8];
    const ssize_t n = read(gpio_fd_, buf, sizeof(buf) - 1);
    if (n <= 0) {
        return;
    }
    buf[n] = 0;
    const bool down = (buf[0] == '1' || buf[0] == 'h' || buf[0] == 'H');
    set_pressed(down);
#endif
}

static bool parse_ptt_token(const std::string &token, bool *down) {
    if (token == "1" || token == "down" || token == "press" || token == "on") {
        *down = true;
        return true;
    }
    if (token == "0" || token == "up" || token == "release" || token == "off") {
        *down = false;
        return true;
    }
    return false;
}

void PttController::poll_pipe() {
#ifndef _WIN32
    if (pipe_fd_ < 0) {
        return;
    }
    char buf[64];
    const ssize_t n = read(pipe_fd_, buf, sizeof(buf) - 1);
    if (n <= 0) {
        return;
    }
    buf[n] = 0;
    std::istringstream iss(buf);
    std::string token;
    while (iss >> token) {
        bool down = false;
        if (parse_ptt_token(token, &down)) {
            set_pressed(down);
        }
    }
#endif
}

void PttController::poll_toggle() {
#ifndef _WIN32
    if (!stdin_raw_) {
        struct termios t;
        if (tcgetattr(STDIN_FILENO, &t) == 0) {
            stdin_saved_ = static_cast<int>(t.c_lflag);
            t.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
            t.c_cc[VMIN] = 0;
            t.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
            stdin_raw_ = true;
            std::cerr << "[PTT] toggle mode: press SPACE or p to start/stop talking" << std::endl;
        }
    }
    char c = 0;
    const ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) {
        if (c == ' ' || c == 'p' || c == 'P') {
            set_pressed(!pressed_.load());
        } else if (c == '1') {
            set_pressed(true);
        } else if (c == '0') {
            set_pressed(false);
        }
    }
#endif
}

void PttController::worker_loop() {
    while (running_) {
        if (source_ == PTT_SOURCE_EVDEV) {
            poll_evdev();
        } else if (source_ == PTT_SOURCE_GPIO) {
            poll_gpio();
        } else if (source_ == PTT_SOURCE_PIPE) {
            poll_pipe();
        } else {
            poll_toggle();
        }
#ifndef _WIN32
        usleep(20 * 1000);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
#endif
    }
}

}  // namespace cabin
