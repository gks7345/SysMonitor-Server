#include "grpc/GrpcServer.h"

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

namespace {

// ServerContext::client_metadata()에서 key에 해당하는 값을 꺼낸다. 없으면 빈 문자열.
std::string metadataValue(const grpc::ServerContext* context, const char* key) {
    auto& metadata = context->client_metadata();
    auto it = metadata.find(key);
    if (it == metadata.end()) {
        return "";
    }
    return std::string(it->second.data(), it->second.size());
}

// 스코프를 벗어날 때(정상 종료 / 오류 반환 / 예외) 반드시 한 번 실행한다 — 세션의 활성 스트림 수가
// 어떤 경로로 핸들러가 끝나든 줄어들도록 하기 위함.
template <typename F>
class ScopeExit {
    F fn_;
public:
    explicit ScopeExit(F fn) : fn_(std::move(fn)) {}
    ~ScopeExit() { fn_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
};

// metadata의 instance_id를 읽는다. 비어 있으면 세션을 구분할 수 없으므로 거부한다 (fail-closed).
grpc::Status readInstanceId(const grpc::ServerContext* context, const std::string& agentId, std::string* instanceId) {
    *instanceId = metadataValue(context, "instance_id");
    if (instanceId->empty()) {
        spdlog::warn("[GrpcServer] rejected agent={}: missing instance_id metadata", agentId);
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "missing instance_id metadata");
    }
    return grpc::Status::OK;
}

} // namespace

GrpcServer::GrpcServer(AgentAuth auth) : auth_(std::move(auth)) {}

void GrpcServer::logReceived(const char* channel, const sysmonitor::AgentData& data) {
    // Phase 5에서 DataStore 저장으로 교체 예정 — 지금은 스냅샷 종류별 요약만 남긴다.
    switch (data.body_case()) {
    case sysmonitor::AgentData::kSys:
        spdlog::info("[{}] agent={} sys ts_us={} cpu={:.1f}% mem={:.1f}%", channel, data.agent_id(), data.sys().timestamp_us(), data.sys().cpu_total(), data.sys().mem_usage_percent());
        break;
    case sysmonitor::AgentData::kProc:
        spdlog::info("[{}] agent={} proc ts_us={} procs={}", channel, data.agent_id(), data.proc().timestamp_us(), data.proc().procs_size());
        break;
    case sysmonitor::AgentData::kTarget:
        spdlog::info("[{}] agent={} target ts_us={} targets={}", channel, data.agent_id(), data.target().timestamp_us(), data.target().targets_size());
        break;
    case sysmonitor::AgentData::BODY_NOT_SET:
        break;   // validateMessage()가 이미 INVALID_ARGUMENT로 거부하므로 도달하지 않음
    }
}

grpc::Status GrpcServer::authenticate(grpc::ServerContext* context, std::string* verifiedAgentId) const {
    std::string agentId = metadataValue(context, "agent_id");
    std::string apiKey = metadataValue(context, "api_key");

    if (!auth_.verify(agentId, apiKey)) {
        spdlog::warn("[GrpcServer] rejected connection: agent_id={} (invalid or unregistered api_key)", agentId);
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "invalid agent_id/api_key");
    }
    *verifiedAgentId = std::move(agentId);
    return grpc::Status::OK;
}

grpc::Status GrpcServer::validateMessage(const char* channel, const sysmonitor::AgentData& data, const std::string& verifiedAgentId) {
    // 인증은 연결 단위라, 메시지 본문의 agent_id는 따로 대조해야 한다.
    // (대조하지 않으면 한 Agent의 키로 인증한 뒤 다른 Agent를 사칭한 데이터를 보낼 수 있다.)
    if (data.agent_id() != verifiedAgentId) {
        spdlog::warn("[GrpcServer] {} agent_id mismatch: authenticated={} payload={} — closing stream", channel, verifiedAgentId, data.agent_id());
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "payload agent_id does not match authenticated agent_id");
    }
    // 스냅샷 없이 온 메시지는 저장할 수 없다 — 조용히 건너뛰지 않고 거부해 Agent 쪽 버그를 드러낸다.
    if (data.body_case() == sysmonitor::AgentData::BODY_NOT_SET) {
        spdlog::warn("[GrpcServer] {} empty message from agent={} (no snapshot) — closing stream", channel, verifiedAgentId);
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "AgentData has no snapshot (body not set)");
    }
    return grpc::Status::OK;
}

