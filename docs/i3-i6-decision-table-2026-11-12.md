# I3~I6 최종 결정표 (5·6번 소스 확인 반영) (2026-11-12)

성격: 읽기 전용 소스 분석의 결과와 구현 계획. **벤더링 소스·I10·`main`은 변경하지 않았고, I3~I6 구현은 착수하지 않았다.** 이 표는 구현 승인이 아니다. 표시: **[확인]** 이번에 소스를 읽어 확인(파일:줄), **[가설]** 추론이며 시험으로 입증되지 않음, **[미확인]** 읽지 않았거나 결론 없음. 앞 문서: `docs/i3-i6-entry-analysis-2026-11-12.md`(작업 대응표, 7개 진입 조건).

기준선 `23d146a` = 호스트 상태 전이 계약의 기준선. 벤더링 복원 기능의 안전성은 확정되지 않았다(T5·T12·T13 기준선에서 재현, Android 실기기 미검증).

## 1. 확정 원칙 (구현 위치는 §3)

1. 처리기 A: `thread_new` 이전에 등록 예약. 생성 실패 시 예약과 `args->message`까지 정리.
2. 처리기 A 완료: `client`·`message` 등 공유 객체 접근이 모두 끝난 뒤 `complete`(마지막 동작). 완료 통지와 실제 스레드 종료 확인은 구분.
3. 연결 스레드 E: 연결별 핸들과 종료 상태 관리. `th_conn` 덮어쓰기 제거. 카운터만으로 대체하지 않음.
4. `parent` 해제: 관련 스레드가 종료되고 참조가 반환된 뒤.
7. 최소 diff. 후크가 NULL이면 기존 동작 불변(시험으로 확인).

## 2. 5번: 작업 편입 결정

판단 기준: 복원 세션이 소유한 객체를 참조해 세션 수명에 영향을 주는가, 독립적인 종료·참조 계약이 **실제 호출 순서로** 확인되는가.

| 작업 | 결정 | 근거 |
|---|---|---|
| A 비동기 데이터 요청 처리기 | **편입 (I3)** | `restore.c:5786` 생성, `:5152` 처리기가 `client`·`message`를 직접 참조, 분리(detach)되어 종료 확인 수단 없음 [확인] |
| D 리버스 프록시 제어 스레드 | **편입 (I6)** | `reverse_proxy.c:601` 생성, 종료는 `client_free`의 join뿐 [확인] |
| E 리버스 프록시 연결 스레드 | **편입 (I6)** | `reverse_proxy.c:497`, `th_conn`을 덮어써 마지막 하나만 join [확인] |
| B FDR 제어 리스너 (`restore.c:5541`) | **제외** | `third_party/restore_config/config.h:12`가 `HAVE_REVERSE_PROXY`를 정의하고, 이 설정을 쓰는 빌드(`-DHAVE_CONFIG_H -I$TP/restore_config`; bmprobe/ios/check-no-test-flags/download-test 등)에서 `restore.c:5414` `#ifndef` 구간과 `:5911` FDR 분기는 컴파일되지 않음 [확인]. 선택된 설정은 리버스 프록시 경로 |
| C FDR 명령 스레드 (`fdr.c:432`) | **제외 (B에 종속)** | `fdr_poll_and_handle_message`/`fdr_handle_sync_cmd`는 `fdr_listener_thread`(`fdr.c:181~188`)에서만 도달하고, 리스너 생성은 컴파일 제외된 B뿐 [확인: fdr.c 내 호출 위치]. **재평가 조건**: 설정이 바뀌어 FDR 경로가 켜지면 다시 편입 검토. 참고(그 경우에만 해당): `fdr_handle_sync_cmd`의 지역 `THREAD_T`(`fdr.c:416,432`)는 join·보관되지 않음 [확인] |
| F libusbmuxd 장치 모니터 스레드 (`libusbmuxd.c:1186`) | **제외 (레지스트리 등록 안 함)**, 단 **불변조건 기록** | `idevicerestore_start`가 `idevice_event_subscribe(idevice_event_cb, client)`로 시작(`idevicerestore.c:405~408`), `idevicerestore_client_free`가 `idevice_event_unsubscribe()`로 취소+join(`idevicerestore.c:1670~1672`; `libusbmuxd.c` 구독 해제는 마지막 구독 해제 시 `thread_cancel`+`thread_join`) [확인]. 호스트는 `idevicerestore_client_free`를 `CLOSED` 해제 허용 뒤에만 부르고(`install.c:147~148`) 그 뒤에 사설 usbmuxd를 멈춘다(`:149`) [확인]. 즉 모니터 스레드는 엔진 해제 경로 안에서 join되는 구조. **조건부 제외**: 세부 분석은 §7, 1단계 시험 결과는 `docs/stage1-evidence-2026-11-12.md`(S4: Linux에서 `shutdown(listenfd)`로 모니터가 즉시 종료·join 반환; S4j: join 오류 주입 시 `thread_free`가 살아 있는 스레드에 호출됨). 요약: `thread_cancel` 실패는 join을 건너뛰지 않지만, 이 빌드에서는 `thread_cancel`이 항상 실패해 종료가 `socket_shutdown(listenfd)`의 차단 해제 효과(V5)에 의존한다. 실패 시 형태는 UAF가 아니라 `client_free` 안의 hang으로 보이나 [가설], 시험은 없음 |
| G deviceinterfaced killer (`deviceinterfaced.c:288`) | **제외** | `#ifdef __APPLE__`이고 CLI `main`의 `--exclusive-usb` 경로(`idevicerestore.c:2113~2130`)에서만 시작·정지됨 [확인]. 호스트는 `idevicerestore_start`를 직접 호출(`install.c`) [확인]. Android에서는 컴파일 제외. iOS 대상에서 `main`이 호출되지 않는다는 것은 [가설] |
| libimobiledevice 서비스 스레드(ostrace, syslog_relay, companion_proxy, notification_proxy, preboard, installation_proxy, bt_packet_logger) | **잠정 제외** (§8: 검색 범위 안에서 스레드를 만드는 호출은 `usbmuxd` 쪽 `np_set_notify_callback` 1곳뿐이고 호스트가 `-p`로 막음) | 호출이 **발견되지 않았다**는 것이지 도달 불가능이 증명된 것은 아님. §8 |

