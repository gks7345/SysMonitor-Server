# SysMonitor Server

> 다중 Agent 중앙 집계 서버 + 통합 Web UI

## 개요

여러 PC에 설치된 SysMonitor Agent로부터 데이터를 수신하여 통합 대시보드로 제공하는 중앙 집계 서버입니다. Docker Compose로 서버와 UI를 함께 배포하는 것을 목표로 합니다.

## 현재 진행 상태

| 단계 | 내용 | 상태 |
|------|------|------|
| Phase 1 | gRPC 뼈대 (Agent 연결 수신) | ✅ 완료 |
| Phase 2 | 인증 + 설정 파일 (`agents.ini`, 장비 식별, 세션 관리) | ✅ 완료 |
| Phase 3 | 실제 수집 데이터 수신 | 🔧 proto 구조만 선반영 |
| Phase 5 | 저장 (DuckDB) + AgentManager | 예정 |
| Phase 6 | REST API + SSE | 예정 |
| Phase 7 | 역방향 명령 (타겟 등록/제거) | 예정 |
| Phase 8 | Web UI | 예정 |

지금 서버는 Agent를 인증하고, 받은 스냅샷을 **로그로만** 출력합니다. 저장소, REST/SSE, Web UI, Docker 배포는 아직 없습니다. 아래 설명 중 "(Phase N 예정)"으로 표시된 부분은 목표 설계입니다. 전체 설계 근거와 로드맵은 `SysMonitor-확장판_기획노트.md`에 있습니다.

## 아키텍처

```
Agent A (Windows) ──┐
Agent B (Windows) ──┼── gRPC ──→ 중앙 서버 ──→ Web UI (통합 대시보드)
Agent C (Windows) ──┘              │
                                   ├── 실시간: 메모리 보관 + DuckDB 저장 (Agent별 파일)   (Phase 5 예정)
                                   └── SSE → 브라우저 Push                              (Phase 6 예정)
```

## 구성 요소

```
SysMonitor-Server/
├── src/, include/       집계 서버 (C++)
│   ├── GrpcServer          Agent gRPC 연결 수신 · 인증 · 세션 관리      (구현됨)
│   ├── AgentAuth           agents.ini 기반 (agent_id, api_key) 검증      (구현됨)
│   ├── AgentManager        Agent 상태 관리                               (Phase 5 예정)
│   ├── DataStore           Agent별 DuckDB 저장                           (Phase 5 예정)
│   └── ApiServer           Web UI용 REST API + SSE                       (Phase 6 예정)
├── proto/
│   └── sysmonitor.proto    Agent와 같은 내용으로 유지
├── ui/                  통합 Web UI (React)                              (Phase 8 예정)
└── docker-compose.yml                                                    (예정)
```

## 기술 스택

| 분류 | 기술 |
|------|------|
| 서버 언어 | C++17 |
| 빌드 | CMake + FetchContent (의존성 자동 빌드) |
| 통신 (Agent ↔ 서버) | gRPC (Protobuf) |
| 통신 (서버 → UI) | SSE (Server-Sent Events) |
| 과거 조회 | HTTP REST |
| 설정 | inih (ini 파서) |
| 저장 | DuckDB (Agent별 파일 분리) |
| UI | React + Recharts |
| 배포 | Docker Compose |

## 통신 설계

### Agent → 서버 (gRPC)

```
왜 gRPC인가
  실시간 스냅샷(1초)과 재연결 후 갭 데이터(대량)를 동시에 처리
  HTTP는 대량 전송 중 실시간 데이터 블로킹 발생
  gRPC HTTP/2 멀티플렉싱으로 두 채널이 독립 동작
  keepalive ping으로 연결 상태 감지
  서버 → Agent 역방향 제어 명령 가능

채널 구성 (Agent당)
  실시간 채널  1초마다 sys / proc(전체 목록) / target 스냅샷
  배치 채널    재연결 후 갭 데이터 전송 전용
  역방향       서버 → Agent 타겟 등록/제거 명령             (Phase 7 예정)
```

- 메시지 1개에 스냅샷 1건을 담고, 종류(sys / proc / target)는 `oneof`로 구분합니다.
- `proto/sysmonitor.proto`는 SysMonitor-Agent와 **같은 내용**이어야 합니다. 구조를 바꾸면 파일 안의 `PROTOCOL_VERSION_CURRENT`를 올립니다. 버전이 다른 Agent는 연결 시점에 거부됩니다.

