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
| F libusbmuxd 장치 모니터 스레드 (`libusbmuxd.c:1186`) | **제외 (레지스트리 등록 안 함)**, 단 **불변조건 기록** | `idevicerestore_start`가 `idevice_event_subscribe(idevice_event_cb, client)`로 시작(`idevicerestore.c:405~408`), `idevicerestore_client_free`가 `idevice_event_unsubscribe()`로 취소+join(`idevicerestore.c:1670~1672`; `libusbmuxd.c` 구독 해제는 마지막 구독 해제 시 `thread_cancel`+`thread_join`) [확인]. 호스트는 `idevicerestore_client_free`를 `CLOSED` 해제 허용 뒤에만 부르고(`install.c:147~148`) 그 뒤에 사설 usbmuxd를 멈춘다(`:149`) [확인]. 즉 모니터 스레드는 엔진 해제 경로 안에서 join되는 구조. **조건**: (a) `QUIESCENCE_FAILED`에서 호스트가 `client_free`를 건너뛰면 클라이언트도 해제되지 않으므로 콜백의 `client` 참조는 유효 [가설], (b) `thread_cancel` 실패 시 `running=0` 경로(`libusbmuxd.c` 구독 해제부)가 join을 건너뛰는지는 **[미확인]**, (c) 실제 시험은 없음 |
| G deviceinterfaced killer (`deviceinterfaced.c:288`) | **제외** | `#ifdef __APPLE__`이고 CLI `main`의 `--exclusive-usb` 경로(`idevicerestore.c:2113~2130`)에서만 시작·정지됨 [확인]. 호스트는 `idevicerestore_start`를 직접 호출(`install.c`) [확인]. Android에서는 컴파일 제외. iOS 대상에서 `main`이 호출되지 않는다는 것은 [가설] |
| libimobiledevice 서비스 스레드(ostrace, syslog_relay, companion_proxy, notification_proxy, preboard, installation_proxy, bt_packet_logger) | **제외 (복원 경로 호출 미확인)** | 복원 경로의 호출 여부는 **[미확인]**. 구현 단계에서 호출 그래프로 확인하고, 호출되면 이 표를 갱신 |

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
| I6-3 | `global_quit_flag` 9곳 | 벤더링 쪽 접근 규칙 통일(호스트는 원자 접근) | TSan |

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

- 호스트 `hv_install_cancel()`(`install.c:253~258`)은 `global_quit_flag`만 올린다. 복원 루프(`restore.c:5731`)는 이 값을 읽지 않으므로 **I3 없이는 호스트 취소가 복원 루프를 끝내지 못한다.** 취소는 다운로드·ipsw 경로에만 효과가 있다. 호스트의 취소 이력(`ABORTED_BY_USER` 합성)은 "요청이 수락되었다"는 사실이지 엔진이 멈췄다는 뜻이 아니다.
- 시그널 핸들러(`idevicerestore.c:1790~1796`)는 `FLAG_QUIT`과 `global_quit_flag`를 **둘 다** 올린다. 호스트 취소는 하나만 올리므로 의미가 다르다.
- 루프가 수신 대기에서 깨어나는 주기: `restored_receive` → `property_list_service_receive_plist`는 **30000 ms**(`property_list_service.c:~272`), 타임아웃이면 루프는 `continue`로 맨 앞에서 `FLAG_QUIT`를 다시 본다(`restore.c:5738~5742`, `HAVE_RESTORE_E_RECEIVE_TIMEOUT` 정의됨 `config.h:13`) [확인]. 유휴 상태에서 플래그가 올라가도 루프가 관측하기까지 **최대 약 30초**.
- ASR: 수신 `asr_receive`→`idevice_connection_receive`(`asr.c:147`)는 취소 확인 없는 차단 호출이고, 검증 루프 `asr.c:261`은 수신 오류에서만 빠짐, 송신 루프 `:385`는 재시도 횟수 기반 [확인]. 종료 플래그를 보지 않음.
- 리버스 프록시: 제어 루프(1000 ms 타임아웃, 조건 `client->parent && running`), 연결 루프(조건 `client->th_ctrl != NULL`), 프록시 명령 처리의 `while (1)`(100 ms 타임아웃 `reverse_proxy.c:~232`)은 종료 플래그가 아니라 연결 종료·오류·`parent` 변경으로만 빠진다 [확인]. 외부 호스트 `socket_connect`(`:191`)가 시간 제한 없이 차단되는지는 **[가설]**.

### 4.3 결정과 미결

| 번호 | 결정/미결 | 상태 |
|---|---|---|
| 6-1 | I3-3: 루프 맨 앞에서 **레지스트리 종료 플래그**(원자)를 읽는다. 호스트 `request_cancel`/`global_quit_flag`와의 연결은 레지스트리가 담당(C16). `global_quit_flag` 단독 읽기 추가만으로는 취소 계약이 완성되지 않음 | 원칙 확정, 구현 대기 |
| 6-2 | 유휴 시 관측 지연(최대 약 30초): (a) 허용하고 D2 한도에 반영, 또는 (b) 복원 루프의 수신 타임아웃을 줄이는 변경(`libimobiledevice/src/restore.c`의 `restored_receive`가 `property_list_service_receive_plist_with_timeout`을 쓰게 함, 벤더링 변경 추가) | **사용자 결정 필요** |
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

## 6. 한계

- 읽기 전용 분석이다. [가설]과 [미확인]은 구현 승인 뒤 시험으로 확정한다.
- 읽지 않은 곳: `libusbmuxd.c` 구독 해제의 `thread_cancel` 실패 경로 세부, 프록시 연결 스레드가 `parent`를 NULL 이후 접근하는지, 복원 경로에서 쓰일 수 있는 libimobiledevice 서비스 스레드.
- 6-2는 사용자 결정이 필요하다(관측 지연 허용 vs 벤더링 변경 추가).
- Android 실기기, 실제 복원, TSS 통합은 검증되지 않았다.
