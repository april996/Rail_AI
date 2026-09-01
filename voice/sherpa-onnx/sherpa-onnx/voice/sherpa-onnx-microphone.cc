// sherpa-onnx/csrc/sherpa-onnx-microphone.cc
//
// Copyright (c)  2022-2023  Xiaomi Corporation
//
// 修改原因：复用现有流式 OnlineRecognizer + ZMQ 闭环，增加 PTT 门控录音、
// 多学员 session/role 信封，以及按下 PTT 时打断 TTS/LLM。
// 旧版连续听写通过 CABIN_PTT_MODE=legacy 保持兼容。

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <clocale>
#include <cwctype>
#include <iostream>
#include <string>
#include <vector>

#include "CabinProtocol.h"
#include "PttController.h"
#include "ZmqClient.h"
#include "portaudio.h"  // NOLINT
#include "sherpa-onnx/csrc/display.h"
#include "sherpa-onnx/csrc/microphone.h"
#include "sherpa-onnx/csrc/online-recognizer.h"

bool stop = false;
float mic_sample_rate = 16000;
bool wait = false;
std::atomic<bool> accept_audio(true);

struct MicUserData {
    sherpa_onnx::OnlineStream *stream;
};

static int32_t RecordCallback(const void *input_buffer,
                              void * /*output_buffer*/,
                              unsigned long frames_per_buffer,  // NOLINT
                              const PaStreamCallbackTimeInfo * /*time_info*/,
                              PaStreamCallbackFlags /*status_flags*/,
                              void *user_data) {
    if (accept_audio.load() && !wait) {
        auto *ctx = reinterpret_cast<MicUserData *>(user_data);
        ctx->stream->AcceptWaveform(mic_sample_rate,
                                    reinterpret_cast<const float *>(input_buffer),
                                    frames_per_buffer);
    }

    return stop ? paComplete : paContinue;
}

static void Handler(int32_t /*sig*/) {
    stop = true;
    fprintf(stderr, "\nCaught Ctrl + C. Exiting...\n");
}

static std::string tolowerUnicode(const std::string &input_str) {
    std::setlocale(LC_ALL, "");

    std::wstring input_wstr(input_str.size() + 1, '\0');
    std::mbstowcs(&input_wstr[0], input_str.c_str(), input_str.size());
    std::wstring lowercase_wstr;

    for (wchar_t wc : input_wstr) {
        if (std::iswupper(wc)) {
            lowercase_wstr += std::towlower(wc);
        } else {
            lowercase_wstr += wc;
        }
    }

    std::string lowercase_str(input_str.size() + 1, '\0');
    std::wcstombs(&lowercase_str[0], lowercase_wstr.c_str(),
                  lowercase_wstr.size());

    return lowercase_str;
}

static void safe_interrupt(zmq_component::ZmqClient &client, const std::string &msg) {
    try {
        client.setTimeout(150);
        (void)client.request(msg);
    } catch (const std::exception &e) {
        std::cerr << "[voice] interrupt skipped: " << e.what() << std::endl;
    }
}

static std::string flush_asr(sherpa_onnx::OnlineRecognizer &recognizer,
                             sherpa_onnx::OnlineStream *stream,
                             const sherpa_onnx::OnlineRecognizerConfig &config) {
    std::vector<float> tail_paddings(static_cast<int>(0.8 * mic_sample_rate), 0.0f);
    stream->AcceptWaveform(mic_sample_rate, tail_paddings.data(), tail_paddings.size());
    while (recognizer.IsReady(stream)) {
        recognizer.DecodeStream(stream);
    }
    std::string text = recognizer.GetResult(stream).text;
    if (text.empty() && !config.model_config.paraformer.encoder.empty()) {
        std::vector<float> extra(static_cast<int>(1.0 * mic_sample_rate), 0.0f);
        stream->AcceptWaveform(mic_sample_rate, extra.data(), extra.size());
        while (recognizer.IsReady(stream)) {
            recognizer.DecodeStream(stream);
        }
        text = recognizer.GetResult(stream).text;
    }
    return text;
}