호스트의 사설 usbmuxd 스레드는 이 표 범위 밖이다(호스트가 별도 관리; C13 재확인 항목).

## 3. 변경 대상과 위치 (구현 승인 시)

| 항목 | 파일·함수·호출 지점 | 내용 | 종료 조건(확인 방법) |
|---|---|---|---|
| I3-1 | `restore.c:5780~5793` (`AsyncDataRequestMsg` 분기) | `try_register` → 실패면 `thread_new`를 부르지 않음. `thread_new` 실패 시 `unregister`와 `args->message` 해제(`plist_free`) | 생성 실패 후 카운터 0, 누수 없음(T3) |
| I3-2 | `restore.c:5152~5168` (처리기) | `client->async_err`·`flags` 쓰기 이후 `plist_free(message)`까지 끝낸 뒤 `complete`(마지막 동작); `client` 접근은 그 앞에서 끝냄 | 해제 시점에 처리기가 `client`를 참조하지 않음(T5) |
| I3-3 | `restore.c:5731` 루프, `:5907~5924` 종료 구간 | 반복 맨 앞에서 레지스트리 종료 플래그(원자) 확인; 종료 순서 4.3 적용(해제 허용 조회 포함) | T5, T6, T9, T17 (설계 8절 대응표) |
| I3-4 | `async_err`/`flags` | 처리기(`:5161~5162`)와 루프(`:5731`, `:5907`)의 동기화 | TSan(`restore.c:5161/5162`) 지적 해소 |
| I4 | `libimobiledevice/src/restore.c:160` `restored_send` | 연결별 뮤텍스(C10) | T4 |
| I5 | `idevice.c:519` `idevice_connect`, `:600` `idevice_disconnect`(`socket_close` `:615`) | 연결 등록·참조 규약(C14); 닫기 전 참조 반환 대기 | T15, T8, T18 |
| I6-1 | `reverse_proxy.c:497` | `th_conn` 덮어쓰기 제거; 연결별 핸들·종료 상태 | T13 |
| I6-2 | `reverse_proxy.c:601`, `:~634~648` `client_free` | 제어·연결 스레드 등록·완료; 종료 요청/완료 확인/해제 분리; `parent`는 연결 스레드 종료 확인 뒤 해제 | T12, T13 |
| I6-3 | `global_quit_flag` 9곳 (현재 접근 수는 `docs/i3-i6-entry-analysis-2026-11-12.md` §4의 정정 참조; `restore.c`에는 없고 원자 접근은 `download.c`, `ipsw.c`, `common.c`, `idevicerestore.c`) | 벤더링 쪽 접근 규칙 통일(호스트는 원자 접근) | TSan |

