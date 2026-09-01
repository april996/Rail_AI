// Copyright (c) 2024 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// 修改原因：复用 rkllm 流式 callback 与 ZMQ 7777 推送 TTS 的现有路径，
// 增加 SessionManager 多学员隔离、RoleRouter 三角色 Prompt、PTT 中止 rkllm_abort。
// 旧版纯文本请求仍按 default/instructor 处理。

#include <string.h>
#include <unistd.h>
#include <string>
#include "rkllm.h"
#include <fstream>
#include <iostream>
#include <csignal>
#include <vector>
#include <set>
#include <atomic>
#include <mutex>
#include <thread>
#include "ZmqServer.h"
#include "ZmqClient.h"
#include "CabinProtocol.h"
#include "CabinExtensions.h"
#include "RoleRouter.h"
#include "SessionManager.h"
#include <cwchar>
#include <locale>
#include <clocale>
#include <cstdlib>
#include <codecvt>

using namespace std;
LLMHandle llmHandle = nullptr;

zmq_component::ZmqServer server;
zmq_component::ZmqClient client("tcp://localhost:7777");

std::wstring buffer_;
std::string g_session_id = cabin::CabinProtocol::kDefaultSession;
std::string g_role = cabin::CabinProtocol::kDefaultRole;
std::string g_assistant_acc;
std::atomic<bool> g_aborting(false);
std::mutex g_infer_mutex;

static const std::set<wchar_t> split_chars = {
    L'：',
    L'，',
    L'。',
    L'\n',
    L'；',
    L'！',
    L'？'
};

bool is_valid_utf8_continuation(uint8_t c)
{
    return (c & 0xC0) == 0x80;
}

std::wstring extract_after_think(const std::wstring &input)
{
    const std::wstring start_tag = L"<think>";
    const std::wstring end_tag = L"</think>";

    size_t start_pos = input.find(start_tag);
    size_t end_pos = input.find(end_tag);

    std::wstring result;

    if (start_pos != std::wstring::npos && end_pos != std::wstring::npos && end_pos > start_pos)
    {
        result = input.substr(start_pos + start_tag.length(), end_pos - start_pos - start_tag.length());
    }
    else if (end_pos != std::wstring::npos)
    {
        result = input.substr(end_pos + end_tag.length());
    }
    else
    {
        result = input;
    }

    const std::wstring punct = L" \t\n\r*#@$%^&，。：、；！？【】（）“”‘’";

    std::wstring filtered;
    for (wchar_t c : result)
    {
        if (punct.find(c) == std::wstring::npos)
        {
            filtered += c;
        }
    }

    return filtered;
}

std::wstring utf8_to_wstring(const std::string &str)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.from_bytes(str);
}

std::string wstring_to_utf8(const std::wstring &str)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.to_bytes(str);
}

void exit_handler(int signal)
{
    if (llmHandle != nullptr)
    {
        {
            cout << "程序即将退出" << endl;
            LLMHandle _tmp = llmHandle;
            llmHandle = nullptr;
            rkllm_destroy(_tmp);
        }
    }
    exit(signal);
}

static void send_tts(cabin::CabinMsgType type, const std::string &text)
{
    const std::string wire = cabin::CabinProtocol::encode(type, g_session_id, g_role, text);
    try
    {
        auto response = client.request(wire);
        std::cout << "[tts -> llm] received : " << response << std::endl;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[llm] tts send failed: " << e.what() << std::endl;
    }
}

void send_response(const std::wstring &text)
{
    std::string response_str = wstring_to_utf8(extract_after_think(text));
    if (response_str.empty())
    {
        return;
    }
    g_assistant_acc += response_str;
    send_tts(cabin::CABIN_MSG_TTS_TEXT, response_str);
}

void callback(RKLLMResult *result, void *userdata, LLMCallState state)
{
    (void)userdata;

    if (g_aborting.load() && state == RKLLM_RUN_NORMAL)
    {
        return;
    }

    if (state == RKLLM_RUN_FINISH)
    {
        if (!g_aborting.load() && !buffer_.empty())
        {
            std::string response_str = wstring_to_utf8(extract_after_think(buffer_));
            if (!response_str.empty())
            {
                g_assistant_acc += response_str;
                send_tts(cabin::CABIN_MSG_TTS_END, response_str + " END");
            }
            else
            {
                send_tts(cabin::CABIN_MSG_TTS_END, "END");
            }
            buffer_.clear();
        }
        else
        {
            send_tts(cabin::CABIN_MSG_TTS_END, "END");
            buffer_.clear();
        }

        if (!g_assistant_acc.empty())
        {
            cabin::SessionManager::instance().append_assistant(g_session_id, g_assistant_acc);
            cabin_ext::instructor_console_publish("llm.finish", g_session_id);
        }
        cabin::SessionManager::instance().set_state(g_session_id, cabin::SESSION_SPEAKING);
        printf("\n");
    }
    else if (state == RKLLM_RUN_ERROR)
    {
        printf("\\run error\n");
        send_tts(cabin::CABIN_MSG_TTS_END, "END");
        buffer_.clear();
    }
    else if (state == RKLLM_RUN_NORMAL)
    {
        if (result->last_hidden_layer.embd_size != 0 && result->last_hidden_layer.num_tokens != 0)
        {
            int data_size = result->last_hidden_layer.embd_size * result->last_hidden_layer.num_tokens * sizeof(float);
            printf("\ndata_size:%d", data_size);
            std::ofstream outFile("last_hidden_layer.bin", std::ios::binary);
            if (outFile.is_open())
            {
                outFile.write(reinterpret_cast<const char *>(result->last_hidden_layer.hidden_states), data_size);
                outFile.close();
                std::cout << "Data saved to output.bin successfully!" << std::endl;
            }
            else
            {
                std::cerr << "Failed to open the file for writing!" << std::endl;
            }
        }

        printf("%s", result->text);

        std::wstring wide_text = utf8_to_wstring(result->text);

        for (wchar_t c : wide_text)
        {
            buffer_ += c;

            if (split_chars.count(c))
            {
                if (!buffer_.empty())
                {
                    send_response(buffer_);
                    buffer_.clear();
                }
            }
        }
    }
}

