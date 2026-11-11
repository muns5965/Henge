# I1·I2·I7 구현과 변경 후 회귀 시험 기록 (2026-11-11)

범위: 호스트 코드만. 벤더링 소스(I3~I6, I10), 확정 설계 D1~D8·C1~C19, main 브랜치는 변경하지 않았다.

## 1. 구현한 것

| 항목 | 파일 | 내용 |
|---|---|---|
| I1 | host/restore_registry.{h,c} | 단일 mutex + CLOCK_MONOTONIC 조건변수. 상태 IDLE→ACTIVE→CLOSING→CLOSED→IDLE, 종단 QUIESCENCE_FAILED. 종류별 카운터, 세대 번호, 첫 오류 보존, 취소 1회 시작(재시도 1회), 연결 참조 계수, 해제 허가 질의, 결과 합성표(4.7), 최종 보고 1회 |
| I2 | host/restore_session.{h,c} | begin→레지스트리 begin(**끝나지 않은 세션이 있으면 거부** — 기존에는 조용히 재설정, 기준선 B6), run은 ACTIVE에서만, close()/end()가 CLOSING→대기→CLOSED→정책 제거→IDLE. 대기 기한을 넘기면 QUIESCENCE_FAILED(정책 유지, 해제 없음) |
| I7 | host/install.c, install.h | Android 분기: 복원 종료 뒤 close→해제 허가 질의→허가 시에만 idevicerestore_client_free→end. 불허 시 클라이언트를 의도적으로 해제하지 않음. `hv_install_cancel()` 추가(**작은 API 추가**) |
| 빌드 | tools/restore-selftest-build.sh, restore-android-check.sh | restore_registry.c 추가 |

한계(미구현, 승인 대기): 벤더링 쪽 작업(비동기 스레드, 프록시 제어·연결 스레드 등)은 아직 레지스트리에 **등록되지 않는다**. 따라서 이 변경만으로 T5·T12·T13 결함은 사라지지 않는다. 제한 시간 상수(close 대기 기본 15 s)는 **잠정값, 기기에서 측정하지 않음**.

## 2. 새 시험

| 시험 | 결과 |
|---|---|
| tools/registry-test.sh (일반/HV_TEST_HOOKS, 각각 일반·ASan·TSan) | 모두 통과, 보고 0 (qf, normal, errors, storm 200회, cancel, phase, conn, watchdog, hooks) |
| tools/session-reg-test.sh | normal 10, stuck 5, engine 2 통과 |
| tools/check-no-test-flags.sh | 통과 (레지스트리 객체 검사 활성: 출하 객체에 hv_test_hook 없음, 시험 객체에는 있음) |
| install.c -D__ANDROID__ 구문 검사 | 통과 (**Android 분기는 Linux에서 구문 검사만 가능, 실행은 미검증**) |

HV_TEST_HOOKS 후크는 락 안에서 고정 크기 기록만 한다(대기 없음). T1a 순서 검증은 락 밖에서 제어하고, 기록으로 "허용 판정과 카운터 증가가 같은 임계구역"임을 확인한다.

## 3. 기존 시험 (변경 후)

fdr-session-test: FAIL 0 (400회 run/end 경쟁 포함). netguard-test: 0 failed. preflight-test: 종료 0(내부의 'EXPECTATION NOT MET'은 음성 대조군의 의도된 출력).

## 4. 가짜 장치 회귀 (기준선 대비)

일반 빌드, 전 케이스 — 반환값·감시 만료 모두 기준선과 같음(시간 차이는 0.1 s 이내):
0, b: 0 / a: -1 (31.6 s) / c1·c2·c3·e: -1 / d: -11 / t5s_fdr: 0 / t5s_fdr_nr: 0 / t5a_*: -1 / t12_close: 0 (2.4 s) / t13: 0 / t1b: 0 / t3_inject: -1 / **t5s_bb, t12: 45 s 내 반환 없음(HANG OBSERVED, 기준선과 동일)**.

ASan: t5a_free heap-use-after-free restore.c:5161 **그대로 재현**. 그 외 사례 보고 없음.
TSan: t5a_free use-after-free 5161/5162, t13 data race 3건 **그대로 재현**.

해석: 호스트 변경이 벤더링 결함을 고치지도 악화시키지도 않았다(예상대로). 결함 해소는 I3~I6 승인 이후의 일이다.

## 5. 동작 변화
- B6: begin을 end 없이 다시 부르면 이제 -1(이전: 정책 재설정).
- 새 종료 경로: 레지스트리가 종단 상태에 빠지면 end()는 -1, 정책 유지, begin 거부(프로세스 재시작 필요 — 설계 C11).

## 6. 미검증
Android 실행, 실제 장치, CI에서의 시험 실행(CI는 빌드만), 시간 상수 적정성, 벤더링 작업 등록 후의 동작.
