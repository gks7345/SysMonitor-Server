#pragma once

#include <string>
#include <unordered_map>

// Phase 2: agents.ini에 등록된 (agent_id, api_key) 목록과 대조한다.
// mTLS 도입 전까지의 최소 신원 확인 (기획노트 §1).
class AgentAuth {
private:
    std::unordered_map<std::string, std::string> keysByAgentId_;

public:
    // iniPath가 없거나 파싱에 실패하면 등록된 Agent가 0개인 상태로 시작한다
    // (모든 연결이 거부됨 — 운영자가 agents.ini를 채워야 한다).
    static AgentAuth load(const std::string& iniPath = "agents.ini");

    // agent_id에 등록된 api_key와 일치하는지 확인한다.
    bool verify(const std::string& agentId, const std::string& apiKey) const;
};