grpc::Status GrpcServer::checkSession(const std::string& agentId, const std::string& instanceId, bool claim) {
    std::lock_guard<std::mutex> lock(sessionsMtx_);

    auto it = sessions_.find(agentId);
    if (it != sessions_.end() && it->second.activeStreams > 0 && it->second.instanceId != instanceId) {
        spdlog::warn("[GrpcServer] rejected agent={} from instance {}: already connected from instance {} — two machines are using the same agent_id (check agent.ini id)", agentId, instanceId, it->second.instanceId);
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS,
                            "agent_id " + agentId + " is already in use by another machine — give this machine its own id in agent.ini");
    }
    if (!claim) {
        return grpc::Status::OK;
    }

    Session& session = sessions_[agentId];
    if (session.instanceId != instanceId) {
        if (session.instanceId.empty()) {
            spdlog::info("[GrpcServer] agent={} bound to instance {}", agentId, instanceId);
        } else {
            // 활성 스트림이 없는 상태에서 다른 instance가 왔다 — PC 교체/재설치로 본다.
            spdlog::info("[GrpcServer] agent={} rebound to instance {} (was {})", agentId, instanceId, session.instanceId);
        }
        session.instanceId = instanceId;
    }
    ++session.activeStreams;
    return grpc::Status::OK;
}

void GrpcServer::releaseSession(const std::string& agentId) {
    std::lock_guard<std::mutex> lock(sessionsMtx_);
    auto it = sessions_.find(agentId);
    if (it != sessions_.end() && it->second.activeStreams > 0) {
        --it->second.activeStreams;
        if (it->second.activeStreams == 0) {
            // 이 시점부터 다른 instance가 이 agent_id로 접속할 수 있다 (정상 종료, 오류, keepalive 끊김 감지 모두 여기로 온다).
            spdlog::info("[GrpcServer] agent={} has no active streams (instance {})", agentId, it->second.instanceId);
        }
    }
}

grpc::Status GrpcServer::beginStream(grpc::ServerContext* context, std::string* verifiedAgentId) {
    grpc::Status authStatus = authenticate(context, verifiedAgentId);
    if (!authStatus.ok()) {
        return authStatus;
    }
    std::string instanceId;
    grpc::Status idStatus = readInstanceId(context, *verifiedAgentId, &instanceId);
    if (!idStatus.ok()) {
        return idStatus;
    }
    return checkSession(*verifiedAgentId, instanceId, /*claim=*/true);
}

grpc::Status GrpcServer::Hello(grpc::ServerContext* context, const sysmonitor::HelloRequest* request, sysmonitor::Ack* ack) {
    std::string agentId;
    grpc::Status authStatus = authenticate(context, &agentId);
    if (!authStatus.ok()) {
        return authStatus;
    }

    // 두 레포의 proto가 어긋나 있으면 protobuf는 모르는 필드를 조용히 무시하므로, 에러 없이 빈 데이터가
    // 오가게 된다. 연결 시점에 버전을 대조해 명확한 오류(Agent: Fatal)로 드러낸다.
    if (request->protocol_version() != static_cast<uint32_t>(sysmonitor::PROTOCOL_VERSION_CURRENT)) {
        spdlog::warn("[GrpcServer] rejected agent={}: protocol version mismatch (agent={}, server={})", agentId, request->protocol_version(), static_cast<int>(sysmonitor::PROTOCOL_VERSION_CURRENT));
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "protocol version mismatch: agent=" + std::to_string(request->protocol_version()) + ", server=" + std::to_string(sysmonitor::PROTOCOL_VERSION_CURRENT) + " (sync sysmonitor.proto)");
    }

    // 다른 PC가 같은 agent_id로 이미 스트림을 열고 있으면, 스트림을 열기 전에 여기서 원인과 함께 거부한다.
    // (판정만 하고 바인딩은 스트림 진입 시 — Hello만 하고 끝나는 연결이 세션을 차지하지 않도록)
    std::string instanceId;
    grpc::Status idStatus = readInstanceId(context, agentId, &instanceId);
    if (!idStatus.ok()) {
        return idStatus;
    }
    grpc::Status sessionStatus = checkSession(agentId, instanceId, /*claim=*/false);
    if (!sessionStatus.ok()) {
        return sessionStatus;
    }

    spdlog::info("[GrpcServer] hello from agent={}", agentId);
    ack->set_success(true);
    ack->set_message("Hello " + agentId);
    return grpc::Status::OK;
}

