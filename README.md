# SysMonitor Server

> 다중 Agent 중앙 집계 서버 + 통합 Web UI

## 개요

여러 PC에 설치된 SysMonitor Agent로부터 데이터를 수신하여 통합 대시보드로 제공하는 중앙 집계 서버입니다. Docker Compose로 서버와 UI를 함께 배포합니다.

## 아키텍처

```
Agent A (Windows) ──┐
Agent B (Windows) ──┼── gRPC ──→ 중앙 서버 ──→ Web UI (통합 대시보드)
Agent C (Windows) ──┘              │
                                   ├── 실시간: 메모리 보관
                                   ├── 배치:   DuckDB (Agent별 파일)
                                   └── SSE/WebSocket → 브라우저 Push
```

## 구성 요소

```
SysMonitor-Server/
├── server/              집계 서버 (C++)
│   ├── src/
│   │   ├── GrpcServer      Agent gRPC 연결 수신
│   │   ├── AgentManager    Agent 상태 관리
│   │   ├── DataStore       Agent별 DuckDB 저장
│   │   └── ApiServer       Web UI용 REST API + SSE
│   └── proto/
│       └── sysmonitor.proto
├── ui/                  통합 Web UI (React)
└── docker-compose.yml
```

## 기술 스택

| 분류 | 기술 |
|------|------|
| 서버 언어 | C++17 |
| 통신 (Agent ↔ 서버) | gRPC (Protobuf) |
| 통신 (서버 → UI) | SSE (Server-Sent Events) |
| 과거 조회 | HTTP REST |
| 저장 | DuckDB (Agent별 파일 분리) |
| UI | React + Recharts |
| 배포 | Docker Compose |

## 통신 설계

### Agent → 서버 (gRPC)

```
왜 gRPC인가
  실시간(1초)과 배치(60초, 수 MB)를 동시에 처리
  HTTP는 배치 전송 중 실시간 데이터 블로킹 발생
  gRPC HTTP/2 멀티플렉싱으로 두 채널이 독립 동작
  연결 상태 실시간 감지 (keepalive ping)
  서버 → Agent 역방향 제어 명령 가능

채널 구성 (Agent당)
  실시간 채널  1초마다 소량 데이터
  배치 채널    60초마다 대량 데이터
  역방향       서버 → Agent 타겟 등록/제거 명령
```

**인증**: Agent는 gRPC metadata에 `api_key`를 실어 보내고, 서버는 등록된 `(agent_id, api_key)` 조합과 대조해 연결을 허용/거부합니다. `agent_id` 문자열만으로는 다른 프로세스의 스푸핑을 막을 수 없기 때문입니다. (mTLS는 Agent 수가 늘어나거나 외부망 노출이 필요해지는 시점에 도입 검토)

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

## 데이터 흐름

```
실시간 흐름
  Agent 실시간 채널
  → 서버 메모리 (Agent별 RingBuffer)
  → SSE → Web UI 실시간 그래프

배치 흐름
  Agent 배치 채널
  → 서버 DuckDB (agent_A_2026-09-14.db)
  → HTTP REST → Web UI 과거 기록 조회

갭 채우기 (재연결 시)
  Agent 재연결
  → 서버에 마지막 수신 timestamp 요청
  → Agent가 로컬 DuckDB에서 갭 조회
  → 배치 채널로 전송
  → 서버 DuckDB 갭 채움
```

## Agent 상태 관리

```
각 Agent의 마지막 수신 시각 기록
  30초 이상 미수신 → Warning
  90초 이상 미수신 → Offline

gRPC keepalive로 연결 끊김 즉시 감지
→ UI에 즉시 상태 변경 반영
```

## 타겟 등록/제거

```
Web UI에서 타겟 등록 요청
  → 서버 HTTP API
  → gRPC 역방향으로 해당 Agent에 명령 전달
  → Agent pendingByName 큐에 적재
  → 메인 루프 applyPending() 처리
  → 완료 ACK → 서버 → UI
```

## DB 구조

```
Agent별 파일 분리
  data/
  ├── agent_PC-01_2026-09-14.db
  ├── agent_PC-02_2026-09-14.db
  └── agent_PC-03_2026-09-14.db

날짜별 로테이션
  자정마다 새 파일 생성
  ATTACH로 과거 파일 조회 가능

보관 정책
  RETENTION_DAYS 설정값 (기본 90일)
  자정 로테이션 시 RETENTION_DAYS보다 오래된 파일 자동 삭제
  (무기한 누적으로 인한 디스크 사용량 증가 방지)
```

## Web UI 기능

```
전체 Agent 대시보드
  Agent 목록 + 온/오프라인 상태
  전체 통합 CPU/메모리 현황

Agent 상세 (선택 시)
  실시간 그래프 (120초 RingBuffer)
  프로세스 Top N (정렬 기준 선택)
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

## 배포

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
