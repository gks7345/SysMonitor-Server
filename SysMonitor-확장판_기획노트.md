# SysMonitor 확장판 기획 노트

> 대화를 통해 결정된 사항들과 판단 근거 기록

---

## 1. 통신 방식

### 결정: Agent → 서버는 gRPC

**검토한 방식**

| 방식 | 검토 결과 |
|------|----------|
| Raw TCP 소켓 | 프로토콜 직접 설계 필요, 방화벽 문제, 이점 없음 → 기각 |
| HTTP REST | 배치 전송 중 실시간 블로킹, 채널 분리 불가 → 기각 |
| WebSocket | HTTP/1.1 기반, 멀티플렉싱 없음, 배치 블로킹 가능 → 기각 |
| gRPC | HTTP/2 멀티플렉싱, 스트리밍, 연결 감지 → 채택 |

**gRPC 선택 근거**
- 실시간(1초, 소량)과 배치(60초, 수 MB)를 동시에 처리해야 함
- 같은 채널에 넣으면 배치 전송 중 실시간 데이터 HOL Blocking 발생
- HTTP/2 멀티플렉싱으로 두 스트림이 독립 동작
- keepalive ping으로 연결 끊김 즉시 감지
- 역방향(서버 → Agent) 제어 명령 가능

**레퍼런스**
- Pinpoint: gRPC Client Streaming (Stat/Span/Agent 채널)
- Datadog: HTTPS Push (Agent Forwarder)
- Prometheus: HTTP Pull (scrape 모델, 데이터 크기 최소화로 HTTP 유지)

---

### 결정: Agent 인증은 Static API Key로 시작

**문제**
- `agent_id`가 설정 파일의 평문 문자열(`"PC-01"`)이라, 다른 프로세스가 동일한 ID로 접속해도 서버가 구분할 수 없음
- 사내망 전제라도 최소한의 신원 확인 없이는 다른 PC나 프로세스가 임의로 데이터를 주입할 수 있음

**검토한 방식**

| 방식 | 검토 결과 |
|------|----------|
| 인증 없음 (agent_id만 신뢰) | 스푸핑 방지 불가 → 기각 |
| mTLS | 신원 보장 가장 강력하나, 초기 단계엔 인증서 발급/배포 비용이 큼 → 추후로 연기 |
| Static API Key | 구현 간단, gRPC call credentials(metadata)로 전달 가능, 최소한의 신원 확인 충족 → 채택 |

**결정 사항**
```
Agent 설정 파일에 agent_id + api_key 보관
gRPC 요청 시 metadata에 api_key 포함
서버는 등록된 (agent_id, api_key) 목록과 대조
  일치 → 연결 허용
  불일치 → 연결 거부 + 로그 기록
```
- mTLS는 Agent 수가 늘어나거나 외부망 노출이 필요해지는 시점에 도입 검토 (§11 미결 사항 유지)

---

### 결정: 서버 → UI는 SSE

**검토한 방식**

| 방식 | 검토 결과 |
|------|----------|
| HTTP 폴링 | 지연, 불필요한 요청 → 기각 |
| WebSocket | 양방향 불필요, 구현 복잡 → 기각 |
| SSE | 단방향 push, 구현 단순, 자동 재연결 → 채택 |

**SSE 선택 근거**
- 서버 → 브라우저 단방향만 필요
- UI → 서버는 HTTP 요청으로 충분 (Agent 선택, 필터 등)
- WebSocket보다 구현 단순
- HTTP 위에서 동작 → 방화벽 통과, CDN 지원

**서버 → UI 데이터 크기 문제**
- Agent가 보내는 수 MB 배치 데이터가 UI에 그대로 가지 않음
- 서버가 집계/처리 후 수 KB 요약 데이터만 UI로 전송
- → SSE로 충분

---

## 2. 채널 설계

### 결정: Agent당 채널 2개 (gRPC 스트림 2개, TCP 1개)

**검토 과정**
- 처음에는 채널 1개 + 타입 필드로 구분 검토
- 배치 데이터(수 MB) 전송 중 실시간 데이터 블로킹 가능성
- 일반 PC 프로세스 100~200개 × 60초 = 수 MB 배치
- → 채널 2개로 분리

**채널 구성**
```
실시간 채널  1초마다  수 KB  sys/proc/target 현재 수치
배치 채널    60초마다 수 MB  sys/proc/target 상세 스냅샷
역방향       서버 → Agent   타겟 등록/제거 명령 (Bidirectional)
```

