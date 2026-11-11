# 호스트 종료·오류·취소 계약 보완 기록 (2026-11-11)

범위: 호스트 코드(`host/`)와 시험만. 벤더링 소스(I3~I6, I10), 확정 설계 D1~D8·C1~C19, main은 변경하지 않았다. 검토 지적 5건(①취소 ②결과 합성 ③스레드 생성 실패 ④엔진 ⑤연결 테이블)에 대한 조치다.

## 1. C13 대조 결과(엔진 등록)
C13: "`CLOSED` 전환 권한은 **엔진 스레드에만** 있고 감시는 `QUIESCENCE_FAILED`만 확정한다." C15(e): 엔진 스레드의 실제 종료 대기는 요구하지 않는다. 즉 엔진은 작업 카운터에 넣는 대상이 아니라 **닫기를 결정하는 주체**다. 따라서 엔진을 일반 비동기 작업처럼 카운터에 등록하지 않고, 엔진 실행 여부의 기록은 `net_guard`의 engine 카운트 **한 곳**에 둔다(두 개의 종료 판정을 만들지 않기 위해). 대신 `hv_restore_session_close()`가 engine 카운트가 0보다 크면 상태를 바꾸지 않고 -1을 돌려준다(엔진이 `run()` 안에 있는 동안 CLOSED 결정 금지). `HV_REG_QF_ENGINE_NOT_EXITED`는 호스트가 소유한 usbmuxd 스레드가 멈추지 않은 경우의 사유로만 쓴다.

## 2. 조치
| 지적 | 조치 |
|---|---|
| ③ 스레드 생성 실패 | 첫 `pthread_create`(usbmuxd) 실패: 아무것도 시작되지 않았으므로 `abort_start`(레지스트리 abandon + 정책 제거)로 IDLE 복귀. 둘째(복원 스레드) 실패: usbmuxd 스레드는 **조인 가능**하게 소유하고(`g_mux_th`), 정지 요청 후 한도 안에 멈추면 조인·IDLE, 못 멈추면 `QUIESCENCE_FAILED`로 선언하고 정책·세션을 유지하며 이후 시작을 거부한다(C11) |
| ② 결과 합성 | `hv_restore_session_finish(rc, user_cancel, extra_cleanup_failed)`가 닫기→정리 결과→`hv_reg_compose`→세대당 1회 보고를 한 곳에서 수행. 엔진 반환값, 정리(작업·클라이언트·usbmuxd), 세션 종료(정책 제거)를 구분. `end` 실패·정리 실패는 성공으로 보고되지 않음. usbmuxd 정리 실패 시 정책도 유지 |
| ① 취소 | `hv_install_cancel()` 유지. 세션 취소를 레지스트리에 1회 접수·시작·완료로 기록(C17; 스레드를 만들지 않으므로 생성 실패 단계는 없음)하고 종료 플래그와 도구의 quit 플래그를 올림. `global_quit_flag`와 취소 표시는 **다음 begin 성공 직후**에만 초기화한다(begin은 이전 세션이 깨끗이 닫힌 뒤에만 가능). 세션이 없거나 CLOSED면 취소는 거부되고 아무것도 바꾸지 않음. 사용자 취소 뒤 실패는 `ABORTED_BY_USER` |
| ④ 엔진 | 위 1절 |
| ⑤ 연결 테이블 | 같은 fd 중복 등록과 32개 초과 등록은 -1이며 **하드 실패로 기록**되어 깨끗한 CLOSED가 되지 않음(사유 `CONN_TABLE`). 호출자는 해당 연결을 프록시에 쓰지 않아야 함 |
| TSan 발견 | 호스트의 비동기 변수(결과, 취소 표시, usbmuxd 생존, 세대 번호)를 원자 접근으로 변경. 복원 스레드가 결과를 게시하기 전에 마지막 콜백을 호출하도록 순서 변경(새 시작이 콜백 포인터를 덮어쓰는 경쟁 제거) |

## 3. 시험
- tools/install-start-test.sh (install.c를 `-D__ANDROID__`로 Linux에서 컴파일, `--wrap`으로 pthread_create·usbmuxd_main·idevicerestore_start 대체): ok(재시작 포함), p1(첫 생성 실패), p2(둘째 생성 실패), p2_stuck(usbmuxd 미정지 → QUIESCENCE_FAILED, 정책 유지, 다음 시작 거부), end_fail(엔진 0 + 정리 실패 → CLEANUP_FAILED, 성공 아님, 다음 시작 거부), cancel(취소 1회, ABORTED_BY_USER, 재시작 시 플래그 초기화, 세션 없는 취소 거부). 일반·ASan·TSan 모두 통과(p2_stuck·end_fail은 의도적으로 usbmuxd 스레드를 남기므로 TSan thread-leak 보고를 끔).
- tools/registry-test.sh: conn_dup, conn_full 추가. 일반·후크·ASan·TSan 통과.
- tools/session-reg-test.sh: busy(엔진이 run() 안이면 닫기 거부·보고 없음), finish(정리 실패) 추가. 통과.
- 회귀: fdr-session 0 FAIL, netguard 0 failed, preflight 종료 0, check-no-test-flags OK. 가짜 장치 전 케이스 반환값·감시 만료가 기준선과 동일. ASan t5a_free(restore.c:5161), TSan t5a_free(5161/5162)·t13 data race 3건은 **그대로 재현**(벤더링 미변경).

## 4. 한계·미검증
- 벤더링 작업은 여전히 미등록이므로 T5·T12·T13 결함은 남아 있다.
- `global_quit_flag`는 벤더링 변수라 호스트는 원자 저장만 하고, 벤더링 쪽 읽기는 평문이다(TSan이 보고할 수 있는 벤더링 측 경쟁, 미조치).
- usbmuxd 정지 대기 5 s, 닫기 대기 15 s는 잠정값(기기에서 측정하지 않음).
- Android 실제 실행, 실제 장치, CI에서의 시험 실행은 미검증(CI는 빌드만).
- 사용자 취소가 복원 중인 비동기 작업을 실제로 끊는 것은 I3~I6 이후에 의미가 생긴다.
