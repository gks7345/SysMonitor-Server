#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "sysmonitor.grpc.pb.h"
#include "AgentAuth.h"

// Phase 1: 받은 데이터를 로그로만 출력하는 최소 서비스 구현.
// Phase 2: 스트림을 읽기 전에 call metadata(agent_id/api_key)를 AgentAuth로 검증한다.
// Phase 5에서 AgentManager/DataStore 연동으로 교체 예정.
class GrpcServer final : public sysmonitor::AgentService::Service {
private:
    AgentAuth auth_;

    static void logReceived(const char* channel, const sysmonitor::AgentData& data);

    // context의 metadata에서 agent_id/api_key를 읽어 auth_로 검증한다.
    // 성공 시 검증된 agent_id를 verifiedAgentId에 채우고, 실패 시 UNAUTHENTICATED 상태와 사유를 반환한다.
    // 스트림은 이 값과 각 메시지의 data.agent_id()를 대조한다 — 신원의 기준은 항상 인증된 metadata다.
    grpc::Status authenticate(grpc::ServerContext* context, std::string* verifiedAgentId) const;

    // 스트림으로 받은 메시지 한 건을 검사한다: agent_id가 인증된 값과 같은지(PERMISSION_DENIED),
    // 스냅샷(oneof body)이 들어 있는지(INVALID_ARGUMENT). 실패하면 스트림을 끊을 상태를 반환한다.
    static grpc::Status validateMessage(const char* channel, const sysmonitor::AgentData& data, const std::string& verifiedAgentId);

    // --- 세션 표 (Phase 5 AgentManager의 첫 조각) ---
    // 같은 agent_id + api_key를 쓰는 두 PC(설치 폴더를 복사하고 id를 안 바꾼 경우)는 인증을 정상 통과하므로,
    // agent_id만으로는 "같은 PC의 재접속"과 "다른 PC의 중복"을 구분할 수 없다. metadata의 instance_id
    // (Agent: Windows MachineGuid)로 구분한다:
    //   - 같은 instance의 재접속               → 허용
    //   - 다른 instance인데 활성 스트림 있음     → ALREADY_EXISTS (데이터가 섞이지 않도록 거부)
    //   - 다른 instance인데 활성 스트림 없음     → 허용하고 새 instance로 교체 (PC 교체/재설치)
    // 스트림 핸들러는 gRPC 워커 스레드에서 동시에 실행되므로 sessions_는 sessionsMtx_로 보호한다
    // (auth_처럼 "시작 시 한 번 채우고 읽기만"이 아니다).
    struct Session {
        std::string instanceId;
        int activeStreams = 0;
    };
    std::mutex sessionsMtx_;
    std::unordered_map<std::string, Session> sessions_;

    // 위 규칙으로 판정한다. claim=true(스트림 진입)면 통과 시 이 instance로 바인딩하고 활성 스트림 수를 늘린다.
    // claim=false(Hello)면 판정만 한다.
    grpc::Status checkSession(const std::string& agentId, const std::string& instanceId, bool claim);
    // 스트림 종료 시 활성 스트림 수를 줄인다. 바인딩된 instance는 남겨 두어 다음 교체 로그에 쓴다.
    void releaseSession(const std::string& agentId);

    // 스트림 공통 진입 절차: 인증 → instance_id 확인 → 세션 claim. 성공하면 호출부가 반드시 releaseSession()해야 한다.
    grpc::Status beginStream(grpc::ServerContext* context, std::string* verifiedAgentId);

public:
    explicit GrpcServer(AgentAuth auth);

    // 연결 직후 Agent가 1회 호출하는 인증 + 프로토콜 버전 확인용 unary RPC. 성공 시 Ack.message에 "Hello <agent_id>"를 담는다.
    // 버전이 다르면(두 레포의 proto 불일치) FAILED_PRECONDITION, 다른 PC가 같은 agent_id로 접속 중이면 ALREADY_EXISTS를 반환한다.
    // 스트림 인증을 대체하지 않는다 — Hello를 건너뛰고 스트림을 바로 여는 클라이언트도 있을 수 있으므로
    // StreamRealtime/StreamBatch는 계속 각자 authenticate()를 호출한다.
    grpc::Status Hello(grpc::ServerContext* context, const sysmonitor::HelloRequest* request, sysmonitor::Ack* ack) override;

    grpc::Status StreamRealtime(grpc::ServerContext* context, grpc::ServerReader<sysmonitor::AgentData>* reader, sysmonitor::Ack* ack) override;
    grpc::Status StreamBatch(grpc::ServerContext* context, grpc::ServerReader<sysmonitor::AgentData>* reader, sysmonitor::Ack* ack) override;

    // host:port(예: "0.0.0.0:50051")에서 블로킹 방식으로 서버를 기동한다.
    // agentsIniPath에서 등록된 (agent_id, api_key) 목록을 로드한다.
    // 포트를 열지 못하면(잘못된 listen_addr, 포트 사용 중) 로그를 남기고 즉시 false를 반환한다.
    [[nodiscard]] static bool run(const std::string& listenAddr, const std::string& agentsIniPath = "agents.ini");
};