**타겟 데이터는 별도 채널 불필요**
- sys/proc/target 모두 같은 주기 (실시간 1초, 배치 60초)
- 성격이 같으므로 타입 필드로 구분, 같은 채널 사용

**Pinpoint와의 차이**
- Pinpoint: Agent당 TCP 연결 3개 (Span/Stat/Agent 각각 독립 서버 분리 가능)
- SysMonitor: Agent당 TCP 연결 1개 (HTTP/2 멀티플렉싱으로 스트림 2개)
- Pinpoint는 Collector 독립 스케일 아웃이 목적, SysMonitor는 불필요

---

## 3. 데이터 동기화

### 결정: 로컬 DuckDB는 단기 버퍼 역할만

**검토 과정**
- 처음에는 로컬 DuckDB를 영구 저장 + 연결 버퍼로 사용 검토
- 역할이 혼재되어 일관성 부족
- 영구 저장은 중앙 서버, 로컬은 버퍼로 역할 분리

**결정 사항**
```
로컬 DuckDB   단기 버퍼 (최대 24시간)
              연결 끊김 시 임시 저장
              재연결 시 갭 전송 후 삭제

중앙 서버 DuckDB   영구 저장
                   과거 기록 조회
                   Agent별 파일 분리
```

**재연결 시 갭 채우기**
1. gRPC keepalive로 재연결 감지
2. 서버에 마지막 수신 timestamp 요청
3. 로컬 DuckDB에서 갭 데이터 조회
4. 배치 채널로 갭 전송
5. 로컬 버퍼 데이터 삭제
6. 정상 스트리밍 재개

### 결정: 로컬 DuckDB는 메인 수집 루프 스레드에서만 접근

**문제**
- 메인 루프가 1초마다 로컬 DuckDB에 쓰는 도중, 재연결 시 네트워크 스레드가 갭 조회를 위해 같은 DB를 읽으면 동시 접근 발생

**결정 사항**
- 기존 SysMonitor의 `TargetCollector`가 `registerTarget`/`unregisterTarget`을 메인 루프 전용으로 못박고, HTTP 스레드는 `requestRegister`/`requestUnregister`로 커맨드 큐 + `std::future`를 통해서만 결과를 받던 패턴을 그대로 재사용
- 네트워크 스레드는 "갭 데이터 요청"을 커맨드 큐에 적재하고 `std::future`로 결과 수신
- 실제 DuckDB 읽기/쓰기는 메인 루프에서만 수행 → 동시성 문제 원천 차단

### 결정: 재연결 백오프에 랜덤 지터 추가

**문제**
- 서버가 재시작되면, 연결돼 있던 모든 Agent가 거의 동일한 타이밍에 keepalive 실패를 감지하고 동시에 재연결 + 갭 전송을 시도 (thundering herd)
- Agent 수가 늘어날수록 서버 재기동 직후 순간 부하가 커짐

**결정 사항**
```
지수 백오프 각 단계에 ±20~30% 랜덤 지터 적용
예: 1초 → 2초 → 4초 ... 각 값에 무작위 오프셋을 더해 재연결 타이밍 분산
```
- 지금은 Agent 수가 적어 체감되지 않지만, 나중에 대수가 늘었을 때 겪을 문제를 미리 방지하는 차원에서 초기 구현에 포함

---

## 4. Web UI 구성

### 결정: 완전 중앙화 (Agent Web UI 제거, 통합 UI만 유지)

**검토 과정**
- 처음에는 Agent 로컬 UI + 통합 UI 둘 다 유지 검토
- 타겟 등록/제거를 중앙 서버에서 관리하기로 결정
- Agent에도 UI가 있으면 관리 포인트가 두 곳 → 중앙화 의미 반감
- → Agent Web UI 제거, 통합 UI만 유지

**폴백 모드 (추후 추가)**
```
SEND_TO_SERVER = false 설정 시
  → Agent 로컬 Web UI 활성화
  → 기존 SysMonitor 동작 (단독 모드)

SEND_TO_SERVER = true 설정 시
  → 로컬 UI 비활성화
  → 통합 Web UI 사용
```
- A플랜(완전 중앙화)으로 시작 → C플랜(조건부 전환) 추후 추가
- Agent Web UI 코드가 이미 존재하므로 플래그 추가만으로 구현 가능

### 타겟 등록/제거 위치

