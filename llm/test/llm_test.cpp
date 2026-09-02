// 修改原因：离线联调桩保持与 llm_demo 相同的 CABIN1 信封与分句推送 TTS，
// 便于无 RK 板时验证 Session/Role/PTT 协议，旧纯文本仍可用。

#pragma once

#include <string.h>
#include <unistd.h>
#include <string>
#include <fstream>
#include <iostream>
#include <csignal>
#include <vector>
#include <set>
#include "ZmqServer.h"
#include "ZmqClient.h"

#新增的三个头文件
#include "CabinProtocol.h"
#include "RoleRouter.h"
#include "SessionManager.h"

#include <cwchar>
#include <locale>
#include <clocale>
#include <cstdlib>
#include <codecvt>
#include <regex>
#include <cwchar>
#include <cstdlib>
#include <codecvt>

using namespace std;

zmq_component::ZmqServer server;
zmq_component::ZmqClient tts_client_("tcp://localhost:7777");

void exit_handler(int signal)
{
    exit(signal);
}
#新增session_id,role
void message_worker(const std::string &session_id,
                    const std::string &role,
                    const std::string &rag_text)
{
    static const std::wregex wide_delimiter(
        L"([。！？；：\n]|\\?\\s|\\!\\s|\\；|\\，|\\、|\\|)");
    const std::wstring END_MARKER = L"END";

    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;

    std::wstring wide_text = converter.from_bytes(rag_text) + END_MARKER;

    std::wsregex_iterator it(wide_text.begin(), wide_text.end(), wide_delimiter);
    std::wsregex_iterator end;

    int skip_counter = 0;
    size_t last_pos = 0;
    while (it != end && skip_counter < 2)
    {
        last_pos = it->position() + it->length();
        ++it;
        ++skip_counter;
    }

    while (it != end)
    {
        size_t seg_start = last_pos;
        size_t seg_end = it->position();
        last_pos = seg_end + it->length();

        std::wstring wide_segment = wide_text.substr(seg_start, seg_end - seg_start);

        wide_segment.erase(0, wide_segment.find_first_not_of(L" \t\n\r"));
        wide_segment.erase(wide_segment.find_last_not_of(L" \t\n\r") + 1);

        if (!wide_segment.empty())
        {
            #修改后
            const std::string payload = converter.to_bytes(wide_segment);
            auto response1 = tts_client_.request(
                cabin::CabinProtocol::encode(cabin::CABIN_MSG_TTS_TEXT, session_id, role, payload));
            
            std::cout << "[tts -> llm] received: " << response1 << std::endl;
        }
        ++it;
    }

    if (last_pos < wide_text.length())
    {
        std::wstring last_segment = wide_text.substr(last_pos);
        if (!last_segment.empty())
        {
            #修改后
            const std::string payload = converter.to_bytes(last_segment);
            auto response1 = tts_client_.request(
                cabin::CabinProtocol::encode(cabin::CABIN_MSG_TTS_END, session_id, role, payload));
            
            std::cout << "[tts -> llm] received: " << response1 << std::endl;
        }
    }
}

void receive_asr_data_and_process()
{
    while (true)
    {
        std::string input_str = server.receive();
        std::cout << "[voice -> llm] received: " << input_str << std::endl;
        server.send("llm sucess reply !!!");

        #新处理逻辑，待优化
        cabin::CabinMessage msg = cabin::CabinProtocol::decode(input_str);
        const std::string user_text = msg.payload.empty() ? input_str : msg.payload;
        if (user_text.empty())
        {
            continue;
        }
        const std::string role = cabin::RoleRouter::normalize(msg.role);
        cabin::SessionManager::instance().set_role(msg.session_id, role);
        const std::string prompt = cabin::SessionManager::instance().build_prompt(msg.session_id, user_text);
        cabin::SessionManager::instance().append_user(msg.session_id, user_text);
        cabin::SessionManager::instance().append_assistant(msg.session_id, user_text);
        message_worker(msg.session_id, role, prompt);
    }
}

int main(int argc, char **argv)
{
    (void)argc;#新
    (void)argv;#新
    setlocale(LC_ALL, "en_US.UTF-8");

    signal(SIGINT, exit_handler);
    printf("rkllm init start\n");

    receive_asr_data_and_process();

    return 0;
}
