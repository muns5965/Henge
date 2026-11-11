# 가짜 장치 S1 기준선 관측 기록 (2026-11-11)

범위: `docs/fake-device-impl-scope.md`의 S1만. 새 시험 파일(가짜 usbmuxd·가짜 restored·구동기·스크립트)을 추가하고 T2 (a)~(e)를 **변경 전 코드**에 대해 실행한 기록이다.
제품 코드, 벤더링 소스, 확정 설계(D1~D8, C1~C19)는 변경하지 않았다. 후크·계측 없음. 이 문서는 **기록만** 하며 통과/실패 판정을 하지 않는다.
실기기 안전·Android 실기 검증은 이 기록의 대상이 아니다.

## 1. 추가한 파일 (모두 시험 전용, 앱에 링크되지 않음)

| 파일 | 내용 |
|---|---|
| tests/fake_device/fake_dev.{h,c} | 가짜 usbmuxd(TCP 루프백, 커널이 고른 임시 포트) + 가짜 restored(62078). 포트 1082 연결은 RESULT_CONNREFUSED(3)로 거절 |
| tests/fake_device/stubs.c | virtio_usb.c의 게스트 메모리 쪽 대역(tests/test_restore_stack.c와 동일) |
| tests/test_fake_restore.c | 사례당 1프로세스. 호스트 세션 API(begin/allow_usbmuxd/run)로 `restore_device()`를 직접 호출. 사례별 감시 스레드(기본 150초, `FAKE_CASE_LIMIT_S`), 만료 시 "HANG OBSERVED" 기록 후 `_exit(124)` |
| tools/fake-restore-test.sh | 빌드(restore-selftest-build.sh 산출물 재사용) + 사례 순차 실행. 종료 코드는 "사례가 실행됐음"만 뜻함 |

격리: `USBMUXD_SOCKET_ADDRESS`를 시험이 직접 설정(`127.0.0.1:<임시포트>`), 실제 usbmuxd·외부 연결 없음. 식별자는 명백한 가짜 값(ECID 0x1F4C0DE, UDID `FAKE-UDID-FOR-TEST`)만 사용. 판정은 프로세스 종료 코드가 아니라 `restore_device()` 실제 반환값과 로그로 한다.

## 2. 환경

Linux 6.18.44 x86_64, gcc 13.3.0 (baseline-observations-2026-11-11.md와 동일). 빌드: `third_party/restore_config/config.h`로 빌드된 벤더링 스택. **HAVE_RESTORE_E_RECEIVE_TIMEOUT = 정의됨**(시험이 같은 config.h를 포함해 출력; V6 관련), HAVE_REVERSE_PROXY = 정의됨.

## 3. 관측 (3회 실행, 전부 동일)

반환값은 `restore_device()`의 값. 모든 사례에서 `hv_restore_session_run()`은 0, 실행 후 엔진 스레드 수 0, net_guard 거부 0(허용 10 = 가짜 mux 루프백뿐).

| 사례 | 가짜 restored 동작 (StartRestore 이후) | 반환값 | 소요 | 로그 요점 |
|---|---|---|---|---|
| 0 대조 | StatusMsg Status=0 | 0 | 1.6 s | 정상 종료. 하네스가 동작함을 확인 |
| (a) | StatusMsg(AMRError=1) 후 침묵 | -1 | 31.6 s | "Unable to successfully restore device" 후 30초 뒤 "No data to read (timeout)" |
| (b) | StatusMsg(AMRError=1) 직후 유효 ProgressMsg | **0** | 1.6 s | 실패 메시지 처리 후 ProgressMsg를 한 번 더 받아 처리 |
| (c1) | 실패 StatusMsg 후 MsgType 없는 메시지 | -1 | 1.6 s | "Unknown message received" |
| (c2) | 〃 MsgType가 문자열 아님 | -1 | 1.6 s | "Unknown message received" |
| (c3) | 〃 알 수 없는 MsgType | -1 | 1.6 s | "Unknown message type received" |
| (d) | 실패 StatusMsg 후 연결 종료 | **-11** | 1.5 s | "Could not read data (-256). Aborting." |
| (e) | CheckpointMsg(필수 필드 없음) | -1 | 1.6 s | "Failed to parse checkpoint id" |

공통: 가짜 mux에 포트 1082(역방향 프록시) 연결이 2회 도달해 거절되고, restore_device는 "waiting for it to reappear" 경로(약 1.1초)를 거쳐 계속 진행한다. 가짜 restored는 StartRestore를 1회 받는다. watchdog 만료(hang)는 어느 사례에서도 관측되지 않았다.

## 4. 추론 (관측에서 이끌어낸 것; 설계 문서의 소스 분석과 일치)

- (b): 현재 코드는 음수 err 이후에도 메시지를 한 번 더 받아 처리하므로, 뒤따르는 정상 ProgressMsg가 err를 0으로 덮어써 **실패 후 restore_device가 0을 반환**할 수 있다(P10). C18("실패 시 항상 음수") 계약은 현재 코드에서 이 시나리오에 성립하지 않는다.
- (d): 최초 오류(-1)가 수신 오류 코드(-11)로 덮인다. 둘 다 음수이므로 부호 계약은 유지되나 최초 오류 보존(D3)은 되지 않는다.
- (a)의 약 30초: `property_list_service_receive_plist` 기본 수신 제한이 30000 ms인 것(소스)과 로그 시각이 일치. 실패 선언 후에도 한 번 더 수신을 기다리는 구조의 비용이다.
- (c1~c3): 알 수 없는/없는 MsgType는 err를 건드리지 않아 이전 실패가 유지된다.

## 5. 미검증

- 위 모두 Linux x86_64 + 가짜 장치. 실기기·Android(NDK, 기기)에서의 동작은 미검증.
- 가짜 restored의 응답 형식은 소스 독해에서 구성했다. 실제 restored와의 일치는 미검증.
- 비동기 요청 처리(T5), 프록시 연결 스레드(T12·T13), 호스트 해제 경로(T1a·T1b·T3)는 이번 범위가 아니다(S2~S4).
- StatusMsg 이후의 모든 덮어쓰기 경로를 열거한 것은 아니다. 관측한 것은 (b)(d)뿐이다.
- 이 기록은 I1~I10 구현 승인이나 V5·V9·V11~V14 해소를 뜻하지 않는다.

## 6. 재현

`tools/fake-restore-test.sh <빌드 디렉터리>` (사례 지정: `CASES="a b"`). 사례 (a)는 약 31초 걸린다. 사례별 전체 로그는 `<빌드 디렉터리>/fd/case_<id>.out`.