**결정: 통합 Web UI에서 관리**
- 중앙화의 목적 = 여러 Agent를 한 곳에서 통합 관리
- 타겟 등록도 관리의 일부
- gRPC Bidirectional Streaming으로 서버 → Agent 역방향 명령 전달

---

## 5. 미들웨어

### 결정: 현재 단계 미들웨어 없음

**판단 근거**
- gRPC가 연결/재연결/상태 감지 처리
- 로컬 DuckDB가 단기 버퍼 역할
- Agent 수십 대 이하에서는 불필요

**추후 고려 시점**
```
Redis Streams   Agent 20대 이상, 60초 배치 스파이크 완충
Kafka           Agent 수백 대 이상, 대용량 처리, 서버 다중화
```

---

## 5-1. 서버 DuckDB 보관 정책

### 결정: 보관 기간을 설정값으로 두고, 만료 파일 자동 삭제

**문제**
- Agent별·날짜별 DuckDB 파일(`agent_PC-01_2026-09-14.db`)이 로테이션은 되지만 삭제되지는 않아 무기한 누적 → 디스크 사용량 증가

**결정 사항**
```
설정값 RETENTION_DAYS 도입 (기본값 예: 90일)
자정 로테이션 루틴에서 RETENTION_DAYS보다 오래된 파일 함께 삭제
```
- 초기 구현에서는 수동 삭제도 허용하되, 정책 자체는 §6 프로젝트 구조 단계부터 명시해 나중에 빠뜨리지 않도록 함

---

## 6. 프로젝트 구조

### 결정: 레포 3개로 분리

| 레포 | 역할 | 배포 |
|------|------|------|
| SysMonitor | 단일 PC 모니터링 (기반 프로젝트) | Windows exe |
| SysMonitor-Agent | gRPC 기반 Agent | Windows exe |
| SysMonitor-Server | 중앙 서버 + 통합 UI | Docker Compose |

**분리 근거**
- Agent: Windows 전용, 각 PC에 설치, exe 배포
- 서버: Linux, Docker, 단일 서버에 배포
- 배포 환경이 완전히 달라 같은 레포로 관리하면 혼재
- Agent도 gRPC로 변경되어 기존 코드에서 변경량이 상당함

**Agent를 새 레포로 파는 이유**
- ApiServer 전체 교체 (cpp-httplib → gRPC)
- CMakeLists.txt 대폭 변경 (Protobuf + gRPC 추가)
- Web UI 제거
- 기존 SysMonitor는 포트폴리오 1번 프로젝트로 유지

---

## 7. Agent 설정 관리

### 결정: 컴파일타임 상수 대신 런타임 설정 파일 사용

**문제**
- 기존 SysMonitor의 `Config.h`는 `SERVER_URL`, `AGENT_ID` 등을 컴파일타임 상수로 박아둠
- 단일 PC 전제에서는 문제없었으나, PC마다 값이 달라지는 다중 Agent 배포에서는 PC마다 다시 빌드해야 하는 문제 발생

**결정 사항**
```
exe와 같은 경로의 agent.json(또는 .ini)에서 런타임 로드

{
  "server_url": "192.168.0.100:50051",
  "agent_id": "PC-01",
  "api_key": "...",
  "send_to_server": true,
  "local_buffer_hours": 24
}
```
- 파일이 없으면 기본값으로 폴백 (`send_to_server=false`, 로컬 UI 모드)
- `api_key`는 §1 "Agent 인증" 결정에 따라 gRPC metadata로 전달

**근거**
- 빌드 1회로 여러 PC에 동일 exe 배포 가능
- 설정 변경 시 재빌드 없이 파일 수정 + 재시작만으로 적용

---

## 8. Agent 배포 방식

### 결정: Docker 미사용, Windows 네이티브 설치

**판단 근거**
- Docker 컨테이너는 호스트 OS와 격리
- PDH API → 컨테이너 내부 리소스만 수집 (호스트 전체 불가)
- ETW → 커널 레벨 이벤트가 컨테이너 경계에서 차단
- 모니터링 도구의 목적 자체가 불가능해짐

**배포 방식**
- Windows 서비스 등록 (부팅 시 자동 시작) 권장
- 또는 SysMonitor-Agent.exe 직접 실행

---

## 9. 플랫폼 지원

### 현재: Windows 전용

**이유**
- PDH, ETW, Win32 API 모두 Windows 전용
- 이 API들이 SysMonitor의 핵심 기술

