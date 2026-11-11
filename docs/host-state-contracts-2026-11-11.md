# 호스트 상태 전이 계약 통합 보완 기록 (2026-11-11, 2차)

범위: 호스트 코드와 시험만. 벤더링 소스(I3~I6, I10), 확정 설계 D1~D8·C1~C19의 문언, main은 변경하지 않았다. 설계 문서 7절 API 표에 해당하는 신규 함수는 이 문서와 `host/restore_registry.h` 주석이 정의한다(원문 갱신은 별도).

## 1. C15 대조
C15 (e): "엔진 등록 해제 뒤 남아 실행되는 코드가 전역 레지스트리와 다음 세션의 상태(전역 콜백·결과 변수 포함)에 접근하지 않는다." — 결과 게시(콜백·결과 변수)가 끝나기 전에 다음 세션이 시작되면 이 조건을 어긴다. 따라서 "게시 완료 뒤 IDLE"은 새 조건이 아니라 (e)의 구현이다. (f) "확정된 결과는 IDLE 전환이 바꾸지 않는다"와 191행 "CLOSED 유지는 정리 실패가 기록된 채 남는다는 뜻"과도 일치한다. C15 문언 변경 없음.

## 2. 구현한 계약
| 계약 | 구현 |
|---|---|
| 소유자(C13) | `hv_restore_session_claim()`이 `(스레드, 세대)`를 기록, `release_idle`/`end`까지 유지. `close`·`finalize`·`publish_done`·`release_idle`·`end`는 소유자만. `run()` 실행 여부는 `net_guard` 엔진 카운트 그대로(두 번째 기록 없음). 소유자가 없으면(claim 안 한 기존 호출자) 기존 단일 스레드 동작. `abort_start`는 claim 전에만 |
| 결과 확정 | `hv_reg_finalize`: 닫기 상태, 정리 결과(기록된 정리 실패·미해제 객체 포함), 합성, 세대당 1회 기록, **게시 권한 1회 발급**을 한 임계구역에서. 재호출은 기록된 결과만 반환하고 권한을 다시 주지 않음 |
| 게시 | `NOT_PUBLISHED→PUBLISHING→PUBLISHED`. 콜백·결과 변수는 잠금 밖에서 권한을 받은 쪽만. `release_idle`은 `PUBLISHED`이고 정리 실패·미해제 객체가 없을 때만 IDLE. 콜백 안에서는 PUBLISHING이라 start·cancel·release_idle이 거부됨 |
| 취소 | `hv_reg_cancel_note`: 접수 이력만 기록(카운터·취소 작업 상태는 건드리지 않음, 닫기를 막지 않음). 엔진이 `run()`에서 반환한 뒤의 요청은 거부(완료 선행). 최종 결과는 호출자가 아닌 이력으로 판정: 접수 후 복원 실패=ABORTED_BY_USER, 접수 후에도 성공=SUCCESS. `DONE_*`는 장차 취소 실행 주체(I3~I6)만 기록 |
| 실패 구분 | `QUIESCENCE_FAILED`(정지 미확인; 정책·클라이언트·usbmuxd 유지, finalize는 SESSION_NOT_QUIESCENT) / `CLEANUP_FAILED`(CLOSED 유지 + `cleanup_failed` 기록; 결과 코드와 상태는 별개) |

열린 점(보고): 복원 후 usbmuxd 정지 실패는 상태가 이미 CLOSED이므로 C19("CLOSED 먼저면 감시는 아무것도 바꾸지 않는다")에 따라 QUIESCENCE_FAILED가 아니라 **CLEANUP_FAILED**로 처리한다(정책·세션 유지, 다음 시작 거부). 검토에서는 호스트 소유 작업의 정지 미확인을 QUIESCENCE_FAILED로 구분하자는 의견이 있었으나, CLOSED 이후 상태를 되돌리는 것은 C19와 충돌하므로 이 구현은 C19를 우선했다. 시작 실패 경로(둘째 스레드 생성 실패, 세션이 ACTIVE)에서는 usbmuxd 미정지가 QUIESCENCE_FAILED다.

## 3. 시험
- registry-test: finalize, finalize_qf, cancel_note 추가(일반·후크·ASan·TSan).
- session-reg-test: busy(claim·run 내부 닫기 거부·finalize 1회 권한·release_idle 순서·취소 이력·완료 선행), owner(다른 스레드의 close/finalize/release_idle/end 거부, 세대 교체 시 소유 기록 폐기), finish, pending(finalize 안에서 정리 결과 결정), reentry(콜백 안의 재진입), hang(콜백 미반환 시 CLOSED 유지·새 세션 거부, 늦은 반환 뒤에만 시작). 무한 대기는 시험 쪽 스레드로 모사했고 운영 코드에는 콜백 시간 제한이 없다.
- install-start-test: reenter 추가(콜백 안에서 `hv_install_start`·`hv_install_cancel` 거부, quit 플래그 불변).
