# 가짜 장치 시험 구현 범위 설계 (T2·T5·T12·T13)

상태: **설계안 (구현 미승인).** 이 문서는 시험 코드 작성의 범위와 방식을 정할 뿐 코드를 포함하지 않는다. 확정된 통합 설계(`docs/restore-lifetime-integrated-design.md` v2.2, D1~D8, C1~C19)와 시험 명세(`docs/fake-device-test-spec.md`)를 바꾸지 않는다. 기준선 관측은 `docs/baseline-observations-2026-11-11.md`.

표기: **[확인]** 소스 읽기로 확인 / **[설계]** 이 문서의 제안 / **[미정]** 구현 착수 전에 소스를 더 읽어야 정해짐 / **[미검증]** 실행 확인 없음.

## 1. 목적과 비목표

- 목적: 변경 전 코드(기준선)와 I1~I7 이후 코드에서 같은 입력으로 **같은 관측**을 하도록 하는 가짜 장치(복원 모드 기기 흉내)를 만든다. 우선 대상은 T2(오류 보존·전파), T5(응답 정지와 대기 한도), T12(열린 프록시 연결이 있는 종료), T13(연달아 오는 연결 요청).
- 비목표: 실제 Apple 서비스(TSS 포함) 호출, 서명 이미지·펌웨어 전송, IPSW 사용, 복원 프로토콜의 충실한 모사, Android 검증. 가짜 장치의 통과는 **일반 안전성도 실기 안전성도 뜻하지 않는다.**
- 안전 제약 준수: 모든 연결은 loopback 한정(기존 `host/net_guard.c` 정책이 그대로 적용), 가짜 ECID/UDID 값만 사용, HTTPS 인증서 검증·정책 훅은 건드리지 않음.

## 2. 소스 전제 확인 [확인]

| 번호 | 사실 | 근거 |
| --- | --- | --- |
| F1 | `restore_device(client, build_identity)`는 `idevicerestore_start` 밖에서 직접 호출 가능한 비정적 함수다. 필요한 전제는 `client->ecid != 0`, `client->udid`, usbmuxd에서 그 UDID가 보이는 장치, 그 장치의 restored 서비스가 `QueryType`에 `com.apple.mobile.restored`와 버전으로 답하는 것뿐이다. `client->restore`는 없으면 함수 안에서 만든다 | `restore.c` 5404~, `restore_open_with_timeout` 442~ |
| F2 | 그 뒤 `HardwareInfo`·`SavedDebugInfo` 조회(실패해도 진행), `client->tss`가 NULL이어도 `plist_dict_get_item`은 안전하게 통과, 리버스 프록시 생성(장치 포트 1082; 프로토콜 v2 실패 시 v1 재시도; 생성 실패는 로그만 남기고 **진행**), `StartRestore` 전송 후 메시지 루프 진입 | `restore.c` 5430~5740 |
| F3 | `idevicerestore_start`의 IPSW 열기, 빌드 매니페스트, TSS 요청, 복구/DFU 단계는 `restore_device` **앞**에 있다. `restore_device`를 직접 부르면 이 단계들(과 Apple TSS 네트워크 접근)을 거치지 않는다 | `idevicerestore.c` 1596~ |
| F4 | libusbmuxd는 환경 변수 `USBMUXD_SOCKET_ADDRESS`로 접속 주소를 정한다. 호스트 쪽은 `hv_restore_session_allow_usbmuxd(port)`로 그 loopback 포트를 정책에 등록한다 | `libusbmuxd.c` 167, `host/restore_session.h` |
| F5 | 루프: 반복 맨 앞에서 `err < 0`이면 `FLAG_QUIT`를 세우고, 같은 반복에서 메시지 하나를 더 받아 처리한다. 수신 타임아웃은 `continue`(오류 유지), 타임아웃이 아닌 수신 오류는 `err=-11`로 `break`(`HAVE_RESTORE_E_RECEIVE_TIMEOUT` 빌드일 때). `MsgType`이 없거나 문자열이 아니거나 알 수 없는 종류는 `err`를 건드리지 않는다. `CheckpointMsg`에 필수 필드가 없으면 `err=-1; break`. 루프 뒤 `async_err != 0`이면 `err = async_err` | `restore.c` 5730~5924 |
| F6 | `StatusMsg`의 처리기는 `AMRError`(부호 없는 정수) 필드가 있으면 음수 결과를 돌려준다. `ProgressMsg`는 필수 필드가 없으면 -1, 정상이면 0 | `restore.c` 828~890, `restore_handle_progress_msg` |
| F7 | `AsyncDataRequestMsg`는 `restore_handle_data_request_msg`를 **그대로** 별도 detach 스레드에서 실행하고, 음수면 `client->async_err`와 `FLAG_QUIT`를 쓴다. 처리기가 쓰는 자원(`SystemImageData` → IPSW 파일시스템, `BuildIdentityDict` → 빌드 아이덴티티 등)에 따라 가짜 장치만으로는 도달하지 못할 수 있다 | `restore.c` 5152~5165, 4900~ |
| F8 | 리버스 프록시 제어 스레드는 장치가 보내는 32비트 명령을 읽는다: 1 = 연결 요청(연결 스레드 생성, 핸들을 변수 하나에 대입), 2 = 종료 요청. 제어 시작 핸드셰이크: v2는 `BeginCtrl\0` + plist(`CtrlProtoVersion`)를 보내고 `ConnPort`를 가진 plist 응답을 기다림, v1은 `HelloCtrl\0` 교환 | `reverse_proxy.c` 468~620 |

