# G6 진단과 일반 Henge VM 런처 사이의 부팅 제어 격차 분석 (2026-11-22)

목적: "모두 진행" 지시의 세 번째 항목. G4/G6 등 설치 진단(`host/install_gN.c`, `host/vmrun.c`)이 도달한 지점과, 일반 VM 런처
(`host/ui_runner_vm.c`, Android `VmRunnerLauncher`)가 지금 할 수 있는 것 사이에 무엇이 비어 있는지를, 소스 코드 확인과 이
프로젝트 자신의 기존 설계 문서(`docs/restore-architecture.md`)를 근거로 정리한다. **분석 문서이며, 이 작업으로 코드를 바꾸지
않았다.**

구분 표기: **[관측]** 소스 코드에서 직접 확인 / **[기존 설계]** 이 저장소의 기존 문서에 이미 있던 판단 / **[결론]** 둘을 바탕으로
이번에 내린 판단.

## 1. 두 제어 흐름은 전혀 다른 일을 한다

- **설치 진단 계열** (`host/vmrun.c`의 `--install-g0`~`--install-g6`, `host/install_g4.c`/`install_g6.c`): 가상 머신을
  `cfg.installer = 1`(DFU 모드, 가상 USB 장치 포트 노출)로 생성하고, **같은 바이너리 안에서 실행되는 `idevicerestore`**
  (third_party/idevicerestore)가 그 가상 USB 포트 너머의 "장치"를 실제 Apple 복원 대상으로 여기고 복원 프로토콜을 끝까지
  진행시킨다. `cfg.readonly_storage = 1`을 항상 강제해, 프로토콜이 얼마나 깊이 가든 디스크·AUX에는 한 바이트도 쓰이지 않는다 —
  [관측, `host/vmrun.c` 391·405·419·433·449·465행 각 install-gN 분기].
- **일반 VM 런처** (`host/ui_runner_vm.c`, `VmRunnerLauncher`): `idevicerestore`를 전혀 링크하지 않고, 복원 프로토콜을
  전혀 모른다. `--rom --disk --aux`만 받아 `hv_runner_make_config()`로 설정을 만들고, **보통 부팅**(`installer = 0`)으로
  머신을 띄운 뒤 화면·입력만 SHM으로 중계한다 — [관측, `host/runner_vm_core.c`의 `hv_runner_make_config`, `host/ui_runner_vm.c`
  의 `--` 옵션 파서(246~254행)].

즉 설치 진단은 "비어 있는 디스크에 macOS를 쓰는 과정"을 읽기 전용으로 **흉내만 내며 끝까지 가 보는** 것이고, 일반 런처는
"이미 macOS가 설치되어 있다고 가정한 디스크를 평범하게 부팅"하는 것이다. 지금은 둘 사이에 아무 것도 없다 — 설치 진단이
실제로 디스크에 쓴 적이 한 번도 없으므로, 일반 런처가 부팅을 시도할 디스크 내용 자체가 아직 존재하지 않는다.

## 2. 설치 진단이 멈추는 지점 — stop A~E의 사다리

`third_party/idevicerestore/src/hv_stop.h`에 정의된, 이 프로젝트 전용의 5단계 정지점이 있다 — [관측]:

| 정지점 | 반환값 | 도달 시점 | 이 프로젝트의 사용처 |
| --- | --- | --- | --- |
| A | 217 | 빌드 식별·추출 여부 판단 직후, 서명 요청 전 | (G0대) |
| B | 218 | 서명된 iBSS/iBEC 전송 후, DFU→Recovery 재접속 확인 | (G2/G3대) |
| **C** | **219** | RecoveryOS 진입 확인(Recovery→Restore 전이), `restore_device()` 호출 **전** | **G4** (이번에 실기기 성공) |
| D | 220 | `restore_device()` 안, `restored` 서비스 핸드셰이크 확인, 실제 복원 명령 **전** | (G5대) |
| **E** | **221/222** | 복원 옵션 plist 완성 + Reverse Proxy 시작 확인, `restored_start_restore()` 호출 **바로 전** | **G6** (가장 깊은 현재 지점) |

다섯 지점 전부 "그 지점까지는 서명·전송·재접속이 실제로 일어나지만, 실제 디스크 쓰기 명령(`restored_start_restore()`)은
**한 번도** 호출되지 않는다"는 공통 설계다 — [관측, hv_stop.h 각 주석]. G6(stop E) 다음에 호출되는 것이 바로
`restored_start_restore()`이며, 이것이 ASR/FDR 기반 실제 디스크 쓰기 복원이다. **이 프로젝트에는 stop E 다음 지점(가령 "G7")이
아직 존재하지 않는다** — [관측, `tools/`에 `vmrun-install-g7-build.sh` 없음, 소스 전체에 `restored_start_restore` 호출 지점
없음(선언·extern만 있음)].

## 3. 이 프로젝트 자신의 로드맵도 같은 결론을 이미 적어 두었다

`docs/restore-architecture.md` §2의 단계표, §9의 "별도 승인이 필요한 항목" — [기존 설계]:

- 단계 4(RecoveryOS 진입)는 G4로 완료 표시, 단계 5(복원 모드 부팅)·6(ASR·AEA/URLAsset·FDR·펌웨어 TSS)은 "신규·변경 필요"로
  표시되어 있다. G6의 stop E는 단계 5와 6의 경계에 해당한다.
- §9: `[ ] 복원 실행 자체(ASR, 루트 스트리밍)와 설치 디스크 쓰기, macos_disk.img 실제 생성` — 아직 체크되지 않은, 별도 승인이
  필요한 항목으로 명시되어 있다.