grpc::Status GrpcServer::StreamRealtime(grpc::ServerContext* context, grpc::ServerReader<sysmonitor::AgentData>* reader, sysmonitor::Ack* ack) {
    std::string agentId;
    grpc::Status beginStatus = beginStream(context, &agentId);
    if (!beginStatus.ok()) {
        return beginStatus;
    }
    ScopeExit release([&] { releaseSession(agentId); });

    sysmonitor::AgentData data;
    while (reader->Read(&data)) {
        grpc::Status msgStatus = validateMessage("realtime", data, agentId);
        if (!msgStatus.ok()) {
            return msgStatus;
        }
        logReceived("realtime", data);
    }
    ack->set_success(true);
    ack->set_message("ok");
    return grpc::Status::OK;
}

grpc::Status GrpcServer::StreamBatch(grpc::ServerContext* context, grpc::ServerReader<sysmonitor::AgentData>* reader, sysmonitor::Ack* ack) {
    std::string agentId;
    grpc::Status beginStatus = beginStream(context, &agentId);
    if (!beginStatus.ok()) {
        return beginStatus;
    }
    ScopeExit release([&] { releaseSession(agentId); });

    sysmonitor::AgentData data;
    while (reader->Read(&data)) {
        grpc::Status msgStatus = validateMessage("batch", data, agentId);
        if (!msgStatus.ok()) {
            return msgStatus;
        }
        logReceived("batch", data);
    }
    ack->set_success(true);
    ack->set_message("ok");
    return grpc::Status::OK;
}

bool GrpcServer::run(const std::string& listenAddr, const std::string& agentsIniPath) {
    GrpcServer service(AgentAuth::load(agentsIniPath));

    grpc::ServerBuilder builder;
    builder.AddListeningPort(listenAddr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    // 스트림이 Agent → 서버 단방향이라 서버는 스트림이 끝날 때까지 아무것도 보내지 않는다 — TCP 재전송으로
    // 끊김을 알 기회가 없어, 정전/랜선 뽑힘처럼 FIN/RST 없이 사라진 Agent는 keepalive ping으로만 감지된다.
    // gRPC 서버 기본값은 2시간이라, 그동안 죽은 연결이 세션 표에 "활성"으로 남아 교체한 PC를 ALREADY_EXISTS로
    // 막는다 (6차 코드 리뷰 Medium #1). 약 40초 안에 감지하면 Read()가 false를 반환하고 ScopeExit가 세션을 푼다.
    // - KEEPALIVE_TIMEOUT만으로는 부족하다: 이 gRPC 버전(1.66)은 keepalive를 켜면 ping 응답 대기(PING_TIMEOUT)의
    //   기본값이 1분이고, 실측(패킷을 막는 중계기)에서 감지가 30초 + 60초 = 약 90초 걸렸다. 그래서 둘 다 10초로 맞춘다.
    // - GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA는 설정하지 않는다 — 이 버전은 서버 쪽 값을 0(무제한)으로 고정한다.
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 30 * 1000);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10 * 1000);
    // ping 응답 대기 인자는 공개 헤더에 없다 (grpc-src/src/core/ext/transport/chttp2/transport/internal.h의
    // GRPC_ARG_PING_TIMEOUT_MS). 내부 헤더를 include하지 않고 키 문자열을 직접 쓴다 — gRPC를 올리면 이 키가
    // 그대로인지, 패킷을 막는 중계기로 감지 시간이 여전히 약 40초인지 다시 확인할 것.
    builder.AddChannelArgument("grpc.http2.ping_timeout_ms", 10 * 1000);

    // 등록한 포트 중 하나라도 바인딩에 실패하면 BuildAndStart()는 nullptr를 반환한다.
    // 구체적인 원인(예: 주소 형식 오류, WSAEADDRINUSE)은 gRPC 코어가 stderr에 직접 출력한다.
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        spdlog::error("[GrpcServer] failed to listen on {} (invalid listen_addr in agents.ini, or port already in use)", listenAddr);
        return false;
    }

    spdlog::info("[GrpcServer] listening on {}", listenAddr);
    server->Wait();
    return true;
}
