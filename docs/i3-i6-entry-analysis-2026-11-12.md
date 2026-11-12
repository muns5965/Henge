# I3~I6 착수 준비: 소스 대응 분석 (2026-11-12)

성격: **읽기 전용 분석 + 구현 계획.** 벤더링 소스, I10, `main`은 변경하지 않았다. 이 문서의 어떤 항목도 "검증 완료"가 아니다. 각 항목은 아래 세 가지 중 하나로만 표시한다.
- **[확인]** 이번 분석에서 소스를 직접 읽어 확인한 사실(파일:줄)
- **[가설]** 읽은 코드에서 추론했으나 시험으로 입증되지 않은 것
- **[미확인]** 이번에 읽지 않았거나 읽었지만 결론이 없는 것

I3~I6 구현은 별도 승인 전까지 착수하지 않는다.

## 1. 출발 기준선과 검증 범위

- 기준선: `research/android-port` `23d146a` (CI 3/3 빌드 성공; CI는 빌드만 확인).
- **호스트 수준에서 시험한 것(Linux, 가짜 장치, 일반·ASan·TSan)**: 레지스트리 상태·카운터, 세션 소유권 이전, 취소 이력, 게시 권한, 정리 이력/현재 분리, `run()` 진입과 `abort_start()` 직렬화.
- **시험하지 않은 것**: 벤더링 비동기 작업의 수명(T5·T12·T13이 기준선에서 그대로 재현됨), Android 실기기 실행, 실제 장치 복원, TSS 통합. 이 기준선은 **호스트 상태 전이 계약의 기준선이지 벤더링 복원 기능의 안전성 확정이 아니다.**
- I10은 보류. `main`은 변경 없음.

## 2. 작업 수명 계약 (정정 반영)

잘못된 순서(`생성 → 등록`)는 쓰지 않는다. 스레드가 시작되자마자 객체에 접근하면 등록 앞에 실행될 수 있기 때문이다.

1. 등록 예약과 작업 소유권 확보 (`try_register`, 잠금 안, 생성 전)
2. 작업 생성 및 실행
3. *분기*: 생성이 실패한 경우에만 `unregister`(실패 기록 + 카운터 감소, 같은 임계구역) — 정상 경로의 단계가 아니다
4. 정상 완료 또는 (필요한 경우만) 취소 요청·처리 — 취소는 모든 작업의 필수 단계가 아니다
5. **실제 작업 종료 확인** (`complete`가 작업의 마지막 동작) — 종료 요청이나 카운터 감소 한 번이 아니라 "작업이 더 이상 객체를 참조하지 않는다"
6. 참조 반환
7. 객체 해제 (엔진만, `CLOSED` 권한 보유 시)

분석에서 가장 먼저 볼 것: **등록 예약과 실제 스레드 실행 시작 사이에 미등록 구간이 있는가** (표 3의 "예약 위치" 열).

## 3. 비동기 작업별 대응표

