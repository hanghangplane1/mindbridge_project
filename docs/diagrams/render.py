#!/usr/bin/env python3
"""Render Mermaid diagrams to PNG using kroki.io API."""
import urllib.request
import os
import sys

OUTPUT_DIR = os.path.dirname(os.path.abspath(__file__))

diagrams = {
    "01_system_architecture": r"""graph TB
    subgraph Frontend["前端 Browser"]
        UI["聊天界面"]
        Cam["摄像头采集 JPEG Snapshot"]
        Mic["麦克风采集 Web Audio API"]
        Speaker["扬声器播放 PCM Stream"]
        ASR_FE["ASR 前处理 VAD + Resample 16kHz"]
    end

    subgraph Gateway["Gateway :8090"]
        GW_HTTP["HTTP JSON-RPC Router"]
        GW_ASR["/api/demo/asr"]
        GW_TTS["/api/demo/tts/stream"]
        GW_CHAT["/api/demo/chat"]
    end

    subgraph Orchestrator["Orchestrator :5009"]
        OR_Intent["意图/风险启发式分类"]
        OR_MM["多模态情绪分析 analyze_multimodal_emotion"]
        OR_Merge["风险合并 升级"]
        OR_Route["路由决策"]
    end

    subgraph Counselor["Counselor Agent :5010"]
        CL_Memory["ConversationMemory"]
        CL_Risk["assess_risk 工具"]
        CL_RAG["retrieve_knowledge ChromaDB"]
        CL_Prompt["PromptPolicy + 多模态上下文注入"]
        CL_Model["LLM 流式生成"]
        CL_Excel["mcp_excel_write"]
        CL_Email["mcp_email_alert"]
    end

    subgraph Evaluator["Evaluator Agent :5011"]
        EV_Analyze["深度风险评估"]
    end

    subgraph External["外部 AI 服务"]
        DashScope_ASR["DashScope ASR WebSocket"]
        DashScope_MM["DashScope Qwen-VL 多模态 API"]
        DashScope_TTS["DashScope CosyVoice WebSocket TTS"]
        MiMo_MM["MiMo 多模态 API 备选"]
        LLM["OpenAI 兼容 LLM"]
        ChromaDB["ChromaDB 向量库"]
    end

    UI -->|"文本 + 图像 + 音频"| GW_HTTP
    Cam -->|"JPEG base64"| UI
    Mic -->|"PCM/WAV"| ASR_FE
    ASR_FE -->|"base64 audio"| GW_ASR
    GW_ASR -->|"WebSocket"| DashScope_ASR
    DashScope_ASR -->|"transcript"| GW_ASR
    GW_ASR -->|"text 注入"| GW_HTTP

    GW_HTTP -->|"JSON-RPC + parts[]"| OR_Intent
    OR_Intent -->|"检测到非文本 parts"| OR_MM
    OR_MM -->|"text+image+audio"| DashScope_MM
    OR_MM -->|"text+image"| MiMo_MM
    OR_MM -->|"emotion result"| OR_Merge
    OR_Intent -->|"risk_level"| OR_Merge
    OR_Merge -->|"risk=high?"| OR_Route
    OR_Route -->|"high risk"| EV_Analyze
    OR_Route -->|"normal"| CL_Memory
    EV_Analyze -->|"评估结果"| CL_Memory

    CL_Memory --> CL_Risk
    CL_Risk --> CL_RAG
    CL_RAG --> ChromaDB
    CL_RAG --> CL_Prompt
    CL_Prompt -->|"system_prompt + multimodal_context"| CL_Model
    CL_Model -->|"stream"| LLM
    CL_Model -->|"SSE tokens"| OR_Route
    CL_Model -->|"tool: excel"| CL_Excel
    CL_Model -->|"tool: email"| CL_Email

    OR_Route -->|"SSE"| GW_HTTP
    GW_HTTP -->|"SSE"| UI
    GW_TTS -->|"WebSocket"| DashScope_TTS
    DashScope_TTS -->|"PCM chunks"| GW_TTS
    GW_TTS -->|"audio stream"| Speaker

    style Frontend fill:#e1f5fe,stroke:#0288d1
    style Gateway fill:#fff3e0,stroke:#f57c00
    style Orchestrator fill:#fce4ec,stroke:#c62828
    style Counselor fill:#e8f5e9,stroke:#2e7d32
    style Evaluator fill:#fff9c4,stroke:#f9a825
    style External fill:#f3e5f5,stroke:#7b1fa2""",

    "02_multimodal_fusion_flow": r"""flowchart TD
    Start(["用户发送消息"]) --> Input{"输入包含哪些模态?"}

    Input -->|"纯文本"| TextPath
    Input -->|"文本+图像"| TextImagePath
    Input -->|"文本+音频"| TextAudioPath
    Input -->|"文本+图像+音频"| FullMultiPath

    subgraph TextPath["文本路径"]
        T1["提取 text part"]
    end

    subgraph TextImagePath["文本+图像路径"]
        TI1["提取 text + image parts"]
        TI2["ASR: 无需 文本已有"]
    end

    subgraph TextAudioPath["文本+音频路径"]
        TA1["提取 text + audio parts"]
        TA2["Gateway 调用 ASR DashScope WebSocket"]
        TA3["transcript 注入为文本"]
    end

    subgraph FullMultiPath["全模态路径"]
        FM1["提取 text + image + audio parts"]
        FM2["Gateway 调用 ASR → transcript"]
    end

    T1 --> Merge["合并到 Orchestrator"]
    TI2 --> Merge
    TA3 --> Merge
    FM2 --> Merge

    Merge --> HasNonText{"有非文本 parts?"}

    HasNonText -->|No| Heuristic["启发式风险分类 纯文本"]
    HasNonText -->|Yes| MultiModal["调用 analyze_multimodal_emotion 工具"]

    MultiModal --> Provider{"多模态 Provider?"}

    Provider -->|"DashScope"| DS["Qwen-VL API text + image_url → JSON"]
    Provider -->|"MiMo"| MiMo["MiMo API text + image_url + input_audio → JSON"]

    DS --> Parse["解析结构化结果"]
    MiMo --> Parse

    Parse --> Weighted{"加权融合计算"}

    Weighted -->|"DashScope"| W1["weighted = text x 0.45 + visual x 0.55"]
    Weighted -->|"MiMo 三模态"| W2["weighted = text x 0.2 + visual x 0.4 + audio x 0.4"]
    Weighted -->|"MiMo 双模态"| W3["weighted = text x 0.35 + visual/audio x 0.65"]

    W1 --> Safety["安全兜底检查"]
    W2 --> Safety
    W3 --> Safety

    Safety --> RiskCheck{"高风险关键词? 或任一模态 high_risk?"}

    RiskCheck -->|Yes| ForceHigh["强制 risk=high weighted >= 4.0"]
    RiskCheck -->|No| KeepRisk["保持原 risk level"]

    ForceHigh --> MergeRisk["合并多模态风险 + 启发式风险"]
    KeepRisk --> MergeRisk
    Heuristic --> MergeRisk

    MergeRisk --> Route{"风险等级?"}

    Route -->|"high"| Evaluator["Evaluator Agent 深度风险评估"]
    Route -->|"medium/low"| Counselor["Counselor Agent"]

    Evaluator --> InjectCtx["注入多模态上下文到 Counselor system_prompt"]
    Counselor --> InjectCtx

    InjectCtx --> LLMGen["LLM 流式生成回复"]
    LLMGen --> TTSOut{"需要语音输出?"}
    TTSOut -->|Yes| TTS["TTS 合成 CosyVoice WebSocket"]
    TTSOut -->|No| TextOut["纯文本返回"]
    TTS --> SSE["SSE 流式返回前端"]
    TextOut --> SSE

    style MultiModal fill:#ff9800,color:#fff
    style Weighted fill:#e91e63,color:#fff
    style Safety fill:#f44336,color:#fff
    style ForceHigh fill:#b71c1c,color:#fff
    style Evaluator fill:#ff5722,color:#fff""",

    "03_sequence_diagram": r"""sequenceDiagram
    autonumber
    participant U as 用户
    participant FE as 前端
    participant GW as Gateway:8090
    participant ASR as DashScope ASR
    participant OR as Orchestrator:5009
    participant MM as 多模态API
    participant EV as Evaluator:5011
    participant CL as Counselor:5010
    participant LLM as LLM
    participant TTS as DashScope TTS

    rect rgb(225, 245, 254)
        Note over U,FE: 阶段1: 多模态输入采集
        U->>FE: 打字/说话 + 摄像头开启
        FE->>FE: 摄像头每5秒捕获JPEG快照
        FE->>FE: 麦克风持续录音+VAD检测
        FE->>FE: 音频重采样16kHz → WAV base64
        FE->>GW: POST parts: text image audio
    end

    rect rgb(255, 243, 224)
        Note over GW,ASR: 阶段2: Gateway 处理 和 ASR
        GW->>GW: 注入 request_id
        GW->>ASR: WebSocket: 音频流
        ASR-->>GW: transcript 文本
        GW->>OR: JSON-RPC message/stream
    end

    rect rgb(252, 228, 236)
        Note over OR,MM: 阶段3: 多模态融合分析
        OR->>OR: has_non_text_parts → true
        OR->>MM: analyze_multimodal_emotion
        MM-->>OR: text_emotion visual_emotion audio_emotion
        OR->>OR: 加权融合 + 安全兜底
    end

    rect rgb(255, 249, 196)
        Note over OR,EV: 阶段4: 风险路由
        alt risk == high
            OR->>EV: 深度风险评估
            EV->>LLM: 专业风险分析prompt
            LLM-->>EV: 风险评估报告
            EV-->>OR: 评估结果
        end
    end

    rect rgb(232, 245, 233)
        Note over CL,LLM: 阶段5: Counselor AgentLoop
        OR->>CL: 请求含multimodal metadata
        CL->>CL: memory_write + assess_risk + RAG
        CL->>CL: PromptPolicy + multimodal_context
        CL->>LLM: chat_stream
        loop 流式生成
            LLM-->>CL: token by token
            CL-->>OR: SSE token event
            OR-->>GW: SSE token event
            GW-->>FE: SSE token event
            FE-->>U: 逐字显示
        end
    end

    rect rgb(243, 229, 245)
        Note over FE,TTS: 阶段6: 语音输出
        FE->>GW: POST /tts/stream
        GW->>TTS: WebSocket: 文本→语音
        loop 流式播放
            TTS-->>GW: PCM chunks
            GW-->>FE: audio stream
            FE->>U: 实时播放语音
        end
    end""",

    "04_fusion_weights": r"""graph LR
    subgraph Inputs["多模态输入"]
        T["文本 Text"]
        I["图像 Image"]
        A["音频 Audio"]
    end

    subgraph Analysis["各模态独立分析"]
        TA["文本情绪分析 label + score"]
        IA["视觉情绪分析 label + score"]
        AA["音频情绪分析 label + score"]
    end

    subgraph DashScope_Fusion["DashScope 融合"]
        DS_W["加权公式"]
        DS_F["text x 0.45 + visual x 0.55"]
    end

    subgraph MiMo_Fusion["MiMo 融合"]
        MM_W3["三模态: text x 0.2 + visual x 0.4 + audio x 0.4"]
        MM_W2a["双模态图: text x 0.35 + visual x 0.65"]
        MM_W2b["双模态音: text x 0.35 + audio x 0.65"]
    end

    subgraph Safety["安全兜底"]
        S1{"高风险关键词?"}
        S2["强制 risk=high weighted >= 4.0"]
    end

    subgraph Output["融合输出"]
        Final["final_emotion risk_level weighted_score"]
    end

    T --> TA
    I --> IA
    A --> AA

    TA --> DS_W
    IA --> DS_W
    DS_W --> DS_F

    TA --> MM_W3
    IA --> MM_W3
    AA --> MM_W3
    TA --> MM_W2a
    IA --> MM_W2a
    TA --> MM_W2b
    AA --> MM_W2b

    DS_F --> S1
    MM_W3 --> S1
    MM_W2a --> S1
    MM_W2b --> S1

    S1 -->|Yes| S2
    S1 -->|No| Final
    S2 --> Final

    Final -->|"注入 system_prompt"| Counselor["Counselor LLM 生成更具共情力的回复"]

    style Inputs fill:#e3f2fd,stroke:#1565c0
    style Analysis fill:#fff8e1,stroke:#f9a825
    style DashScope_Fusion fill:#fce4ec,stroke:#c62828
    style MiMo_Fusion fill:#f3e5f5,stroke:#7b1fa2
    style Safety fill:#ffebee,stroke:#b71c1c
    style Output fill:#e8f5e9,stroke:#2e7d32"""
}


def render_diagram(name, code):
    """Render a mermaid diagram to PNG using kroki.io."""
    url = 'https://kroki.io/mermaid/png'
    payload = code.encode('utf-8')
    output_path = os.path.join(OUTPUT_DIR, f"{name}.png")

    print(f"Rendering {name}...", end=" ", flush=True)
    try:
        req = urllib.request.Request(url, data=payload, headers={
            'Content-Type': 'text/plain',
            'User-Agent': 'Mozilla/5.0'
        })
        with urllib.request.urlopen(req, timeout=60) as response:
            data = response.read()
            with open(output_path, 'wb') as f:
                f.write(data)
        print(f"OK ({len(data)//1024}KB) → {output_path}")
        return True
    except Exception as e:
        print(f"FAILED: {e}")
        return False


if __name__ == "__main__":
    success = 0
    for name, code in diagrams.items():
        if render_diagram(name, code):
            success += 1
    print(f"\nDone: {success}/{len(diagrams)} diagrams rendered successfully.")
    print(f"Output directory: {OUTPUT_DIR}")