그 밖의 파일(`fdr.c`, `libusbmuxd.c`, `deviceinterfaced.c`)은 위 §2 결정에 따라 **변경 대상에서 제외**한다.

## 4. 6번: 취소·차단 호출 확인 결과

취소 요청 → 플래그 관측 → 차단 함수 반환 → 실제 작업 종료는 별개이며, 객체는 마지막 단계(실제 종료) 이후에만 해제한다. 타임아웃이나 취소 뒤에도 내부 스레드가 실행 중이면 해제하지 않는다.

### 4.1 [확인] 플래그를 누가 읽는가

| 위치 | 읽는 값 | 효과 |
|---|---|---|
| `restore.c:5731` 복원 루프 | `client->flags & FLAG_QUIT` 뿐 | **`global_quit_flag`를 읽지 않음** |
| `asr.c` 전체 | 없음 | 종료 플래그 확인 없음 |
| `download.c:99, 223, 233, 300` | `global_quit_flag` | curl 진행 콜백이 1을 반환하면 전송 중단. 호스트는 `hv_curl_set_cancel_flag(&global_quit_flag)`(`install.c:122`)로 같은 플래그를 취소 관측에 연결 |
| `ipsw.c:1361`, `common.c:292` | `global_quit_flag` | 다운로드 뒤·입력 루프 |

### 4.2 [확인] 핵심 발견

- **정정(2026-11-12 검토 반영)**: 이 표의 이전 판은 "호스트 취소는 `global_quit_flag`만 올린다"고 적었으나 틀렸다. 실제 순서는 `hv_install_cancel()`(`install.c:253~259`) → `hv_restore_session_cancel()`(`restore_session.c:161~168`)이 취소 이력을 기록(`hv_reg_cancel_note`)하고 수락되면(`==1`) **레지스트리 종료 플래그를 올리고**(`hv_reg_request_terminate`) → 그 뒤 `global_quit_flag`를 올린다 [확인]. 즉 호스트는 두 플래그를 모두 올린다.
- 실제 결함은 설정 누락이 아니라 **벤더링 복원 루프(`restore.c:5731`)가 레지스트리 종료 플래그도, `global_quit_flag`도 읽지 않는다**는 점이다(루프는 `FLAG_QUIT`만 읽음). 그래서 I3 없이는 호스트 취소가 복원 루프를 끝내지 못하고, 취소가 닿는 곳은 다운로드·ipsw 경로뿐이다. 호스트의 취소 이력(`ABORTED_BY_USER` 합성)은 "요청이 수락되었다"는 사실이지 엔진이 멈췄다는 뜻이 아니다. **I3-3은 새 취소 상태를 만드는 것이 아니라 기존 레지스트리 종료 상태를 복원 루프에 연결하는 작업이다.**
- 시그널 핸들러(`idevicerestore.c:1790~1796`)는 `FLAG_QUIT`과 `global_quit_flag`를 **둘 다** 올린다. 호스트 취소는 `FLAG_QUIT`을 올리지 않는다(`client`에 접근하지 않는 설계, C16).
- 루프가 수신 대기에서 깨어나는 주기: `restored_receive` → `property_list_service_receive_plist`는 **30000 ms**(`property_list_service.c:~272`), 타임아웃이면 루프는 `continue`로 맨 앞에서 `FLAG_QUIT`를 다시 본다(`restore.c:5738~5742`, `HAVE_RESTORE_E_RECEIVE_TIMEOUT` 정의됨 `config.h:13`) [확인]. 유휴 상태에서 플래그가 올라가도 루프가 관측하기까지 **최대 약 30초**.
- ASR: 수신 `asr_receive`→`idevice_connection_receive`(`asr.c:147`)는 취소 확인 없는 차단 호출이고, 검증 루프 `asr.c:261`은 수신 오류에서만 빠짐, 송신 루프 `:385`는 재시도 횟수 기반 [확인]. 종료 플래그를 보지 않음.
- 리버스 프록시: 제어 루프(1000 ms 타임아웃, 조건 `client->parent && running`), 연결 루프(조건 `client->th_ctrl != NULL`), 프록시 명령 처리의 `while (1)`(100 ms 타임아웃 `reverse_proxy.c:~232`)은 종료 플래그가 아니라 연결 종료·오류·`parent` 변경으로만 빠진다 [확인]. 외부 호스트 `socket_connect`(`:191`)가 시간 제한 없이 차단되는지는 **[가설]**.

