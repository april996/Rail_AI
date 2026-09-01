# V1版本的地铁驾驶教学模拟舱AI语音系统

## 项目概述
当前LLM_Voice_Flow已具备单用户语音闭环能力，现在需要拓展为地铁驾驶教学模拟舱的AI教员系统。V1目标：实现学员按下PTT，开始录音，学员松开PTT，停止录音。随后ASR识别，LLM处理，TTS播报。
实现细节：
1. 增加SessionManager，实现多学员会话隔离
2. 增加RoleRouter，支持Instructor / Dispatcher / Passenger三种角色Prompt
3. 增加PTT机制，实现按下录音、松开识别
4. 将ASR改造成Streaming模式（若已支持则复用）
5. 将LLM改造成Streaming输出
6. 将TTS改造成边生成边播报
7. 实现PTT打断TTS。
8. 对于AEC、WebRTC、RAG、知识库、评分系统、教员台、多GPU调度、微服务拆分、EventBus重构、数据库持久化这些内容只保留扩展点

## ref
https://github.com/superxiaobai-1/LLM_Voice_Flow.git

本项目开发了一套**全离线、模块化**的智能语音交互系统，基于RK3576 NPU实现端到端智能语音交互流水线，集成**流式ASR、DeepSeek大模型推理、TTS语音合成-双缓冲队列**三大核心模块。系统采用松耦合架构，各模块通过**标准化接口(封装ZeroMQ通信协议)**交互，在嵌入式环境下实现4秒内的**语音输入→LLM思考→语音输出闭环**。

## 核心特性

- 🚀 **全离线部署**：不依赖云端服务，基于RK3576 NPU实现本地化推理
- 🔧 **模块化架构**：ASR/TTS/LLM模块通过ZeroMQ松耦合通信
- ⚡ **低延迟优化**：流式ASR + 双缓冲TTS队列实现快速响应

