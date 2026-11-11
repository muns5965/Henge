# 변경 전 기준선 관측 기록 (2026-11-11)

목적: 통합 설계 v2.2(확정, 커밋 05b009b / 상태 기록 d037d69) 구현 전에, 현재 코드의 동작을 기록해 이후 I1~I7 변경 전후를 비교할 수 있게 한다. 이 문서는 관측 기록이며 **D1~D8, C1~C19를 바꾸지 않는다.** 코드·벤더링 소스는 변경하지 않았다.

구분 표기: **[관측]** 실제로 실행해 본 결과 / **[추론]** 소스 읽기에서 나온 판단(실행 확인 아님) / **[미검증]** 확인하지 못했거나 이 환경에서는 확인할 수 없음.

## 1. 실행 환경과 한계

| 항목 | 내용 |
| --- | --- |
| 환경 | Linux 6.18.44 x86_64 컨테이너, gcc 13.3.0 (Ubuntu 24.04), PC 빌드 |
| 소스 기준 | 커밋 d037d69 (`research/android-port`) |
| 빌드 위치 | 저장소 밖 임시 디렉터리. 저장소 파일 변경 없음 |
| 대상 아님 | Android 빌드·실기, 실제 장치, 실제 Apple 서비스, IPSW (사용하지 않음) |

**한계.** 아래 관측은 PC 빌드의 현재 동작 기록이다. Android 동작, 실제 장치 연결 이후의 비동기 종료 안전성, 복원 프로토콜의 안전성을 보여주지 않는다. 존재하지 않는 IPSW로 한 시험은 복원 **초기 오류 경로**만 확인한다.

## 2. 관측 결과

### 2.1 netguard-test.sh (연결 정책)

- [관측] `tools/netguard-test.sh`: 마지막 줄 `NETGUARD OK (0 failed)`. 소요 약 27 초(복원 스택 빌드 포함).
- 의미: 설치 전/후, 설치 중 등록된 loopback 외 연결 거부, 제거 후 원래 동작 복귀 등 **기존 정책 기준선**. 세션 수명 결함의 유무를 보이지 않는다.

### 2.2 restore_selftest (복원 스택 자가 시험)

- [관측] `tools/restore-selftest-build.sh`로 빌드 후 `restore_selftest /nonexistent/no-such.ipsw <work>`를 3회 실행. 3회 모두 같은 결과:
  - `hv_install_start -> 0`, `MUX_LISTENING=1`, `MUX_PROTOCOL_OK=1`
  - `ipsw_open ...: No such file or directory`, `no ipsw file given`
  - `install: restore finished: -1`, 진행 콜백 1회(step 101), `RESTORE_RESULT=-1 AFTER_S=0`
- 의미: 복원이 시작될 수 없을 때 멈춤·충돌 없이 오류와 진행 콜백으로 끝난다는 **초기 오류 경로**의 재현성. 장치 연결 이후 경로(T2, T5 등)는 아님.

### 2.3 호스트 세션 API (B1~B9)

시험 코드는 부록 A. 2회 실행 결과가 같았다.

| 번호 | 호출 | 결과 | 비고 |
| --- | --- | --- | --- |
| B1 | `begin()` | 0 | |
| B2 | 엔진 실행 중 `engine_threads()` | 1 | |
| B3 | 엔진 실행 중 `end()` | -1 | 정책 유지 |
| B4 | 엔진 실행 중 `begin()` | -1 | |
| B5 | 엔진 종료 후 `engine_threads()` | 0 | |
| B6 | **`end()` 없이 `begin()` 재호출** | **0** | |
| B7 | `end()` | 0 | |
| B8 | `end()` 중복 호출 | 0 | |
| B9 | `end()` 뒤 `run()` | -1 (rc 변경 없음) | 훅이 없으면 실행 거부 |

- [관측] B6: 엔진 등록이 해제된 뒤에는 `end()` 없이도 `begin()`이 성공한다. 설계 문서 S8(1)의 "`begin()`은 세션이 열려 있는지 모른다"를 API 수준에서 뒷받침한다.
- 해석 한계: 이것은 **복원이 진행 중인 상태에서 새 세션이 시작됐다는 증거가 아니며** 경쟁 조건이 재현된 것도 아니다. 현재는 `hv_install_start`의 `g_result` 검사가 별도로 이를 막는다(소스 확인, 이 시험에서는 호출하지 않음).

### 2.4 이번 범위에서 얻지 못한 기준선

| 시험 | 얻지 못한 이유 |
| --- | --- |
| T2, T4, T5, T6, T7, T8, T9, T12, T13 | restored 프로토콜을 흉내 내는 가짜 장치 구현이 필요 (별도 승인) |
| T1a, T1b, T3 | 시험 전용 후크 필요 (별도 승인) |
| T14, T15, T17~T28 | I1~I7 이후에만 관측 대상이 생김 |

## 3. V11, V12 조사 (읽기 전용)

### V11 구독 해제의 블로킹