## 3. 구조 [설계]

```
[시험 드라이버] ── hv_restore_session_begin / allow_usbmuxd / run(fn)
      │                     (기존 호스트 API, 훅 NULL 아님)
      └─ fn: idevicerestore_client_new → ecid/udid 설정 → restore_device(client, 빈 build_identity)
                │ libusbmuxd (USBMUXD_SOCKET_ADDRESS=127.0.0.1:P)
                ▼
      [가짜 usbmuxd]  ListDevices/Listen → 가짜 장치 1대, Connect(port) → 서비스로 연결
                ├─ 62078 (restored) → [가짜 restored]  QueryType / QueryValue / StartRestore 응답 + 시나리오 메시지 송신
                └─ 1082 (프록시 제어) → [가짜 리버스 프록시 장치]  (T12·T13에서만; T2·T5는 연결 거부)
```

- 실제 IPSW, TSS, 개인 usbmuxd 스레드, `host/install.c`의 시작 경로를 쓰지 않는다. 호스트 세션 API만 쓰므로 정책(`net_guard`) 아래에서 시험한다.
- 신규 파일(전부 시험 코드, 제품 코드 변경 없음): `tests/fake_device/fake_usbmuxd.c`, `tests/fake_device/fake_restored.c`, `tests/fake_device/fake_proxy_dev.c`(T12·T13), 공용 헤더, 시험 본체 `tests/test_fake_restore.c`, 실행 스크립트 `tools/fake-restore-test.sh`(`tools/netguard-test.sh`와 같은 형태: `tools/restore-selftest-build.sh`로 만든 라이브러리에 링크). 시험 코드는 `tests/test_restore_stack.c`처럼 `hv_virtq_*` 등 스텁이 필요할 수 있다.
- 새니타이저(AddressSanitizer/ThreadSanitizer)는 **컴파일러 플래그만**으로 별도 빌드 구성에 쓴다. 소스 변경 없음. 의존성 추가 없음(컴파일러 기능, CI 환경 확인은 별도).
- 가짜 장치는 시나리오(메시지 순서, 지연, 연결 종료)를 데이터로 받는다. 시나리오는 실행 중 변경되지 않는다.

## 4. 시험별 설계

### 4.1 T2 오류 보존·전파 [설계]

가짜 restored가 `StartRestore` 응답 뒤에 시나리오 메시지를 연달아 보낸다.

