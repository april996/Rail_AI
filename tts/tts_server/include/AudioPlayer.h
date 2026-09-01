#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <atomic>
#include <cstdint>
#include <alsa/asoundlib.h>

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();
    
    bool initialize();
    void play(const int16_t* audioData, int audio_len, float speed = 1.0f);
    void interrupt();
    void reset_interrupt();
    bool interrupted() const;
    
private:
    void cleanup();
    
    snd_pcm_t* pcm_handle_ = nullptr;
    bool initialized_ = false;
    std::atomic<bool> interrupt_{false};
};

#endif // AUDIO_PLAYER_H