### 인증과 장비 식별

Agent는 모든 요청에 gRPC metadata로 `agent_id`, `api_key`, `instance_id`(Windows `MachineGuid`)를 싣습니다.

```
Hello (연결 직후 1회)
  (agent_id, api_key) 확인        → 틀리면 UNAUTHENTICATED
  프로토콜 버전 확인               → 다르면 FAILED_PRECONDITION
  같은 agent_id를 다른 장비가 사용 중인지 확인 → 사용 중이면 ALREADY_EXISTS

스트림 (실시간 / 배치)
  요청마다 다시 인증 (Hello를 건너뛴 클라이언트도 막기 위해)
  메시지마다 agent_id가 인증된 값과 같은지 확인 → 다르면 PERMISSION_DENIED
```

- **`agent_id`는 자리(역할), `instance_id`는 장비입니다.** 장비를 교체해도 기록은 같은 `agent_id`로 이어지고, `instance_id`는 "지금 접속한 것이 같은 장비인가"를 판단하는 데 씁니다.
- **같은 `agent_id`는 한 번에 한 장비만 접속할 수 있습니다.** 설치 폴더를 복사하고 `id`를 바꾸지 않은 두 PC의 데이터가 섞이는 것을 막기 위해서입니다. 같은 장비의 재접속은 항상 허용되고, 접속 중인 장비가 없으면 새 장비로 바뀝니다(PC 교체).
- 서버는 30초마다 keepalive ping을 보내, 전원이 꺼지거나 랜선이 뽑혀 **아무 신호 없이 사라진 Agent를 약 40초 안에** 감지하고 연결을 정리합니다.
- mTLS는 Agent 수가 늘어나거나 외부망 노출이 필요해지는 시점에 도입을 검토합니다.

### 서버 → UI (SSE)

```
왜 SSE인가
  서버 → 브라우저 단방향 push만 필요
  WebSocket보다 구현 단순
  HTTP 위에서 동작 → 방화벽 통과 용이
  자동 재연결 내장
  브라우저 기본 지원 (별도 라이브러리 불필요)

왜 WebSocket이 아닌가
  UI → 서버 방향은 HTTP 요청으로 충분
  (Agent 선택, 필터, 타겟 등록 등)
  양방향이 필요한 구간이 없음
```

## 데이터 흐름 (Phase 5~6 예정)

```
실시간 흐름
  Agent 실시간 채널 (1초마다 전체 스냅샷)
  → 서버 메모리 (Agent별 RingBuffer) → SSE → Web UI 실시간 그래프
  → 서버 DuckDB (agent_PC-01_2026-09-14.db) → HTTP REST → Web UI 과거 기록 조회

갭 채우기 (재연결 시)
  Agent 재연결
  → 서버에 마지막 수신 timestamp 요청
  → Agent가 로컬 DuckDB에서 갭 조회
  → 배치 채널로 전송
  → 서버가 출처 확인 후 DuckDB 갭 채움
```

- **갭 데이터의 출처 확인:** 서버는 `agent_id`별로 "어느 장비가 언제부터 언제까지 연결됐었는지" 이력을 저장하고, 갭 스냅샷마다 "이 데이터를 수집한 장비가 그 시각에 이 자리에 연결되어 있었는가"를 확인합니다. PC 교체로 따라온 옛 장비의 데이터는 기록에 이어 붙이고, 다른 자리로 잘못 복사된 데이터는 저장하지 않고 따로 보관합니다.

## Agent 상태 관리 (Phase 5 예정)

```
각 Agent의 마지막 수신 시각 기록
  30초 이상 미수신 → Warning
  90초 이상 미수신 → Offline

연결 끊김은 keepalive로 약 40초 안에 감지
→ UI에 상태 변경 반영
```

## 타겟 등록/제거 (Phase 7 예정)

```
Web UI에서 타겟 등록 요청
  → 서버 HTTP API
  → gRPC 역방향으로 해당 Agent에 명령 전달
  → Agent pendingByName 큐에 적재
  → 메인 루프 applyPending() 처리
  → 완료 ACK → 서버 → UI
```

## DB 구조 (Phase 5 예정)