| 사례 | 메시지 순서 | 관측 | 비고 |
| --- | --- | --- | --- |
| (a) 타임아웃 | `StatusMsg`(`AMRError` 포함, 음수 결과) 뒤 무응답 | `restore_device` 반환값, 소요 시간 | 수신 타임아웃은 `continue`이므로 `FLAG_QUIT`가 서면 루프가 끝나는지 확인. 대기 시간(복원 수신 한도)이 걸려 가장 느린 사례 |
| (b) 처리기 0 반환 | `StatusMsg`(음수) 직후 `ProgressMsg`(정상, 0 반환) | 반환값이 음수인가 0인가 | P10의 핵심 사례. 기준선에서는 덮어쓰기 여부를 **기록**한다 |
| (c) 대조군 | 음수 `StatusMsg` 직후 `MsgType` 없음 / 문자열 아님 / 알 수 없는 종류 | 반환값 유지 | 덮어쓰지 않는 경로 |
| (d) 수신 오류 | 음수 `StatusMsg` 직후 연결 종료 | `-11`로 덮어쓰는지 | `HAVE_RESTORE_E_RECEIVE_TIMEOUT` 빌드일 때만 의미. 빌드 설정(V6)을 기록 |
| (e) `break` 경로 | 필수 필드 없는 `CheckpointMsg` | `err=-1` 유지 | 보존되는 경로의 대조 |

- 판정: 기준선은 **판정 없이 기록**. 변경 후(I3)에는 T2-보존(첫 오류 식별 정보)과 T2-전파(최종 결과가 성공이 아님)를 각각 판정한다(시험 명세 T2).
- 도달 가능성 [확인 F5, F6]: 가짜 장치만으로 가능. 벤더링 후크 불필요. IPSW·빌드 아이덴티티·TSS 불필요(빈 `build_identity` 딕셔너리).
- 한계: 실제 장치가 실패 직후 어떤 메시지를 보내는지(V4)는 모른다. 시나리오는 가정이다.

### 4.2 T5 응답 정지와 대기 한도 [설계 + 미정]

- 동기 경로 부분: 처리기가 장치 응답을 기다리는 동안 가짜 장치가 응답하지 않을 때 `restore_device`의 소요 시간과 반환을 기록한다. 가짜 restored만으로 가능(예: `BasebandUpdaterOutputData`처럼 `DataPort`로 장치에 접속하는 처리기에서 가짜 usbmuxd가 연결을 수락만 하고 침묵). [미정] 구현 착수 때 해당 처리기가 빈 `build_identity`와 가짜 장치만으로 실제로 장치 접속에 도달하는지 소스로 확인.
- **비동기 경로 부분(T5의 핵심: 루프 종료 뒤 비동기 스레드가 해제된 `client`를 참조)**: F7에 따라 비동기 처리기도 `restore_handle_data_request_msg`이므로 처리기 선택이 관건이다. 후보:
  - (A) 빈 `build_identity`로도 장치 접속(`DataPort`)까지 가서 블록하는 DataType이 있으면 가짜 장치만으로 가능.
  - (B) 없다면 최소 IPSW/빌드 아이덴티티 고정물(fixture) 필요 → 구현이 커지고 범위 확대(별도 승인).
  - (C) 그래도 어려우면 T5 비동기 부분은 후크 설계(나) 단계로 미룬다.
  - **[미정]** 처리기별 첫 자원 의존을 소스로 전수 조사한 뒤 (A)/(B)/(C)를 정한다. 이 조사는 읽기 전용이며 구현 승인 후 첫 작업으로 한다.
- 관측 수단: 기준선은 AddressSanitizer 빌드로 해제 후 접근을, ThreadSanitizer로 `async_err`/`flags` 경쟁을 관찰한다(Linux PC 한정, 둘 다 "관측 없음"이 안전성 증명은 아님).

### 4.3 T12 열린 프록시 연결이 있는 종료 [설계 + 미정]

