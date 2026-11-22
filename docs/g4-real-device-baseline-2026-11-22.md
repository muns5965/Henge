# G4 실기기 성공 실행 — 회귀 기준선 (2026-11-22)

목적: 2026-11-22에 사용자의 실제 Android 기기(Galaxy S26 Ultra, SM-S948N)에서 `VmRunnerG4Launcher`가 `START_ENABLED = true`로
처음 실행되어 Apple 로고를 실시간으로 표시하고 정상 종료한 사례를, 이후 회귀시험이 비교할 수 있는 기준선으로 보존한다.

구분 표기: **[관측]** 실제 실행 로그에서 확인 / **[추론]** 소스 읽기에서 나온 판단 / **[미검증]** 이 실행만으로는 확인되지 않음.

**이 문서가 보존하지 않는 것.** 원본 로그 `henge_vm_display_g4_log.txt`(159,665줄, 12.8MB)는 이 저장소에 커밋하지 않는다. 원본에는
USB 장치 일련번호 형태의 문자열과 방대한 원시 패킷 트레이스가 들어 있어, 회귀 기준선에는 필요하지 않고 커밋하기에 적절하지 않다고
판단했다(이 판단은 검토 문서가 아니라 이 작업을 수행하며 직접 내린 것이다). 아래는 그 로그에서 추출한, 이미 검증된 요약 사실만을
담는다.

## 1. 실행 환경

| 항목 | 내용 |
| --- | --- |
| 기기 | Galaxy S26 Ultra (SM-S948N), Android 17 (API 37), arm64-v8a |
| 바이너리 | `ui_runner_vm_g4` (host/ui_runner_vm_g4.c), `VmDisplayG4Activity` 경유 실행 |
| 커밋 | `714f7df`(`START_ENABLED = true`로 전환한 커밋) 빌드, `research/android-port` |
| 원본 로그 SHA-256 | `9a3f59121860cda6656b461ccc2ad517c8c1c275031a8eefe3c87fb2c6f62d9f` (독립적으로 2회 재계산, 일치) |

## 2. 결과 요약 — [관측]

```
install g4 RESULT: g4_passed
(engine return 219, session report 2,
3 TSS request attempt(s),
device confirmed recovery->restore
with RecoveryOS running off its own ramdisk,
disk and aux content verified,
0 failed row(s))
```

`ui_runner_vm_g4 RESULT: stopped, confirmed (g4 ended), g4 passed (FAILED rows=0), ignored input lines 0`

`g4 vm runner exited, code 0 (stopped, confirmed)`

`session report 2`는 stop C에서 의도적으로 중단했기 때문에 "복원 완료 아님"으로 분류된 것이며, G4 진단의 실패를 의미하지 않는다
(설계상 의도된 결과) — [추론, 기존 설계 문서 기준].

## 3. 안전 경계 — [관측]

| 항목 | 결과 |
| --- | --- |
| TSS 요청 | 3회 시도, 마지막 응답 HTTP 200 / `class=SUCCESS` |
| 네트워크 가드 (`net_guard_hosts`) | 허용 호스트 정확히 3개 (gs.apple.com + CDN 별칭 2개), 포트 443 |
| 연결 수 | 10회 허용, 0회 거부 |
| 디스크 내용 (`disk_identity`) | 실행 전후 해시·크기·mtime 동일 |
| AUX 내용 (`aux_identity`) | 실행 전후 해시·크기·mtime 동일 |
| 디스크 쓰기 시도/거부 | 9건 요청, 9건 거부 (게스트가 AUX에 9회 쓰기를 시도했으나 읽기 전용 보호로 전부 차단됨) |
| 서명 티켓 파일 | 신규 0건 |
| 작업 디렉터리 변경 | 최상위 항목 0건 변경 |
| SHM 프레임 발행 | `display present 185, frames published 185`, `shm_errors 0` |
| Android 측 수신 | `frames 178 (copied), shown 178, rejected 0` |
| 종료 코드 | 0 |

주의(검토 문서에서 지적되어 이미 반영한 제한 2건, 그대로 유지):
- `net_guard`의 "0 refused"는 가드 적용 범위 밖의 직접 `connect()` 호출까지 감시했다는 의미는 아니다.
- `local_writes` 검사는 작업 디렉터리 **최상위 항목**만 비교한 결과이며, 시스템 전체 쓰기가 전혀 없었다는 의미로 확대해서는 안 된다.

## 4. 화면 출처 — [미검증], 이번 실행만으로는 확인 불가

이 로그에는 `HV_SCREEN_PVG` / `HV_SCREEN_PVG_SURFACE` / `HV_SCREEN_BOOT_ARGS` 문자열이 전혀 없다(grep으로 확인). 당시
`host/runner_vm_core.c`의 프레임 펌프가 `HvBootDisplay.source`를 전혀 읽지도, 집계하지도 않았기 때문이다 — 로깅 누락이 아니라
구조적 공백이었다.

이 공백은 이번 보존 작업과 같은 날, 별도 커밋(`frames_from_pvg` / `frames_from_pvg_surface` / `frames_from_boot_args` /
`frames_from_unknown_source`, `HvRunnerStats`)으로 메웠다. **이 기준선 실행 자체에는 그 계측이 없었으므로, 표시된 Apple 로고가
PVG surface 경로에서 왔는지 boot_args 경로에서 왔는지는 여전히 확정할 수 없다** — 이후 동일한 조건의 재실행에서 새 계측으로
확인해야 한다.

## 5. 향후 회귀시험이 비교할 기준 (최소 조건)

동일한 조건(같은 ROM/IPSW/대상 기기 세대)에서 재실행할 때, 다음과 어긋나면 회귀로 본다:

1. `install g4 RESULT: g4_passed`, `engine return 219`, `0 failed row(s))`.
2. `disk_identity ok` / `aux_identity ok` — 해시 불변.
3. 디스크 쓰기 요청 수 = 거부 수 (즉 승인 없이 쓰기가 성공하는 경우가 생기면 즉시 회귀).
4. `net_guard_hosts ok` — 허용 호스트 집합이 그대로이고, 그 밖의 호스트로 연결 시도가 없음.
5. `frames_published > 0`, `shm_errors 0`, `call_errors 0`.
6. 종료 코드 0, `stopped, confirmed`.

위 1~6은 모두 이번 기준선 실행에서 실제로 만족된 값이며, 임의로 정한 목표치가 아니다.

## 6. 범위 밖

이 기준선은 G4(RecoveryOS 도달까지)의 성공만을 기록한다. `restore_device()` 실행, 실제 macOS 설치, 디스크 쓰기 허용은 여전히
별도의, 그때마다 새로 받는 승인 범위이며 이 문서는 그 승인을 구성하지 않는다.