**추후 확장**
- Linux Agent 별도 개발 (/proc 파일시스템 기반)
  - CPU:      /proc/stat
  - 메모리:   /proc/meminfo
  - 프로세스: /proc/{pid}/stat
  - 네트워크: /proc/net/dev
- 공통 인터페이스 추상화 (ISystemCollector)

---

## 10. 레퍼런스 도구 정리

| 도구 | Agent→서버 | 서버→UI | 특징 |
|------|-----------|---------|------|
| Prometheus | HTTP Pull | Grafana HTTP | 데이터 최소화로 HTTP 병목 회피 |
| Datadog | HTTPS Push (배치) | WebSocket | Forwarder가 로컬 버퍼 |
| Pinpoint | gRPC (채널 3개) | HTTP 폴링 | APM 특화, 채널 분리로 스케일 아웃 |
| SysMonitor | gRPC (채널 2개) | SSE | 인프라 모니터링, 중앙화 |

---

## 11. 성능 고려 사항

**JSON 직렬화 병목 시 해결 방법**
1. DuckDB가 직접 JSON 생성 (C++ 직렬화 제거)
2. HTTP gzip 압축 (JSON 텍스트 70~80% 압축)
3. rapidjson 교체 (nlohmann 대비 3~5배 빠름)
4. Protobuf 도입 (gRPC 전환 시 자동 해결)

**과거 기록 조회 느린 경우**
1. hour 파라미터로 쿼리 범위 제한 (즉각 효과)
2. DuckDB 인덱스 추가 (timestamp 기준)
3. 다운샘플링 (1초 → 5분 평균, 데이터 5배 감소)
4. SummaryStore 적극 활용 (이미 집계된 데이터)

---

## 12. 미결 사항 (추후 결정)

```
Linux Agent 개발 시점
  Windows 전용 유지 vs Linux 병행 개발

미들웨어 도입 기준
  Agent 몇 대부터 Redis 도입할 것인가

중앙 서버 다중화
  서버 장애 대비 HA 구성 필요 시점

gRPC 인증 고도화
  Static API Key(§1 결정)에서 mTLS로 전환할 시점 (Agent 수 증가 또는 외부망 노출 시)

로컬 버퍼 보관 기간
  24시간이 적절한가, 조정 가능하게 할 것인가
```

---

## 13. 개발 로드맵

> 기능 단위로 개발하고 각 Phase마다 체크포인트로 검증한다.
> 전체 흐름은 **데이터가 흐르는 경로 순서** — 수집 → 전송 → 수신 → 저장 → 표시.

### Phase 개요

| Phase | 핵심 | 완료 기준 |
|-------|------|----------|
| 1 | gRPC 뼈대 | Agent ↔ Server 더미 데이터 송수신 |
| 2 | 인증 + 설정 파일 | 잘못된 api_key 연결 거부 확인 |
| 3 | 실제 수집 데이터 전송 | sys / proc / target 실수치 서버 수신 |
| 4 | 연결 끊김 처리 | 서버 30초 중단 후 재연결 시 갭 채워짐 |
| 5 | Server 저장 + AgentManager | DuckDB 파일 적재 + 상태 관리 |
| 6 | REST API + SSE | curl로 엔드포인트 응답 확인 |
| 7 | 역방향 명령 (타겟 등록/제거) | UI 요청 → Agent 수집 시작 확인 |
| 8 | Web UI | 브라우저에서 다중 Agent 전환 확인 |
| 9 | 로컬 폴백 모드 | 플래그 전환만으로 두 모드 정상 동작 |

---

### Phase 1 — gRPC 뼈대

Agent와 Server가 실제로 연결되고 데이터를 주고받는 최소 골격.
이게 없으면 이후 모든 테스트가 불가능하다.

```
1-1. proto 파일 작성 (AgentData, 채널 2개 정의)
1-2. Server: gRPC 수신 서버 기동 (받은 데이터 로그 출력만)
1-3. Agent:  gRPC 클라이언트 연결 + 더미 데이터 전송
1-4. 실시간 채널 / 배치 채널 분리 확인
```
✅ Server 로그에 PC-01의 SYS_REALTIME 수신 확인

---

### Phase 2 — 인증 + 설정 파일

Phase 1 연결에 신원 확인을 붙이고 하드코딩을 제거한다.