### 4.3 결정과 미결

| 번호 | 결정/미결 | 상태 |
|---|---|---|
| 6-1 | I3-3: 루프 맨 앞에서 **레지스트리 종료 플래그**(원자)를 읽는다(호스트는 이미 이 플래그를 올림). `global_quit_flag` 단독 읽기 추가만으로는 취소 계약이 완성되지 않음 | 원칙 확정, 구현 대기 |
| 6-2 | **결정(검토 반영): 호스트 통합 경로에서만 복원 루프의 수신 타임아웃을 단축한다.** 후크가 NULL인 기존 경로의 30초는 바꾸지 않는다(호환성, 원칙 7). 초기 시험값 **1000 ms**, 최종값은 측정으로 확정(1초가 최적이라는 근거는 없음). 방법: `restored_receive`가 `property_list_service_receive_plist_with_timeout`을 쓰는 변형을 추가하고 루프(`restore.c:5736`)가 호스트 후크가 있을 때만 사용(벤더링 변경, I3/I4 범위). 호스트의 15초 닫기 상수는 변경하지 않는다(`close()`는 엔진이 반환된 뒤 호출되므로 엔진 수신 지연과 레지스트리 종료 대기는 다른 구간). **한계**: 30초를 1초로 줄여도 전체 취소 시간이 1초로 보장되지 않는다(ASR·프록시 차단 호출은 별도). 측정 항목은 §6 | 결정됨(값은 측정 후) |
| 6-3 | ASR 송수신 루프에 종료 확인을 넣을지, 연결 `shutdown`(C14/4.6)으로 차단 해제를 유도할지 | **미결**, `shutdown`의 대기 해제 효과는 Linux에서도 미검증(V5) |
| 6-4 | 프록시 외부 소켓 연결·수신 루프의 종료 유도(참조 등록 후 `shutdown`) | **미결**, 위 6-3과 동일 조건 |
| 6-5 | 차단 호출이 반환하지 않을 때: 객체 해제 금지, `QUIESCENCE_FAILED`(기존 설계 7b) | 확정(호스트 구현·시험됨) |

## 5. 검증 대응

| 대상 | 시험 | 비고 |
|---|---|---|
| A 해제 후 접근 | T5 (ASan: `restore.c:5161/5162` 기준선 재현) | 수정 후 같은 조건 비교 |
| A 생성 실패·누수 | T3 | 기준선 시험 후크 존재 |
| `async_err`/`flags` | TSan | 기준선 지적 위치 |
| `restored_send` 동시 호출 | T4 | 동시 호출 존재 여부는 [가설] |
| D/E 스레드 수명 | T12, T13 | 기준선 재현 가능 |
| 연결 등록·참조 | T15, T8, T18 | |
| 취소 | T14, T17, T20, T27 | 6-2~6-4 결정 후 구체화 |
| NULL 후크 | 후크 NULL 상태에서 기존 가짜 장치 회귀와 반환값 동일 | 원칙 7 |
| 호스트 불변조건 | `engine_enter()` 경로 불변(`run()` 한 곳), usbmuxd `CLOSED` 범위, `cleanup_resolve` 호출 시점 | I3~I6 뒤 재확인 |

## 6. 6-2 측정 항목과 위험 (시험 전에는 어떤 값도 확정하지 않는다)