- §7.1: `host/disk_policy.c`에 128 GiB `macos_disk.img` 생성 규칙은 이미 구현돼 있지만, **이를 호출하는 앱 코드가 아직 없다**
  (파일 자체 주석, `tools/check-no-test-flags.sh`가 CI에서 이 사실을 검사). 즉 "실제 복원이 쓸 디스크"조차 아직 어떤 실행
  경로에도 연결되어 있지 않다.

이번 분석은 이 기존 로드맵과 어긋나지 않으며, 오히려 소스 코드 쪽에서 같은 결론(격차의 정확한 위치)에 독립적으로 도달해
교차 확인한 것이다.

## 4. "설치됨"에서 "부팅됨"으로 가려면 — 신원(identity) 문제

`host/vm.h`의 `HvVmConfig`: `ecid, seed` 필드는 **"디스크가 설치될 때의 머신 신원"**이라고 명시되어 있다 — [관측, vm.h 18행].
macOS 설치는 특정 ECID/seed에 개인화(personalize)되며, 그 디스크를 나중에 정상적으로 부팅하려면 **같은** ECID/seed로 부팅해야
한다는 뜻이다.

- 설치 진단 쪽(`vmrun.c`)은 `--ecid`/`--seed` CLI 옵션으로 **실제 값**을 받아 `cfg.ecid`/`cfg.seed`에 설정한다 — [관측, vmrun.c
  320~321행, 523~659행의 각 `hv_gN_begin/start`가 `cfg.ecid`를 그대로 전달].
- 일반 런처 쪽(`host/ui_runner_vm.c`)은 **`--ecid`/`--seed` 옵션 자체가 없다** — [관측, 246~254행의 옵션 목록에 없음]. 그리고
  `hv_runner_make_config()`는 `c->ecid = 0; c->seed = 0;`을 **"보장해야 할 네 가지 안전 설정" 중 하나로 하드코딩**한다 —
  [관측, `host/runner_vm_core.c` 142~153행, 주석: "the four settings this runner exists to guarantee"]. Android 쪽
  `VmRunnerLauncher.java`도 ECID를 아예 받지 않는다(`buildCommand`가 `--shm --rom --disk --aux`만 구성) — [관측].

**[결론]** 설령 오늘 당장 누군가 G6 다음 단계(실제 ASR 복원)를 구현해 `macos_disk.img`에 진짜 macOS를 써넣는다 해도, 지금의
일반 VM 런처는 그 디스크를 **구조적으로** 정상 부팅할 수 없다 — ECID/seed를 받을 방법이 코드 어디에도 없고, 항상 0/0으로
강제되기 때문이다. 이것은 누락된 기능이 아니라, 지금 이 런처의 존재 이유("항상 신원 없음을 보장")와 정면으로 반대되는
요구사항이다.

## 5. 격차를 메우는 데 필요한 것 — 순서대로

1. **새 진단/실행 단계(가칭 G7)**: stop E 다음, `restored_start_restore()`를 실제로 호출하는 단계. 이번에 처음으로
   `readonly_storage`를 해제해야 하는 지점이므로, G6의 stop E 설계(`docs/g6-stop-e-design-2026-11-20.md`)에 준하는 별도
   설계 문서와 별도 승인이 필요하다 — §9에 이미 체크되지 않은 항목으로 올라 있다.
2. **`host/disk_policy.c`의 실제 호출**: 128 GiB `macos_disk.img` 생성 로직을 실제 설치 경로에 연결하는 앱 코드. 지금은
   아무 데도 호출되지 않는다(§7.1 자신의 주석).
3. **ECID/seed를 설치→부팅으로 넘기는 경로 신설**: 지금은 어떤 코드도 "이 디스크는 ECID X로 개인화됐다"는 사실을
   기록·전달하지 않는다. G7이 성공한 뒤 그 ECID/seed를 보존하고, 일반 런처가 그 값을 받아 쓸 수 있는 통로(CLI 옵션, Java
   Intent extra, 설정 파일 등)를 새로 만들어야 한다.
4. **일반 런처에 "설치된 디스크를 처음 부팅" 모드 추가**: `hv_runner_make_config()`의 "항상 ecid=0, seed=0, readonly_storage=1"
   이라는 현재의 안전 보장은 **지금의 목적(미설치 디스크에 안전하게 접근)**에는 맞지만, 설치된 디스크를 부팅하는 목적에는
   맞지 않는다. 기존 함수의 보장을 깨는 대신, 그 목적에 맞는 별도의 설정 생성 경로를 새로 만드는 것이 이 코드베이스의
   기존 관행(`vmrun.c`가 진단별로 별도 플래그 분기를 쓰는 것과 같은 방식)에 맞다.
5. 1~4 모두 디스크에 처음으로 실제 내용이 쓰이는 지점을 포함하므로, 이 문서의 결론과 무관하게 **그 자체로 매번 새로
   받는 승인이 필요**하다 — 사용자가 이미 여러 차례 명시한 경계("`restore_device()` 실행과 디스크 쓰기 허용은 별도 승인
   범위")와 일치한다.

## 6. 범위 밖 — 분명히 해 둘 것

이 문서는 설계·소스 대조 분석이며, 1~4 중 어느 것도 구현하지 않았다. 실제 ASR 복원 실행, `macos_disk.img` 생성, 또는
일반 런처의 ECID 수용 코드 추가는 각각 **이 문서와는 별개의, 그때마다 새로 받는 승인**이 필요하다.