| # | 작업 | 생성 지점 | 현재 등록 | 실행 주체 | 종료 방식 | 소유자 | 카운터 감소 조건(계획) | I |
|---|---|---|---|---|---|---|---|---|
| A | 비동기 데이터 요청 처리기 | `restore.c:5786` `thread_new(&t, _restore_handle_async_data_request, args)` [확인] | 없음 [확인] | 분리(detach)된 스레드 `restore.c:5152`; `client`와 `message`를 직접 참조 [확인] | 반환까지; join 없음, 완료 통지 없음 [확인] | 현재 없음(분리) | `complete`가 처리기의 마지막 동작이며 `client` 접근이 모두 끝난 뒤 | I3 |
| B | FDR 제어 리스너 스레드 | `restore.c:5541` (`HAVE_REVERSE_PROXY` 아닐 때) [확인] | 없음 | `fdr_listener_thread` | 종료 시 `fdr_disconnect` 후 `thread_join` (`restore.c:5914~5917`) [확인] | `restore_device` | join 완료 후(이미 join은 있음) | I3(등록만) |
| C | FDR 명령 스레드(새 연결마다) | `fdr.c:432` [확인] | 없음 | `fdr_listener_thread` | 생성한 스레드의 join/보관 여부 [미확인] | [미확인] | [미확인] | 범위 확인 필요 |
| D | 리버스 프록시 제어 스레드 | `reverse_proxy.c:601` [확인] | 없음 | `_reverse_proxy_control_thread` | `reverse_proxy_client_free`가 `th_ctrl`을 join [확인 `reverse_proxy.c:~641`] | `reverse_proxy_client` | 제어 스레드 종료 후 | I6 |
| E | 리버스 프록시 연결 스레드 | `reverse_proxy.c:497`, 연결 요청마다 `th_conn`에 덮어씀 [확인] | 없음 | `_reverse_proxy_connection_thread` | 제어 스레드가 **마지막 `th_conn`만** join [확인 `~:518~522`]; 이전 연결 스레드는 join·추적 안 됨 | 제어 스레드(불완전) | 연결 스레드가 `parent` 접근을 끝낸 뒤 | I6 |
| F | usbmuxd 모니터 스레드 | `libusbmuxd.c:1186` [확인] | 없음 | `device_monitor` | [미확인] | [미확인] | 호스트가 사설 usbmuxd로 별도 관리 중(`install.c` Android 분기) | 범위 밖(C13 재확인) |
| G | deviceinterfaced killer 스레드 | `deviceinterfaced.c:288` [확인] | 없음 | `kill_service` | `stop_killer` 플래그 기반 [확인 일부] | 해당 모듈 | [미확인] | 범위 확인 필요 |

비고(그 외 `thread_new`: ostrace, bt_packet_logger, syslog_relay, companion_proxy, notification_proxy, preboard, installation_proxy)는 복원 경로에서 호출되는지 **[미확인]**. 호출되지 않으면 I3~I6 범위 밖이다.

### 3.1 [확인] 기준선 결함과의 대응 지점

- **T5(해제 후 접근)**: 처리기 A는 `client`를 직접 참조한다(`restore.c:5155~5163`). `restore_device`는 루프 종료 후 `restore_client_free(client)`를 부른다(`restore.c:5922`). A에 완료 확인이 없으므로 처리기가 아직 실행 중일 때 해제될 수 있다. 처리기가 `client->async_err`와 `client->flags |= FLAG_QUIT`를 쓰는 줄이 ASan/TSan이 지적한 위치(5161/5162)이다.
- **message 누수(T3)**: 생성 실패 분기(`restore.c:5786~5793`)가 `args`만 free하고 `args->message`(plist_copy)는 free하지 않는다.
- **`async_err`/`flags`의 무동기 접근**: 처리기가 쓰고 루프(`restore.c:5731`, `5907`)가 읽는다. 동기화 수단이 코드에 보이지 않는다 [확인]. TSan이 지적한 T13 계열은 별도.
- **T13/T12(리버스 프록시)**: 연결 요청마다 `th_conn`이 덮어써져 이전 스레드 핸들이 유실되고(`reverse_proxy.c:497`), 연결 스레드가 `client->th_ctrl`과 `parent`를 동기화 없이 읽는다(`reverse_proxy.c:~437` 루프 조건, `_reverse_proxy_control_thread`의 `client->th_ctrl = THREAD_T_NULL`) [확인]. `reverse_proxy_client_free`는 `parent`를 NULL로 바꾼 뒤 join한다 [확인] — 연결 스레드의 `parent` 접근과의 순서는 [가설].

## 4. `global_quit_flag` 접근 표 [확인, 9곳]

