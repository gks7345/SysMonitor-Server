#include "grpc/GrpcServer.h"

#include <algorithm>

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

namespace {

// ServerContext::client_metadata()에서 key에 해당하는 값을 꺼낸다. 없으면 빈 문자열.
std::string metadataValue(const grpc::ServerContext* context, const char* key) {
    auto& metadata = context->client_metadata();        // 클라이언트가 보낸 metadata 전체 (multimap)
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

// 스냅샷의 timestamp. 스냅샷이 없는 메시지는 validateMessage()가 먼저 거부하므로 BODY_NOT_SET에는 오지 않는다.
int64_t snapshotTimestampUs(const sysmonitor::AgentData& data) {
    switch (data.body_case()) {
    case sysmonitor::AgentData::kSys:
        return data.sys().timestamp_us();
    case sysmonitor::AgentData::kProc:
        return data.proc().timestamp_us();
    case sysmonitor::AgentData::kTarget:
        return data.target().timestamp_us();
	case sysmonitor::AgentData::BODY_NOT_SET:   // validateMessage()가 먼저 거부하므로 여기로 오지 않는다
        break;
    }
    return 0;
}

// 갭 스트림 하나의 요약 — 메시지마다 로그를 남기면 하루치 갭(약 26만 건)이 로그를 덮으므로 스트림이 끝날 때 한 줄로 남긴다.
struct BatchSummary {
    int sys = 0;
    int proc = 0;
    int target = 0;
    int64_t firstTs = 0;
    int64_t lastTs = 0;
    std::string collectedBy;   // 묶음 안의 수집 장비 (서로 다르면 "mixed")

    void add(const sysmonitor::AgentData& data) {
        switch (data.body_case()) {
        case sysmonitor::AgentData::kSys: 
            ++sys; 
            break;
        case sysmonitor::AgentData::kProc: 
            ++proc; 
            break;
        case sysmonitor::AgentData::kTarget: 
            ++target; 
            break;
        case sysmonitor::AgentData::BODY_NOT_SET: 
            break;
        }
        const int64_t ts = snapshotTimestampUs(data);
        if (sys + proc + target == 1) {
            firstTs = lastTs = ts;
            collectedBy = data.collected_instance_id();
        } else {
            firstTs = std::min(firstTs, ts);
            lastTs = std::max(lastTs, ts);
            if (collectedBy != data.collected_instance_id()) collectedBy = "mixed";
        }
    }
};

} // namespace

GrpcServer::GrpcServer(AgentAuth auth) : auth_(std::move(auth)) {}

void GrpcServer::logReceived(const char* channel, const sysmonitor::AgentData& data) {
    // Phase 5에서 DataStore 저장으로 교체 예정 — 지금은 스냅샷 종류별 요약만 남긴다.
    switch (data.body_case()) {
    case sysmonitor::AgentData::kSys: {
        // optional 필드는 has_xxx()가 false면 Agent가 읽지 못한 것이다 — 0으로 읽지 말고 N/A로 다룬다.
        const auto& sys = data.sys();
        spdlog::info("[{}] agent={} sys ts_us={} cpu={} mem={}", channel, data.agent_id(), sys.timestamp_us(),
            sys.has_cpu_total() ? fmt::format("{:.1f}%", sys.cpu_total()) : std::string("N/A"),
            sys.has_mem_usage_percent() ? fmt::format("{:.1f}%", sys.mem_usage_percent()) : std::string("N/A"));
        break;
    }
    case sysmonitor::AgentData::kProc: {
        // 전체 목록이 오므로 개수 + CPU 최상위 1개만 요약한다 (TopN 선택은 UI 몫)
        // CPU 값이 없는 항목(Agent가 읽지 못함)은 0으로 치지 않고 비교에서 뺀다.
        const auto& procs = data.proc().procs();
        const sysmonitor::ProcInfo* top = nullptr;
        for (const auto& p : procs) {
            if (!p.has_cpu_usage()) continue;
            if (!top || p.cpu_usage() > top->cpu_usage()) top = &p;
        }
        if (top) {
            spdlog::info("[{}] agent={} proc ts_us={} procs={} top_cpu={}({:.1f}%, {})", channel, data.agent_id(), data.proc().timestamp_us(), procs.size(), top->name(), top->cpu_usage(),
                top->has_memory_mb() ? fmt::format("{:.1f}MB", top->memory_mb()) : std::string("N/A"));
        } else {
            // 목록이 비었거나, 어느 항목에도 CPU 값이 없다
            spdlog::info("[{}] agent={} proc ts_us={} procs={} top_cpu=N/A", channel, data.agent_id(), data.proc().timestamp_us(), procs.size());
        }
        break;
    }
    case sysmonitor::AgentData::kTarget: {
        // 타겟마다 이름 / 상태 / (실행 중이면) 메인 프로세스 CPU·메모리·핸들·GDI·자식 수
        // optional 필드는 has_xxx()가 false면 Agent가 수집하지 못한 것이다 (권한 부족 등) — 0으로 읽지 말고 N/A로 다룬다.
        std::string summary;
        for (const auto& t : data.target().targets()) {
            if (!summary.empty()) summary += ", ";
            summary += t.name() + "(" + sysmonitor::TargetInfo::Status_Name(t.status());
            if (t.status() == sysmonitor::TargetInfo::RUNNING) {
                const auto& m = t.main();
                summary += fmt::format(" pid={} cpu={} mem={} handles={} gdi={}{} children={}", m.pid(),
                    m.has_cpu_usage() ? fmt::format("{:.1f}%", m.cpu_usage()) : std::string("N/A"),
                    m.has_memory_mb() ? fmt::format("{:.1f}MB", m.memory_mb()) : std::string("N/A"),
                    m.has_handle_count() ? std::to_string(m.handle_count()) : std::string("N/A"),
                    m.has_gdi_object_count() ? std::to_string(m.gdi_object_count()) : std::string("N/A"),
                    t.has_exe_path() ? "" : " path=N/A",
                    t.children_size());
            }
            summary += ")";
        }
        spdlog::info("[{}] agent={} target ts_us={} targets={} [{}]", channel, data.agent_id(), data.target().timestamp_us(), data.target().targets_size(), summary);
        break;
    }
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

void GrpcServer::recordRealtimeTimestamp(const std::string& agentId, int64_t timestampUs) {
    std::lock_guard<std::mutex> lock(sessionsMtx_);
    // 최대 시간이 아니라 마지막으로 읽은 값이다. Agent는 이 값과 같은 틱을 자기가 보낸 순서에서 찾아 그 앞만 지운다 —
    // 최대 시간이면 Agent 시계가 뒤로 간 뒤의 틱이 모두 "이미 받은 것"보다 작아져, 받지 못한 틱까지 지워진다.
    sessions_[agentId].lastRealtimeTimestampUs = timestampUs;
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
	grpc::Status authStatus = authenticate(context, verifiedAgentId);       // agent_id/api_key 확인
    if (!authStatus.ok()) {
        return authStatus;
    }
    std::string instanceId;
	grpc::Status idStatus = readInstanceId(context, *verifiedAgentId, &instanceId);     // instance_id 확인
    if (!idStatus.ok()) {
        return idStatus;
    }
    return checkSession(*verifiedAgentId, instanceId, /*claim=*/true);
}

grpc::Status GrpcServer::checkUnary(grpc::ServerContext* context, std::string* verifiedAgentId) {
	grpc::Status authStatus = authenticate(context, verifiedAgentId);       // agent_id/api_key 확인
    if (!authStatus.ok()) {
        return authStatus;
    }
    std::string instanceId;
	grpc::Status idStatus = readInstanceId(context, *verifiedAgentId, &instanceId);     // instance_id 확인
    if (!idStatus.ok()) {
        return idStatus;
    }
    return checkSession(*verifiedAgentId, instanceId, /*claim=*/false);
}

grpc::Status GrpcServer::GetResumePoint(grpc::ServerContext* context, const sysmonitor::ResumePointRequest* /*request*/, sysmonitor::ResumePoint* response) {
    std::string agentId;
    grpc::Status status = checkUnary(context, &agentId);
    if (!status.ok()) {
        return status;
    }
    // Agent가 연결 중 주기적으로 부르므로 성공은 로그로 남기지 않는다.
    std::lock_guard<std::mutex> lock(sessionsMtx_);
    auto it = sessions_.find(agentId);
    if (it != sessions_.end() && it->second.lastRealtimeTimestampUs) {
        response->set_last_realtime_timestamp_us(*it->second.lastRealtimeTimestampUs);
    }
    return grpc::Status::OK;
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
        recordRealtimeTimestamp(agentId, snapshotTimestampUs(data));
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

    // 갭 데이터는 Agent의 로컬 버퍼에서 오므로, 지금 연결한 장비가 아니라 버퍼에 기록된 수집 장비가 출처다.
    // 출처 판정(수집 시각에 그 장비가 이 agent_id에 연결돼 있었는가)은 연결 이력을 저장하는 Phase 5에서 한다 — 지금은 다른 장비의 데이터가 섞여 왔는지만 알린다.
    const std::string connectedInstance = metadataValue(context, "instance_id");
    BatchSummary summary;
    sysmonitor::AgentData data;
    while (reader->Read(&data)) {
        grpc::Status msgStatus = validateMessage("batch", data, agentId);
        if (!msgStatus.ok()) {
            return msgStatus;
        }
        if (data.collected_instance_id().empty()) {
            spdlog::warn("[GrpcServer] batch message from agent={} has no collected_instance_id — closing stream", agentId);
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "gap message has no collected_instance_id");
        }
        summary.add(data);
    }
    if (summary.sys + summary.proc + summary.target > 0) {
        spdlog::info("[batch] agent={} gap snapshots sys={} proc={} target={} ts_us={}..{} collected_by={}", agentId,
            summary.sys, summary.proc, summary.target, summary.firstTs, summary.lastTs, summary.collectedBy);
        if (summary.collectedBy != connectedInstance) {
            spdlog::warn("[batch] agent={} gap data was collected by instance {} but arrived from instance {} — provenance is not checked until Phase 5",
                agentId, summary.collectedBy, connectedInstance);
        }
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
    // gRPC 서버 기본값은 2시간이라, 그동안 죽은 연결이 세션 표에 "활성"으로 남아 교체한 PC를 ALREADY_EXISTS로 막는다.
    // 약 40초 안에 감지하면 Read()가 false를 반환하고 ScopeExit가 세션을 푼다.
    // - KEEPALIVE_TIMEOUT만으로는 부족하다: 이 gRPC 버전(1.66)은 keepalive를 켜면 ping 응답 대기(PING_TIMEOUT)의
    //   기본값이 1분이고, 실측(패킷을 막는 중계기)에서 감지가 30초 + 60초 = 약 90초 걸렸다. 그래서 둘 다 10초로 맞춘다.
    // - GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA는 설정하지 않는다 — 이 버전은 서버 쪽 값을 0(무제한)으로 고정한다.
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 30 * 1000);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10 * 1000);
    // ping 응답 대기 인자는 공개 헤더에 없다 (grpc-src/src/core/ext/transport/chttp2/transport/internal.h의
    // GRPC_ARG_PING_TIMEOUT_MS). 내부 헤더를 include하지 않고 키 문자열을 직접 쓴다 — gRPC를 올리면 이 키가
    // 그대로인지, 패킷을 막는 중계기로 감지 시간이 여전히 약 40초인지 다시 확인할 것.
    builder.AddChannelArgument("grpc.http2.ping_timeout_ms", 10 * 1000);

    // Agent도 30초마다 keepalive ping을 보낸다. 서버 기본 정책은 "직전 ping과 5분 미만 간격인 ping"을 strike로 세고
    // strike가 2를 넘으면 too_many_pings로 연결을 끊는데, strike는 서버가 데이터/헤더를 보낼 때만 초기화된다 —
    // 이 서버는 스트림 중에 거의 아무것도 보내지 않으므로 기본값이면 Agent가 약 90초마다 끊긴다.
    // 허용 간격을 20초로 낮춰 30초 ping은 strike가 되지 않게 한다. Agent의 keepalive 간격(GrpcClient.cpp)은
    // 이 값 이상이어야 한다 — 두 값은 계약이므로 한쪽을 바꾸면 다른 쪽도 확인할 것. MAX_PING_STRIKES는 기본값(2) 유지.
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 20 * 1000);

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