int32_t main(int32_t argc, char *argv[]) {
    signal(SIGINT, Handler);
    zmq_component::ZmqClient client;
    zmq_component::ZmqClient block_client("tcp://localhost:6677");
    zmq_component::ZmqClient interrupt_tts("tcp://localhost:6688");
    zmq_component::ZmqClient interrupt_llm("tcp://localhost:6667");

    cabin::PttController ptt;
    ptt.start();
    accept_audio.store(ptt.is_legacy_mode());

    const char *kUsageMessage = R"usage(
This program uses streaming models with microphone for speech recognition.
Usage:

  ./bin/sherpa-onnx-microphone \
    --tokens=/path/to/tokens.txt \
    --encoder=/path/to/encoder.onnx \
    --decoder=/path/to/decoder.onnx \
    --joiner=/path/to/joiner.onnx \
    --provider=cpu \
    --num-threads=1 \
    --decoding-method=greedy_search

PTT:
  按下开始流式识别，松开后把最终文本发给 LLM。
  CABIN_PTT_MODE=legacy 恢复旧版连续听写。
  CABIN_SESSION_ID / CABIN_ROLE 指定学员会话与角色。
)usage";

    sherpa_onnx::ParseOptions po(kUsageMessage);
    sherpa_onnx::OnlineRecognizerConfig config;

    config.Register(&po);
    po.Read(argc, argv);
    if (po.NumArgs() != 0) {
        po.PrintUsage();
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "%s\n", config.ToString().c_str());

    if (!config.Validate()) {
        fprintf(stderr, "Errors in config!\n");
        return -1;
    }

    sherpa_onnx::OnlineRecognizer recognizer(config);
    auto s = recognizer.CreateStream();
    MicUserData mic_data;
    mic_data.stream = s.get();

    sherpa_onnx::Microphone mic;

    PaDeviceIndex num_devices = Pa_GetDeviceCount();
    fprintf(stderr, "Num devices: %d\n", num_devices);

    int32_t device_index = Pa_GetDefaultInputDevice();

    if (device_index == paNoDevice) {
        fprintf(stderr, "No default input device found\n");
        fprintf(stderr, "If you are using Linux, please switch to \n");
        fprintf(stderr, " ./bin/sherpa-onnx-alsa \n");
        exit(EXIT_FAILURE);
    }

    const char *pDeviceIndex = std::getenv("SHERPA_ONNX_MIC_DEVICE");
    if (pDeviceIndex) {
        fprintf(stderr, "Use specified device: %s\n", pDeviceIndex);
        device_index = atoi(pDeviceIndex);
    }

    for (int32_t i = 0; i != num_devices; ++i) {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
        fprintf(stderr, " %s %d %s\n", (i == device_index) ? "*" : " ", i,
                info->name);
    }

    PaStreamParameters param;
    param.device = device_index;

    fprintf(stderr, "Use device: %d\n", param.device);

    const PaDeviceInfo *info = Pa_GetDeviceInfo(param.device);
    fprintf(stderr, "  Name: %s\n", info->name);
    fprintf(stderr, "  Max input channels: %d\n", info->maxInputChannels);

    param.channelCount = 1;
    param.sampleFormat = paFloat32;

    param.suggestedLatency = info->defaultLowInputLatency;
    param.hostApiSpecificStreamInfo = nullptr;
    const char *pSampleRateStr = std::getenv("SHERPA_ONNX_MIC_SAMPLE_RATE");
    if (pSampleRateStr) {
        fprintf(stderr, "Use sample rate %f for mic\n", mic_sample_rate);
        mic_sample_rate = atof(pSampleRateStr);
    }
    float sample_rate = 16000;

    PaStream *stream;
    PaError err =
        Pa_OpenStream(&stream, &param, nullptr, /* &outputParameters, */
                      sample_rate,
                      0,          // frames per buffer
                      paClipOff,  // we won't output out of range samples
                                  // so don't bother clipping them
                      RecordCallback, &mic_data);
    if (err != paNoError) {
        fprintf(stderr, "portaudio error: %s\n", Pa_GetErrorText(err));
        exit(EXIT_FAILURE);
    }

    err = Pa_StartStream(stream);
    fprintf(stderr, "Started 111111111\n");

    if (err != paNoError) {
        fprintf(stderr, "portaudio error: %s\n", Pa_GetErrorText(err));
        exit(EXIT_FAILURE);
    }

    std::string last_text;
    int32_t segment_index = 0;
    sherpa_onnx::Display display(30);
    const bool legacy = ptt.is_legacy_mode();

    while (!stop) {
        if (!legacy && ptt.consume_press_edge()) {
            wait = false;
            accept_audio.store(true);
            last_text.clear();
            recognizer.Reset(s.get());
            mic_data.stream = s.get();
            const std::string irq = cabin::CabinProtocol::encode(
                cabin::CABIN_MSG_TTS_INTERRUPT, ptt.session_id(), ptt.role(), "");
            const std::string abort_msg = cabin::CabinProtocol::encode(
                cabin::CABIN_MSG_LLM_ABORT, ptt.session_id(), ptt.role(), "");
            safe_interrupt(interrupt_tts, irq);
            safe_interrupt(interrupt_llm, abort_msg);
        }

        if (accept_audio.load()) {
            while (recognizer.IsReady(s.get())) {
                recognizer.DecodeStream(s.get());
            }
        }

        auto text = recognizer.GetResult(s.get()).text;
        bool is_endpoint = recognizer.IsEndpoint(s.get());

        if (is_endpoint && !config.model_config.paraformer.encoder.empty()) {
            std::vector<float> tail_paddings(static_cast<int>(1.0 * mic_sample_rate));
            s->AcceptWaveform(mic_sample_rate, tail_paddings.data(),
                              tail_paddings.size());
            while (recognizer.IsReady(s.get())) {
                recognizer.DecodeStream(s.get());
            }
            text = recognizer.GetResult(s.get()).text;
        }

        if (!text.empty() && last_text != text) {
            last_text = text;
            display.Print(segment_index, tolowerUnicode(text));
            fflush(stderr);
        }

        bool should_commit = false;
        if (legacy) {
            should_commit = is_endpoint && !text.empty();
        } else if (ptt.consume_release_edge()) {
            accept_audio.store(false);
            text = flush_asr(recognizer, s.get(), config);
            last_text = text;
            should_commit = !text.empty();
            if (text.empty()) {
                fprintf(stderr, "[PTT] empty asr, skip llm\n");
            } else {
                display.Print(segment_index, tolowerUnicode(text));
                fflush(stderr);
            }
        }

        if (should_commit) {
            const std::string req = cabin::CabinProtocol::encode(
                cabin::CABIN_MSG_ASR_TEXT, ptt.session_id(), ptt.role(), text);
            try {
                auto response = client.request(req);
                std::cout << "[llm -> voice] received: " << response << std::endl;
            } catch (const std::exception &e) {
                std::cerr << "[voice] llm request failed: " << e.what() << std::endl;
            }

            if (legacy) {
                wait = true;
                try {
                    auto block_response = block_client.request("block");
                    std::cout << "[tts -> voice] received: " << block_response << std::endl;
                } catch (const std::exception &e) {
                    std::cerr << "[voice] tts wait failed: " << e.what() << std::endl;
                }
                wait = false;
            }

            ++segment_index;
            recognizer.Reset(s.get());
            last_text.clear();
        } else if (legacy && is_endpoint) {
            recognizer.Reset(s.get());
        }

        Pa_Sleep(20);
    }

    ptt.stop();
    err = Pa_CloseStream(stream);
    if (err != paNoError) {
        fprintf(stderr, "portaudio error: %s\n", Pa_GetErrorText(err));
        exit(EXIT_FAILURE);
    }

    return 0;
}
