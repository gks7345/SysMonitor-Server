#include "AgentAuth.h"

#include <cctype>

#include <ini.h>
#include <spdlog/spdlog.h>

namespace {

    // 섹션 이름은 대소문자를 구분하지 않는다 — 같은 파일의 [server]를 읽는 INIReader(main.cpp)와 규칙을 맞춘다.
    // 서버는 Linux Docker 배포 예정(기획노트 §6)이라 _stricmp/strcasecmp 대신 이식 가능한 비교를 쓴다.
    bool equalsIgnoreCase(const char* a, const char* b) {
        for (; *a != '\0' && *b != '\0'; ++a, ++b) {
            if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) {
                return false;
            }
        }
        return *a == *b;
    }

    int handler(void* user, const char* section, const char* name, const char* value) {
        auto* keysByAgentId = static_cast<std::unordered_map<std::string, std::string>*>(user);
        // agent_id(name) 자체는 대소문자를 구분한다 — Agent가 metadata로 보내는 값과 정확히 일치해야 하는 식별자다.
        if (equalsIgnoreCase(section, "agents") && name != nullptr && value != nullptr && value[0] != '\0') {
            if (keysByAgentId->count(name) != 0) {
                spdlog::warn("[AgentAuth] agent '{}' appears more than once in [agents] — the later entry wins", name);
            }
            (*keysByAgentId)[name] = value;
        }
        return 1; // non-zero = success (inih convention)
    }

    // 첫 불일치에서 조기 종료하지 않아 비교 시간이 일치한 바이트 수에 좌우되지 않는다
    // (api_key 타이밍 사이드채널 방지).
    bool constantTimeEquals(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        unsigned char diff = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
        }
        return diff == 0;
    }

} // namespace

AgentAuth AgentAuth::load(const std::string& iniPath) {
    AgentAuth auth;

    // ini_parse(): 0 = 정상, 음수 = 파일 없음/메모리 오류, 양수 N = N번째 줄 문법 오류.
    // 문법 오류 줄은 inih가 건너뛰고 나머지 줄은 계속 읽으므로, 틀린 줄의 Agent만 빠지고 나머지는 정상 등록된다.
    int rc = ini_parse(iniPath.c_str(), handler, &auth.keysByAgentId_);
    if (rc < 0) {
        spdlog::warn("[AgentAuth] {} not found or invalid — no agents registered, all connections will be rejected", iniPath);
        return auth;
    }
    if (rc > 0) {
        spdlog::warn("[AgentAuth] {} line {}: syntax error (expected 'key = value') — line skipped; if it was an agent entry, that agent is NOT registered (only the first error line is reported)", iniPath, rc);
    }

    spdlog::info("[AgentAuth] loaded {} ({} agent(s) registered)", iniPath, auth.keysByAgentId_.size());
    return auth;
}

bool AgentAuth::verify(const std::string& agentId, const std::string& apiKey) const {
    auto it = keysByAgentId_.find(agentId);
    if (it == keysByAgentId_.end()) {
        return false;
    }
    return constantTimeEquals(it->second, apiKey);
}