void Init(const string &model_path)
{
    RKLLMParam param = rkllm_createDefaultParam();
    param.model_path = model_path.c_str();

    param.top_k = 1;
    param.top_p = 0.95;
    param.temperature = 0.8;
    param.repeat_penalty = 1.1;
    param.frequency_penalty = 0.0;
    param.presence_penalty = 0.0;

    param.max_new_tokens = 100;
    param.max_context_len = 256;
    param.skip_special_token = true;
    param.extend_param.base_domain_id = 0;
    param.extend_param.embed_flash = 1;
    param.extend_param.enabled_cpus_num = 2;
    param.extend_param.enabled_cpus_mask = CPU0 | CPU2;

    int ret = rkllm_init(&llmHandle, &param, callback);
    if (ret == 0)
    {
        printf("rkllm init success\n");
    }
    else
    {
        printf("rkllm init failed\n");
        exit_handler(-1);
    }
}

static void apply_role_template(const std::string &role)
{
    const cabin::CabinRole r = cabin::RoleRouter::parse(role);
    const std::string sys = cabin::RoleRouter::system_prompt(r);
    rkllm_set_chat_template(llmHandle, sys.c_str(), "<｜User｜>", "<｜Assistant｜>");
}

void interrupt_worker()
{
    zmq_component::ZmqServer abort_server("tcp://*:6667");
    while (true)
    {
        try
        {
            std::string req = abort_server.receive();
            cabin::CabinMessage msg = cabin::CabinProtocol::decode(req);
            std::cout << "[voice -> llm] abort: " << req << std::endl;
            g_aborting.store(true);
            cabin::SessionManager::instance().request_interrupt(
                msg.session_id.empty() ? g_session_id : msg.session_id);
            if (llmHandle != nullptr)
            {
                rkllm_abort(llmHandle);
            }
            abort_server.send("ACK");
        }
        catch (const std::exception &e)
        {
            std::cerr << "[llm] abort server: " << e.what() << std::endl;
        }
    }
}

void receive_asr_data_and_process()
{
    RKLLMInferParam rkllm_infer_params;
    memset(&rkllm_infer_params, 0, sizeof(RKLLMInferParam));

    rkllm_infer_params.mode = RKLLM_INFER_GENERATE;
    rkllm_infer_params.keep_history = 0;

    apply_role_template(g_role);

    RKLLMInput rkllm_input;
    std::string last_session;

    while (true)
    {
        std::string input_str = server.receive();
        std::cout << "[voice -> llm] received: " << input_str << std::endl;
        server.send("llm sucess reply !!!");

        cabin::CabinMessage msg = cabin::CabinProtocol::decode(input_str);
        if (msg.type == cabin::CABIN_MSG_LLM_ABORT || msg.type == cabin::CABIN_MSG_TTS_INTERRUPT)
        {
            g_aborting.store(true);
            if (llmHandle != nullptr)
            {
                rkllm_abort(llmHandle);
            }
            continue;
        }

        const std::string user_text = msg.payload.empty() ? input_str : msg.payload;
        if (user_text.empty())
        {
            continue;
        }

        g_session_id = msg.session_id;
        g_role = cabin::RoleRouter::normalize(msg.role);
        cabin::SessionManager::instance().set_role(g_session_id, g_role);
        cabin_ext::multi_gpu_schedule(g_session_id);

        std::string prompt = cabin::SessionManager::instance().build_prompt(g_session_id, user_text);
        cabin::SessionManager::instance().append_user(g_session_id, user_text);
        apply_role_template(g_role);

        if (!last_session.empty() && last_session != g_session_id)
        {
            rkllm_clear_kv_cache(llmHandle, 0);
        }
        last_session = g_session_id;

        g_aborting.store(false);
        buffer_.clear();
        g_assistant_acc.clear();

        rkllm_input.input_type = RKLLM_INPUT_PROMPT;
        rkllm_input.prompt_input = const_cast<char *>(prompt.c_str());

        std::lock_guard<std::mutex> lock(g_infer_mutex);
        rkllm_run(llmHandle, &rkllm_input, &rkllm_infer_params, NULL);
    }
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "en_US.UTF-8");

    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " model_path\n";
        return 1;
    }

    signal(SIGINT, exit_handler);
    printf("rkllm init start\n");

    Init(argv[1]);

    std::thread abort_thread(interrupt_worker);
    abort_thread.detach();

    receive_asr_data_and_process();

    rkllm_destroy(llmHandle);

    return 0;
}