```
2-1. agent.json 로드 (server_url / agent_id / api_key)
2-2. Agent: gRPC metadata에 api_key 포함
2-3. Server: (agent_id, api_key) 대조 + 거부 처리
```
✅ 잘못된 api_key로 연결 시 서버가 거부하는지 확인

---

### Phase 3 — 실제 수집 데이터 전송

기존 SysMonitor 수집 엔진을 붙여 더미 데이터를 실제 데이터로 교체한다.
sys / proc / target 모두 주기가 같아(실시간 1초, 배치 60초) 한 Phase에 묶는다.

```
3-1. 기존 SystemCollector / ProcessCollector / TargetCollector 재사용
3-2. 수집 데이터 → Protobuf 직렬화 → payload
3-3. 실시간 채널: 1초마다 SYS_REALTIME / PROC_REALTIME / TARGET_REALTIME 전송
3-4. 배치 채널:   60초마다 SYS_BATCH / PROC_BATCH / TARGET_BATCH 전송
```
✅ Server에서 실제 CPU / 메모리 / 타겟 수치 수신 확인

---

### Phase 4 — 연결 끊김 처리

안정성의 핵심. 여기까지 되면 실사용 가능한 수준이 된다.

```
4-1. keepalive ping 설정 + 끊김 감지
4-2. 지수 백오프 + ±20~30% 랜덤 지터 재연결
4-3. 끊김 동안 로컬 DuckDB 버퍼에 저장
      (메인 루프 스레드만 접근, 네트워크 스레드는 커맨드 큐 경유)
4-4. 재연결 시 서버에 last timestamp 요청 → 갭 전송 → 버퍼 삭제
```
✅ 서버를 30초간 껐다 켰을 때 갭이 채워지는지 확인

---

### Phase 5 — Server 저장 + AgentManager

서버가 받은 데이터를 실제로 저장하고 Agent 상태를 관리한다.

```
5-1. AgentManager: Agent별 상태 관리 (online / warning / offline)
       30초 미수신 → Warning / 90초 미수신 → Offline
5-2. DataStore: Agent별 DuckDB 파일 저장
       data/agent_PC-01_2026-09-17.db
5-3. 날짜별 로테이션 + RETENTION_DAYS 초과 파일 자동 삭제
5-4. 실시간 데이터: 메모리 RingBuffer 보관
```
✅ agent_PC-01_{날짜}.db 파일에 데이터 적재 확인

---

### Phase 6 — REST API + SSE

Web UI가 소비할 API. 기존 SysMonitor ApiServer 구조를 참고한다.

```
6-1. REST API: 과거 기록 조회 (기존 엔드포인트 구조 재사용, agent_id 파라미터 추가)
6-2. SSE: 실시간 데이터 브라우저 push
6-3. Agent 목록 / 상태 조회 엔드포인트
```
✅ curl로 /api/agents, /api/current/system?agent=PC-01 응답 확인

---

### Phase 7 — 역방향 명령 (타겟 등록/제거)

서버 API가 준비된 이후에 붙인다.
UI 요청 → 서버 → gRPC 역방향 → Agent 흐름을 완성한다.

```
7-1. gRPC Bidirectional Streaming으로 Server → Agent 명령 전달
7-2. Agent: pendingByName 큐 → applyPending() (기존 구조 재사용)
7-3. 완료 ACK → Server → UI
```
✅ UI에서 타겟 등록 후 Agent에서 수집 시작되는지 확인

---

### Phase 8 — Web UI

API와 역방향 명령이 모두 준비된 뒤 한 번에 완성도 있게 만든다.

```
8-1. Agent 목록 + 온/오프라인 상태 대시보드
8-2. Agent 선택 → 실시간 그래프 (SSE 연결)
8-3. 과거 기록 조회 (날짜 / 시간 선택)
8-4. 타겟 등록/제거 UI (Phase 7 역방향 명령 연동)
```
✅ 브라우저에서 PC-01 / PC-02 전환하며 실시간 그래프 확인

---

### Phase 9 — 로컬 폴백 모드

`send_to_server: false`일 때 기존 SysMonitor로 동작하는 분기.
Agent Web UI 코드가 이미 존재하므로 플래그 추가만으로 구현 가능하다.

```
9-1. agent.json의 send_to_server 플래그로 분기
       true  → gRPC 클라이언트 시작, ApiServer 비활성화
       false → ApiServer 활성화 (기존 그대로), gRPC 비활성화
```
✅ 플래그 전환만으로 두 모드 정상 동작 확인