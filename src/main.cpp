#include <iostream>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <INIReader.h>
#include "grpc/GrpcServer.h"

// Phase 2: agents.ini에 등록된 (agent_id, api_key)만 연결을 허용한다.
// [server] listen_addr도 같은 파일에서 읽어 포트를 재빌드 없이 바꿀 수 있게 한다.
int main() {
    // 소스는 UTF-8(/utf-8)로 컴파일되지만 Windows 콘솔 기본 코드페이지는 CP949라
    // 로그에 한글이 섞이면 깨져 보인다. 콘솔 출력 코드페이지를 UTF-8로 맞춰 해결한다.
    SetConsoleOutputCP(CP_UTF8);

    const std::string agentsIniPath = "agents.ini";
    constexpr const char* kDefaultListenAddr = "0.0.0.0:50051";

    // ParseError(): 0 = 정상, 음수 = 파일 없음, 양수 N = N번째 줄 문법 오류.
    // inih는 틀린 줄만 건너뛰고 나머지는 읽으므로, 다른 줄의 오타 때문에 listen_addr를 버리지 않는다
    // (파일이 없거나 listen_addr 줄 자체가 틀렸을 때만 Get()이 기본값을 돌려준다).
    // 문법 오류 경고는 같은 파일을 읽는 AgentAuth::load()가 줄 번호와 함께 남긴다.
    INIReader reader(agentsIniPath);
    std::string listenAddr = reader.Get("server", "listen_addr", kDefaultListenAddr);

    return GrpcServer::run(listenAddr, agentsIniPath) ? 0 : 1;
}