- 타임아웃 처리 [확인]: 루프는 `RESTORE_E_RECEIVE_TIMEOUT`을 오류가 아니라 `continue`로 처리하고(`restore.c:5738~5742`, 디버그 로그만) 오류 누적이 없다.
- 프레이밍 [확인+가설]: `internal_plist_receive_timeout`(`property_list_service.c:183~`)은 **처음 4바이트 길이 읽기에만** 지정 타임아웃을 쓰고(`:194`), 본문은 별도 `service_receive`로 읽는다(`:219`). 0바이트면 타임아웃으로 반환(`:200~203`)하므로 프레임을 소비하지 않는다. 그러나 길이 4바이트 중 1~3바이트만 읽힌 경우를 확인하는 코드가 없다(`bytes != 4` 검사 없음) [확인]. **1단계 시험 결과(2026-11-12, `docs/stage1-evidence-2026-11-12.md`)로 갱신: 이 틈은 실제 결함(프레임 유실·30초 정지)이지만 타임아웃 길이와 무관하다** — 길이가 쪼개져 도착하면 바이트가 하나라도 읽히는 즉시 반환하므로 타임아웃 값은 영향을 주지 않는다. 즉 타임아웃 단축이 이 결함을 만들거나 키우지 않는다(기존 30초 경로에도 있는 기존 결함).
- 측정: ① 정상 복원 시 오류·재시도 동작 변화, ② 유휴 상태 CPU 사용량, ③ 취소 요청부터 루프 탈출까지 시간, ④ 길이 접두 분할 전송 시 프레임 손상 여부, ⑤ 후크 NULL 경로 동작 불변.
- 호스트 통합 경로 한정: 변경은 호스트 후크가 설정된 경우에만 적용한다.

## 7. libusbmuxd 장치 모니터(F)의 취소·종료 경로 [확인, 읽기 전용]

`idevice_event_unsubscribe()` → `usbmuxd_events_unsubscribe()`(`libusbmuxd.c:1209~1245`):
1. 마지막 구독자가 빠지면 `cancelling = 1; socket_shutdown(listenfd, SHUT_RDWR)`.
2. `thread_alive(devmon)`(`pthread_kill(t, 0) == 0`)이면 `thread_cancel(devmon)`을 호출. **`thread_cancel`이 실패(<0)하면 `running = 0`** — join을 건너뛰지 않는다.
3. 이어서 **`thread_join(devmon)`을 항상 호출**하고(스레드가 살아 있을 때), 그 결과와 무관하게 `thread_free(devmon); devmon = NULL`.
4. 반환값: join 결과가 0도 `ESRCH`도 아니면 그 값을 반환하지만, 호출자 `idevice_events_unsubscribe`는 오류를 받아도 `IDEVICE_E_UNKNOWN_ERROR`를 반환하는 것뿐이고(`idevice.c:270~286`) `idevicerestore_client_free`(`idevicerestore.c:1670~1672`)는 반환값을 확인하지 않는다.

이 빌드의 특이사항 [확인]: `HAVE_PTHREAD_CANCEL`은 `third_party/restore_config/config.h`에도 `tools/*.sh`의 컴파일 옵션에도 정의되어 있지 않다 → `thread_cancel()`은 `-1`을 반환하는 분기만 컴파일된다(`libimobiledevice-glue/src/thread.c:83~94`). 따라서 이 설정에서는 **항상 `running = 0` 경로**이며 모니터 스레드의 종료는 `socket_shutdown(listenfd)`로 `get_next_event` 대기가 풀리는지에 달려 있다(`libusbmuxd.c:1135~1147`).

판정:
- `thread_cancel` 실패 자체가 join 생략으로 이어지지는 않는다. 구독 해제 함수가 정상 반환하면(join 성공) 모니터 스레드는 이미 종료되어 있고 콜백은 더 이상 `client`를 참조하지 않는다.
- 남는 위험 둘: (1) `shutdown`이 차단 수신을 풀지 못하면 `join`이 반환하지 않아 `idevicerestore_client_free`(호스트의 해제 경로)가 멈춘다 — UAF가 아니라 **hang**이며, 호스트 쪽에서는 게시·`IDLE` 전환이 일어나지 않아 새 세션이 거부되는 안전 측 [가설]. (2) `pthread_join`이 오류를 반환하면(예: 이미 종료·EDEADLK 등) 스레드가 살아 있는 채로 `thread_free`/`devmon = NULL`이 실행될 수 있다 [가설, 발생 조건 미확인].
- `running`, `cancelling`, `listenfd`는 보호 없는 전역이다(`libusbmuxd.c:127~128`, 다른 스레드에서 읽고 씀) [확인]. 기준선 TSan이 `libusbmuxd.c:1546`에서 경합을 보고한 것과 같은 계열이며 I3~I6 범위가 아니다.
- **F 제외 결정은 조건부로 유지**한다. 조건: 구독 해제 실패·지연 경로(`shutdown`이 차단을 풀지 못하는 경우, join 오류)를 I3~I6 통합 시험 대상에 포함한다. 종료 보장은 시험 전에는 문서에 쓰지 않는다.
- 호스트의 사설 usbmuxd(`usbmuxd_main`)는 이 모니터와 별개의 스레드이고, 호스트가 `client_free` **뒤에** 정지한다(`install.c:148~149`) [확인].