- 가짜 프록시 장치: `BeginCtrl` 핸드셰이크에 `ConnPort` 응답, 이후 연결 요청 명령(1)을 보내고 호스트가 여는 연결 스레드의 핸드셰이크(`HelloConn` 등)를 받아 준 뒤 **연결을 열어 둔 채 침묵**한다. 호스트 쪽 외부 호스트 소켓은 시험이 띄운 loopback 서버로 가게 하며, 해당 주소는 시험에서 정책 허용 목록에 등록한다(제품 정책 변경 없음).
- 시나리오: 연결을 연 상태에서 가짜 restored가 `StatusMsg` 완료로 루프를 끝낸다.
- 관측: `restore_device` 반환 여부와 시간(기존 P15의 무기한 join 가능성), 종료 시 열린 fd 수(`/proc/self/fd` 전후 비교), ASan/TSan.
- **무기한 대기 방지**: 시험 드라이버가 별도 감시 시간제한(시험 전용)을 두고, 초과하면 "hang 관측"으로 기록하고 프로세스를 종료한다(`_exit`; `test_restore_stack.c`의 선례). 제품 코드를 건드리지 않는다.
- [미정] 연결 스레드 핸드셰이크 세부(메시지 코드 0x105 등)는 `reverse_proxy.c` 120~250, 380~470을 구현 착수 때 읽어 가짜 장치에 반영.

### 4.4 T13 연달아 오는 연결 요청 [설계]

- T12의 가짜 프록시 장치가 연결 요청(1)을 두 번 이상 **간격 없이** 보낸다.
- 관측(기준선 기록): 스레드 핸들 유실 징후(종료 시 hang, 해제 후 접근, fd 누수)를 ASan/TSan과 fd 수로 본다. P13은 코드상 위험이며 실행에서 나타나지 않을 수 있다(나타나지 않아도 "없음"이 아님).

## 5. 가짜 장치만으로 가능한 것과 후크가 필요한 것 [설계]

| 시험 | 가짜 장치만 | 시험 전용 후크 필요 | 비고 |
| --- | --- | --- | --- |
| T2 (a)~(e) | 가능 | 불필요 | |
| T5 동기 부분 | 가능(처리기 확인 [미정]) | 불필요 | |
| T5 비동기 부분 | 처리기에 따라 (A)/(B)/(C) | (C)일 때 | [미정] |
| T12, T13 | 가능(핸드셰이크 세부 [미정]) | 불필요 | |
| T1a, T1b, T3 | 불가 | 필요 | 등록 순서·스레드 생성 실패 주입은 장치 쪽으로 만들 수 없음 |

후크 설계(나)는 위 표에서 "불가/필요"로 남은 항목에 **한정**한다.

## 6. 단계와 승인 범위 제안 [설계]

| 단계 | 내용 | 승인 필요 | 제품 코드 변경 |
| --- | --- | --- | --- |
| S1 | 하니스(가짜 usbmuxd, 가짜 restored, 시험 본체, 빌드 스크립트)와 T2 (a)~(e) 기준선 | 시험 코드 신규 파일 작성 | 없음 |
| S2 | T5 처리기 전수 조사(읽기 전용) → (A)/(B)/(C) 결정, T5 동기 부분 | S1 결과 확인 후 | 없음 |
| S3 | 가짜 프록시 장치, T12·T13 기준선 | 시험 코드 신규 파일 | 없음 |
| S4 | T5 비동기 부분 및 T1a·T1b·T3용 후크 설계안 제시 | 설계안 검토 뒤 별도 승인 | (승인 시) 벤더링 후크 |
| S5 | I1·I2·I7 구현 | 별도 승인 | 호스트 코드 |

각 단계는 결과를 기록 문서로 남기고(관측/추론/미검증 구분), CI(해당 커밋 SHA 기준)에서 실제 실행된 검사와 건너뛴 검사를 구분해 보고한다. 이 시험들은 CI에 연결하기 전까지는 로컬 실행 결과만이며, Android에서는 실행되지 않는다.

## 7. 한계

- 가짜 장치는 실제 restored·리버스 프록시의 충실한 모사가 아니다. 실제 장치가 실패 후 보내는 메시지, 응답 시간, 연결 거동(V4, V9)은 알 수 없다.
- 통과/무관측은 시험한 순서·시나리오에서의 결과이며 일반 안전성이 아니다. 원자성·해제 순서·훅 수명은 시험으로 입증되지 않고 소스 구조(통합 설계 8절)로 보장해야 한다.
- 새니타이저·링크 가로채기·`shutdown` 효과는 Linux PC 한정 관측이며 Android 동작을 보장하지 않는다.
- 구현 중 소스 가정이 틀리면 이 문서의 범위를 고친 뒤 진행한다.