> **정정 (2026-11-12, `c4137e3` 소스 대조)**: 아래 표와 "9곳"은 I6-3 적용 전의 상태이고, 일부 줄 번호와 "일반 읽기"는 현재 맞지 않는다. 현재(`research/android-port-integ`) 접근을 종류별로 적는다.
> - 정의 1: `common.c:64` (`int global_quit_flag = 0`, 초기화만), 선언 1: `common.h:143`.
> - 벤더링 읽기 6곳, 모두 `__atomic_load_n(..., RELAXED)`: `download.c:99, 223, 233, 300`, `ipsw.c:1361`, `common.c:292`.
> - 벤더링 쓰기 1곳: `idevicerestore.c:1910` (시그널 핸들러, `__atomic_add_fetch`, SEQ_CST).
> - `restore.c`에는 접근이 **없다**(복원 루프는 `FLAG_QUIT`과 레지스트리 종료 플래그를 본다).
> - 호스트 쓰기 3곳: `install.c:211, 264`(`ST` 매크로), `restore_preflight.c:371`(`__atomic_store_n`, SEQ_CST). `ST`/`LD`는 `host/install.c:36-37`에서 `__atomic_store_n`/`__atomic_load_n`(SEQ_CST)으로 정의돼 있다.
> - 주소 전달 2곳: `install.c:122`, `restore_preflight.c:355`가 `hv_curl_set_cancel_flag((volatile int *)&global_quit_flag)`로 주소를 넘기고, 읽는 곳은 `host/curl_mbedtls.c:106`(`__atomic_load_n`, SEQ_CST)이다.
> - 시험 코드(`tests/test_install_start.c`, `tests/test_download.c`)의 접근은 일반 접근이고 시험 전용이다.
> - 위 분류는 소스 grep 기준이다. 실행 중 실제로 도달하는지는 별개이며, 데이터 경쟁이 없다는 증명이 아니다(TSan이 제품 코드에서 보고하지 않았다는 관측만 있다).

| 위치 | 접근 | 비고 |
|---|---|---|
| `common.c:64` | 정의 `int global_quit_flag = 0` | 원자 아님 |
| `idevicerestore.c:1794` | `global_quit_flag++` (시그널 핸들러 `handle_signal`) | 쓰기, 비원자. 호스트에서는 시그널 경로가 아님 |
| `common.c:292` | 읽기(사용자 입력 루프) | 일반 읽기 |
| `download.c:99, 223, 233, 300` | 읽기 | 일반 읽기; 취소 지점 |
| `ipsw.c:1361` | 읽기 | 일반 읽기 |
| 호스트 `install.c` (Android) | 원자 저장/조회 매크로(`ST`/`LD`) | 호스트 쪽은 원자, 벤더링 쪽은 일반 접근 → **혼재** |

계획(I3): 벤더링 쪽 읽기·쓰기를 단일 원자 접근 규칙으로 통일하되, 시그널 핸들러 경로(`handle_signal`)의 의미는 바꾸지 않는다. 구체 수단(C11 원자 내장 함수 vs 래퍼)은 구현 승인 시 확정. **[미확인]**: 다른 번역 단위에서의 `global_quit_flag` 접근 추가 여부(전체 grep은 위 9곳).

## 5. 연결 등록(I5)과 송신 직렬화(I4)

- `idevice_connect` 정의 `idevice.c:519`, `idevice_disconnect` `idevice.c:600` (`socket_close` at `:615`) [확인]. 등록 지점은 설계 4.6 그대로. 연결 참조 규약(등록 해제 → 참조 반환 대기 → close)은 **미구현**.
- `restored_send`(`libimobiledevice/src/restore.c:160`)는 `property_list_service_send_xml_plist`를 직접 호출하고 뮤텍스가 없다 [확인]. 설계 C10(연결별 뮤텍스)이 필요한 위치. 어떤 스레드가 동시에 `restored_send`를 부르는지(처리기 A 대 루프)는 [가설], T4로 확인 대기.
- `asr.c`의 재시도·송신 루프(연결 `:70`, 수신 `:261`, 송신 `:385` 등) 위치는 확인했으나, 취소 플래그 확인을 넣을 지점의 선정과 영향은 **[미확인]**.

## 6. 계약(C1~C19) → 소스 대응 현황 (분석 단계)

| 계약 | 소스 대응 위치 | 상태 |
|---|---|---|
| C1·C2·C3 (예약→생성→실패 처리) | A: `restore.c:5780~5793`, D: `reverse_proxy.c:601`, E: `reverse_proxy.c:497` | 계획만 |
| C4·C5 (`complete`가 마지막 동작) | A 끝(`restore.c:5164~5168`), D/E 반환 직전 | 계획만 |
| C6·C13 (`CLOSED` 권한 = 엔진) | `restore.c:5907~5924` 종료 구간 | 호스트 쪽은 구현·시험됨, 벤더링 쪽 계획만 |
| C7 (프록시 정리) | `reverse_proxy_client_free` | 계획만 |
| C10 | `libimobiledevice/restore.c:160` | 계획만 |
| C14 | `idevice.c:519, 600` | 계획만 |
| C16 (루프의 종료 플래그 확인) | `restore.c:5731` 루프 조건 | 계획만 |
| C17·C18·C19, C8·C9, C15 | 호스트 레지스트리·세션 | 호스트 구현·시험됨(Linux 가짜 장치) |