## 8. 서비스 스레드 7종의 호출 그래프 [확인, 검색 범위 명시]

검색 범위: 저장소 전체의 `*.c *.h *.m`에서 스레드를 만드는 공개 함수(`ostrace_start_activity`, `bt_packet_logger_start_capture`, `syslog_relay_start_capture(_raw)`, `companion_proxy_start_listening_for_devices`, `np_set_notify_callback`, `instproxy_*`, `preboard_create/commit_stashbag`)의 호출을 찾았다(해당 서비스 자신의 소스·헤더와 `libimobiledevice/tools` 제외). 모든 `libimobiledevice/src/*.c`가 빌드에 포함된다(`tools/restore-selftest-build.sh:26`)는 점도 확인했다.

| 서비스 | 스레드 생성 함수 | 호출 발견 | 판정 |
|---|---|---|---|
| ostrace | `ostrace_start_activity` | 없음 | 미도달(발견 안 됨) |
| bt_packet_logger | `bt_packet_logger_start_capture` | 없음 | 미도달(발견 안 됨) |
| syslog_relay | `syslog_relay_start_capture(_raw)` | 없음 | 미도달(발견 안 됨) |
| companion_proxy | `companion_proxy_start_listening_for_devices` | 없음 | 미도달(발견 안 됨) |
| installation_proxy | `instproxy_*` 상태 루프 | 없음 | 미도달(발견 안 됨) |
| preboard | `preboard_receive_status_loop_with_callback` | `normal.c:469, 598`에서 `status_cb == NULL`로 호출 → `preboard.c:228`에서 스레드 없이 반환 [확인] | 스레드 미생성(콜백이 NULL인 동안) |
| notification_proxy | `np_set_notify_callback` | **`third_party/usbmuxd/src/preflight.c:292`** 1곳 | 아래 |

- `usbmuxd` 프리플라이트의 `np` 알림 스레드: `preflight.c:292`가 장치 추가 시 insecure_notification_proxy에 `np_set_notify_callback`을 걸어 스레드를 만든다. 호스트는 사설 usbmuxd를 `-p`(`--no-preflight`)로 띄우며(`install.c:52,55`) `preflight.c:380`이 `no_preflight == 1`이면 프리플라이트를 건너뛴다 [확인]. **1단계에서 더 강한 사실 확인**: `np_*`를 쓰는 본문 전체가 `#ifdef HAVE_LIBIMOBILEDEVICE`(`preflight.c:31~`) 안에 있고 이 매크로는 `restore_config/config.h`에도 빌드 스크립트에도 정의되어 있지 않아 **이 빌드에서는 컴파일되지 않으며** `imd_notification_proxy.o`는 호스트 링크 닫힘에 포함되지 않는다(링크 추적). **조건부 제외**: 호스트가 `-p`를 유지하는 동안만 성립하며, `-p`를 빼면 이 스레드가 사설 usbmuxd 쪽에 생기고 호스트 소유 수명(C13의 usbmuxd 범위)이 되므로 다시 편입을 검토한다.
- 호출을 찾지 못한 6종은 **"발견되지 않음"이지 도달 불가능의 증명이 아니다.** 검색은 정적 문자열 호출만 본다(함수 포인터·매크로·다른 확장자·빌드 목록 밖의 파일은 보지 않았다). 구현 단계에서 링크 결과(심볼 참조)로 한 번 더 확인하고, 도달이 확인되는 스레드는 소유권과 참조 객체를 분석해 편입 여부를 다시 정한다. 그때까지 상태는 **잠정 제외**.
- 위 7종 외에 `normal.c`(preboard)가 장치 정상 모드 경로에서만 쓰이는지, Henge의 가상 장치가 그 경로에 들어가는지는 **[미확인]**.

## 9. Linux `shutdown` 차단 해제 시험 설계 (작성만, 코드·실행 없음)