| 대상 | [추론] 소스 읽기 | [미검증] |
| --- | --- | --- |
| `irecv_device_event_unsubscribe` | 마지막 리스너를 지우면 `thread_join`. 이벤트 스레드는 반복마다 `listeners==0`을 확인해 종료한다. 한 반복 = `libusb_handle_events_timeout`(tv=0) + `usleep` 100 ms(핫플러그 분기) 또는 500 ms(비핫플러그 분기) | `libusb_handle_events_timeout` 자체의 반환 시간(이 프로젝트의 libusb 대체 구현 포함), 어느 분기가 실제로 컴파일되는지(`HAVE_LIBUSB_HOTPLUG_API` 전달 미확인). 따라서 "최대 500 ms"로 확정하지 않는다 |
| `usbmuxd_events_unsubscribe` | `listenfd`에 `shutdown` 후 `thread_cancel` 시도. `HAVE_PTHREAD_CANCEL`은 확인한 어떤 빌드 설정에도 정의되어 있지 않아 `thread_cancel`은 -1을 돌려주고 `running=0`이 된다. 이후 `thread_join`은 `device_monitor`가 `get_next_event` 복귀를 거쳐 루프를 나올 때까지 기다린다 | `shutdown`이 `get_next_event`를 실제로 깨우는지(V5와 동일 문제), `get_next_event`의 대기 상한(끝까지 읽지 않음). **종료 시간 상한은 확인되지 않았다** |

결론: V11은 **미해결 유지**. irecv는 짧은 주기 구조가 보이나 상한 미확정, usbmuxd는 V5에 의존.

### V12 단조 시계 조건변수

- [관측] 프로젝트 설정: `android/app/build.gradle`의 `minSdk 30`, `ndkVersion '27.2.12479018'`.
- [추론] AOSP bionic master `pthread.h`: `pthread_condattr_setclock`에는 API 가용성 제한 주석이 없고, `pthread_cond_clockwait`는 `__INTRODUCED_IN(30)`이다. minSdk 30이므로 소스상 사용 가능해 보인다.
- [미검증] 실제로 쓰는 NDK r27 sysroot 헤더, 해당 NDK로 빌드한 결과, 실기 동작. **V12는 완료 처리하지 않는다.**
- [관측] `libimobiledevice-glue`의 `cond_wait_timeout`은 `gettimeofday`(벽시계) 기반 절대 시각으로 `pthread_cond_timedwait`를 부른다. C19의 단조 시계 조건과 다르므로 구현 시 레지스트리는 이 함수를 쓰지 않고, 조건변수 시계 설정(`pthread_condattr_setclock`)과 마감 시각 계산 모두에 `CLOCK_MONOTONIC`을 써야 한다(구현 시 주의 사항, 설계 변경 아님).

## 4. 미검증 유지 목록

V5(`shutdown` 효과), V9(유예 단계 연결 차단의 장치 영향), V11(위 표), V12(NDK 헤더·실기), V13(정리 한도 값), V14(`FLAG_CUSTOM` 경로 비사용 확인)는 이 기록으로 해소되지 않았다.

## 5. 다음 단계 (이 문서는 결정하지 않음)

1. 가짜 장치 시험의 최소 구현 범위 결정 (우선 T2, T5, T12, T13), 필요한 시험 코드와 벤더링 변경 범위 구분 — 별도 승인.
2. 가짜 장치만으로 재현하기 어려운 T1a, T1b, T3 등의 후크 설계 — 마지막, 별도 승인.
3. I1·I2·I7 구현 — 별도 승인.

## 부록 A. 호스트 세션 API 시험 코드 (B1~B9)

저장소에는 추가하지 않은 임시 시험 코드이며 재현용으로 문서에만 기록한다. `host/restore_session.c`, `host/net_guard.c`를 포함한 복원 스택(`tools/restore-selftest-build.sh`의 `rs/lib.a`)과 `tests/test_restore_stack.c`의 `hv_virtq_*` 등 스텁을 함께 링크해 빌드했다.

```c
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include "restore_session.h"
static volatile int started;
static int fn_sleep(void *a) { (void)a; started = 1; usleep(400000); return 0; }
static void *th(void *a) { int rc = -99; int r = hv_restore_session_run(fn_sleep, NULL, &rc); printf("[thread] run -> %d rc=%d\n", r, rc); return NULL; }
int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("B1 begin#1 -> %d\n", hv_restore_session_begin());
  pthread_t t; pthread_create(&t, NULL, th, NULL);
  while (!started) usleep(1000);
  printf("B2 engine_threads (during run) -> %d\n", hv_restore_session_engine_threads());
  printf("B3 end() during run -> %d (expect -1: policy stays)\n", hv_restore_session_end());
  printf("B4 begin() during run -> %d\n", hv_restore_session_begin());
  pthread_join(t, NULL);
  printf("B5 engine_threads (after run) -> %d\n", hv_restore_session_engine_threads());
  printf("B6 begin() again WITHOUT end() -> %d (S8: begin does not know a session is open)\n", hv_restore_session_begin());
  printf("B7 end() -> %d\n", hv_restore_session_end());
  printf("B8 end() again -> %d\n", hv_restore_session_end());
  int rc = -99; int r = hv_restore_session_run(fn_sleep, NULL, &rc);
  printf("B9 run() after end() (hooks removed) -> %d rc=%d\n", r, rc);
  return 0;
}
```