## 7. 7개 진입 조건과 현재 상태

| # | 조건 | 상태 |
|---|---|---|
| 1 | 작업 생성 전 등록(생성 실패 시 예약 정리·오류 기록) | **확인 대기** — 레지스트리 연산은 있으나 벤더링 호출 지점 없음 |
| 2 | 종료 확인 전 객체 해제 금지 | **확인 대기** — 처리기 A는 현재 해제 후에도 실행될 수 있음(위 3.1) |
| 3 | 취소 실행과 결과 합성 연결 | **확인 대기** — 호스트 이력은 있음, 벤더링 쪽 취소 실행 없음 |
| 4 | `global_quit_flag` 접근 통일 | **확인 대기** — 벤더링은 일반 접근, 호스트는 원자(위 4) |
| 5 | T5·T12·T13 수정 전후 동일 조건 비교 | **확인 대기** — 기준선 로그는 있음(변경 전) |
| 6 | 호스트 불변조건 재확인(엔진 진입 경로, usbmuxd의 `CLOSED` 범위, 정리 후 `cleanup_resolve` 호출) | **확인 대기** — 아래 8 |
| 7 | 전체 통합 시험(레지스트리 포함 일반·ASan·TSan) | **확인 대기** — I3~I6 이후 |

## 8. 호스트 불변조건 (재확인 대상)

- `abort_start()`의 `hv_net_guard_uninstall()` 실패 불가 전제: (a) 엔진 등록 경로가 `run()` 하나뿐이다, (b) `run()`과 `abort_start()`가 같은 `g_own_mu` 아래에서 직렬화된다. 호스트 시험에서 실패 횟수 0이었으나 관측일 뿐 증명이 아니다. **I3~I6에서 엔진 등록·소유권 경로가 바뀌면 재검증한다.**
- usbmuxd는 아직 `CLOSED` 범위 밖(C13 재확인 대상).
- `hv_reg_cleanup_resolve()`는 호스트 호출처 없음. 처음 사용하는 단계에서 호출 시점이 **실제 자원 해제 성공**과 일치하는지 검증한다. 종료 요청을 보냈다는 사실과 종료가 확인되었다는 사실은 구분한다.

## 9. 구현 단계에서 확정할 미결 사항 (승인 시)

1. 처리기 A에 `try_register`/`complete`를 넣는 정확한 위치와, `thread_new` 실패 시 `args->message` 해제.
2. 처리기 A의 완료 시점: `client` 접근이 끝난 뒤인지(결과 기록 이후).
3. 연결 스레드 E를 다중으로 추적하는 방식(카운터만 vs 핸들 목록); `th_conn` 덮어쓰기 제거.
4. `parent` 접근을 연결 스레드가 끝낸 뒤에만 해제하는 순서.
5. C·F·G의 범위 편입 여부.
6. 취소 지점(`download.c`, `asr.c` 루프)과 레지스트리 종료 플래그의 연결.
7. 벤더링 변경 각각 최소 diff, 후크가 NULL이면 기존 동작 불변(설계 7절 원칙).

## 10. 분석의 한계

- 이 분석은 소스 읽기이며 실행하지 않았다. 표의 [가설]·[미확인]은 구현 승인 후 시험(T1~T28)으로 확정한다.
- 읽지 않은 파일/함수: `fdr.c` 스레드 수명 전체, `libusbmuxd.c` 모니터 스레드, `asr.c` 루프 세부, 복원 경로에서 쓰이지 않을 가능성이 있는 libimobiledevice 서비스 스레드.
- Android 실기기와 실제 복원은 이 문서 어디에서도 검증되지 않았다.