목적: 6-3(ASR), 6-4(프록시)에서 연결 `shutdown`을 취소 수단으로 쓸 수 있는지 **시험 전에 결정하지 않기 위해** 판정 조건을 먼저 정한다. 시험 결과 전에는 어떤 종료 보장도 문서화하지 않는다.

대상 차단 호출을 호출 종류로 나눠 각각 별도 시험한다(동작 조건이 다르다).

| 시험 | 차단 상태 | 취소 동작 | 판정 항목 |
|---|---|---|---|
| S1 | 연결된 TCP 소켓의 `recv`(예: `asr_receive`→`idevice_connection_receive`, 프록시 `socket_receive_timeout`) | 다른 스레드가 `shutdown(SHUT_RDWR)` | 반환 여부·반환 코드·소요 시간 |
| S2 | 연결된 소켓의 `send`가 상대의 수신 지연으로 막힘(프록시 송신 루프, ASR 송신) | `shutdown` | 반환 여부·부분 송신 처리(`sent != bytes`) |
| S3 | `connect` 진행 중(`socket_connect`가 차단되는 경우(라이브러리에 `CONNECT_TIMEOUT` 5000 ms가 있다. 결함은 시간 제한이 아니라 취소할 수 없고 연결 전 fd가 공개되지 않는다는 점); 응답 없는 가짜 호스트) | `shutdown`(소켓이 아직 연결 전) | 반환 여부 — 연결 전 소켓에는 효과가 다를 수 있어 별도 판정 |
| S4 | 로컬 사설 usbmuxd 수신(F의 `get_next_event`) | `socket_shutdown(listenfd)` | 모니터 스레드 종료와 `join` 반환(§7) |
| S5 | `poll/select` 대기(`socket_receive_timeout`) | `shutdown` | 타임아웃 전에 반환하는지 |

공통 확인:
1. 차단 해제 후 **스레드 종료**(join 반환)까지 걸린 시간과 반환 코드.
2. **참조 반환**: 취소 측이 확보한 fd 참조가 반환된 뒤에만 소유 측이 `close`하는지(C14). `shutdown` 직후 다른 스레드가 같은 fd를 쓰는 중이면 즉시 `close`하지 않는다. fd 재사용 경쟁(닫힌 번호를 다른 열기가 재사용)을 의도적으로 만든다.
3. **오류 전파**: 차단 호출의 오류 반환이 호출 경로 위쪽(`asr_*`, `restore_*`, 프록시 스레드)에서 오류로 전달되어 작업이 종료되는지, 아니면 재시도 루프가 되살아나는지(`asr.c:385`의 `retry`, ASR 연결 재시도 `asr.c:70`).
4. 부정 대조: `shutdown`을 하지 않으면 차단이 유지되는지(시험이 의미 있는지).
5. 환경: Linux에서만. **Android는 보장하지 않는다**(V5). Linux 결과를 Android 보장으로 쓰지 않는다.

사용 도구(구현 승인 뒤): 기존 가짜 장치 하네스(`tests/fake_device/`)의 응답 지연·무응답 모드, `--wrap`/링크 대체. 시험 코드는 이 문서의 범위가 아니다.

판정 후 갱신: S1~S5 결과로 6-3·6-4의 방식(`shutdown` 유도 / 루프 내 플래그 확인 / 둘 다)을 확정한다. `shutdown`이 풀지 못하는 호출은 해당 작업을 "종료 불가"로 두고 `QUIESCENCE_FAILED` 경로(기존 7b)로 수렴시킨다.

## 10. 한계

- 읽기 전용 분석이다. [가설]과 [미확인]은 구현 승인 뒤 시험으로 확정한다.
- 읽지 않은 곳: 프록시 연결 스레드가 `parent`를 NULL 이후 접근하는지, `pthread_join` 오류 발생 조건, 서비스 스레드의 링크 결과 확인, 정상 모드(`normal.c` preboard) 경로 진입 여부.
- 6-2의 값(1000 ms)은 시험용 초기값이며 측정 전 확정이 아니다. §6 측정 항목이 끝나기 전에는 적용값으로 쓰지 않는다.
- §7~§8의 [가설]은 시험·링크 확인 전까지 확정하지 않는다.
- Android 실기기, 실제 복원, TSS 통합은 검증되지 않았다.