```
Agent별 파일 분리
  data/
  ├── agent_PC-01_2026-09-14.db
  ├── agent_PC-02_2026-09-14.db
  └── agent_PC-03_2026-09-14.db

행마다 수집한 장비(instance_id) 저장
  장비를 교체해도 같은 agent_id 기록으로 이어지고, 교체 시점을 구분할 수 있음

장비 연결 이력
  agent_id별 (instance_id, 연결 시작, 연결 종료) — 갭 데이터 출처 확인에 사용

날짜별 로테이션
  자정마다 새 파일 생성
  ATTACH로 과거 파일 조회 가능

보관 정책
  RETENTION_DAYS 설정값 (기본 90일)
  자정 로테이션 시 RETENTION_DAYS보다 오래된 파일 자동 삭제
  (무기한 누적으로 인한 디스크 사용량 증가 방지)
```

## Web UI 기능 (Phase 8 예정)

```
전체 Agent 대시보드
  Agent 목록 + 온/오프라인 상태
  전체 통합 CPU/메모리 현황

Agent 상세 (선택 시)
  실시간 그래프 (120초 RingBuffer), 장비 교체 시점 표시
  프로세스 Top N (개수와 정렬 기준을 UI에서 선택)
  타겟 프로그램 현황

과거 기록
  날짜 선택 → 해당 날짜 요약
  시간대 선택 → 상세 그래프

타겟 관리
  Agent별 타겟 프로그램 등록/제거
  중앙에서 일괄 관리
```

## 미들웨어

```
현재 단계
  미들웨어 없음
  gRPC가 연결/재연결/상태 감지 처리
  Agent 로컬 DuckDB가 버퍼 역할

추후 Agent 수 증가 시 고려
  Redis Streams  Agent 20대 이상, 순간 스파이크 완충
  Kafka          Agent 수백 대 이상, 대용량 처리
```

## 설정

실행 위치(현재 작업 디렉터리)의 `agents.ini`에서 읽습니다. `agents.ini.example`을 복사해 사용합니다.

```ini
[server]
listen_addr = 0.0.0.0:50051   ; 서버가 대기할 주소

[agents]
PC-01 = <정한 키>              ; agent_id = api_key — 각 Agent의 agent.ini와 같은 값
PC-02 = <정한 키>
```

- **파일이 없거나 등록된 Agent가 없으면 모든 연결을 거부합니다.**
- **예제의 키는 일부러 비워 두었습니다.** 값이 빈 항목은 등록되지 않으므로, 예제를 그대로 쓰면 모든 Agent가 거부됩니다. Agent마다 키로 쓸 문자열을 정해 양쪽에 같은 값으로 넣습니다.
- 섹션 이름은 대소문자를 구분하지 않지만(`[Agents]`도 동작), **agent_id는 대소문자를 구분**합니다.
- **틀린 줄**(`=` 누락 등)은 줄 번호와 함께 경고하고 건너뜁니다. 나머지 항목과 `listen_addr`는 그대로 적용됩니다. 같은 agent_id가 두 번 있으면 경고하고 나중 값을 씁니다.
- 포트를 열 수 없으면(잘못된 주소, 사용 중인 포트) 원인을 로그로 남기고 종료합니다.

## 빌드 및 실행

의존성(gRPC, Protobuf, spdlog, inih)은 CMake FetchContent가 소스로 받아 함께 빌드합니다.

```powershell
# Visual Studio 2022 개발자 환경에서
cmake -B out/build/x64-Debug
cmake --build out/build/x64-Debug

# 실행 — agents.ini가 있는 폴더에서 실행
cd out/build/x64-Debug/src
.\SysMonitorServer.exe
```

## 배포 (예정)

```bash
# 실행
docker-compose up -d

# 접속
http://서버IP:80
```

```yaml
# docker-compose.yml
version: '3.8'
services:
  server:
    build: ./server
    ports:
      - "50051:50051"   # gRPC (Agent 연결)
      - "8080:8080"     # HTTP REST + SSE (Web UI)
    volumes:
      - ./data:/app/data
    restart: always

  ui:
    build: ./ui
    ports:
      - "80:80"
    depends_on:
      - server
    restart: always
```

## 확장 계획

```
단기
  Linux Agent 추가 (/proc 기반)
  → Windows PC 외 Linux 서버도 모니터링 가능

중기
  Redis Streams 미들웨어 추가
  → Agent 수십 대 이상 부하 분산

장기
  중앙 서버 다중화
  → 서버 장애 시 전체 장애 방지
```

## 관련 프로젝트

- [SysMonitor](https://github.com/gks7345/SysMonitor) — 단일 PC 모니터링 (기반 프로젝트)
- [SysMonitor-Agent](링크) — 다중 PC Agent
