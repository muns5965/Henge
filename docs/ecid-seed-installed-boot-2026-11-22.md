# ECID/seed 전달 경로 — 설계 및 구현 (2026-11-22)

이 문서는 사용자의 "1번 ECID/seed 전달 경로 설계를 진행하되, 설계 문서에 그치지 말고 일반 VM의 실제 부팅 기능 구현을 목표로 해주세요" 지시에 따라
진행한 작업의 결과와, 이어서 2번(G7 vs 외부 이미지)·3번(Android GPU 백엔드)을 순차적으로 진행하기 위한 구체적인 구현 과제를 정리한다.

## 1. 구현한 것: 세 번째 실행 모드 (`host/ui_runner_vm_installed.c`)

기존 두 바이너리는 각각 다음 조합만 지원했다:

| 바이너리 | installer | ecid/seed | 용도 |
|---|---|---|---|
| `host/ui_runner_vm.c` (`ui_runner_vm`) | 0 (일반 부팅) | 항상 0/0 (`hv_runner_make_config`가 강제) | 아무것도 설치되지 않은 상태의 진단 |
| `host/ui_runner_vm_g4.c` (`ui_runner_vm_g4`) | 1 (DFU 설치 부팅) | 0이 아닌 값 필수 | TSS·DFU→Recovery→RecoveryOS 진단 |

`installer=0`(일반 부팅, DFU 아님) + `ecid/seed`가 0이 아닌 값(설치된 디스크의 머신 아이덴티티) 조합은 어느 쪽에도 없었다. 이번에 그 조합을
위한 **세 번째, 별도의 바이너리** `host/ui_runner_vm_installed.c` (`ui_runner_vm_installed`)를 추가했다.

핵심 설계 원칙 — `host/ui_runner_vm_g4.c`의 `g4ui_config_ok()` 선례를 그대로 따름:

- `host/runner_vm_core.c`의 `hv_runner_make_config()`(네 가지 안전 보장: readonly_storage=1, no_net=1, installer=0, **ecid=0/seed=0**)는
  **전혀 수정하지 않았다**. 이 함수는 `host/ui_runner_vm.c`를 비롯한 기존의 모든 호출자에게 ecid=0/seed=0을 "보장"하는 함수이기 때문에,
  여기서 다르게 만들면 그 보장이 거짓이 된다.
- 공유 검사 함수 `hv_runner_config_ok()` (ecid==0 && seed==0을 강제) 역시 **전혀 수정하지 않았고, 새 바이너리는 이 함수를 호출하지 않는다.**
- 대신 새 바이너리 자신만의 로컬 검사 함수 `installed_boot_config_ok()`를 두어, `hv_runner_config_ok()`가 보장하는 다른 모든 것(읍리only 스토리지,
  네트워크 차단, JIT 메모리 없음, 경로/크기 범위)은 동일하게 강제하면서, ecid=0 요구만 뺐다. `installer==0` 요구는 그대로 유지했다 (G4와 달리
  이것은 DFU 부팅이 아니라 일반 부팅이다).
- `host/ui_runner_vm.c`는 **한 글자도 수정하지 않았다.**

### 안전장치 — 빌드/정적 검사 스크립트

- `tools/ui-runner-vm-installed-build.sh`: `tools/ui-runner-vm-build.sh`와 동일한 패턴으로 실제 엔진에 링크하여 빌드.
- `tools/check-runner-vm-installed-safe.sh`: `tools/check-runner-vm-safe.sh`의 대응 버전. 네트워크/프로세스 생성 심볼 금지, restore/TSS/libusb
  패스스루 금지, 공유 라이브러리 allow-list, 그리고 소스 레벨 검사(이 파일이 `readonly_storage`/`no_net`/`installer`를 전혀 대입하지 않는지,
  `cfg.ecid = ecid;` / `cfg.seed = seed;`가 정확히 한 번씩만 있는지, `runner_vm_core.c`의 4가지 안전 대입이 그대로인지, `hv_runner_config_ok`를
  호출하지 않는지, 허용된 옵션이 정확히 `--shm --rom --disk --aux --ecid --seed --ram-mb --cpus --jit-mb --seconds --stop-wait-ms`인지)를 강제한다.
  `tools/check-runner-vm-safe.sh`가 `host/ui_runner_vm.c`에서 `--ecid`/`--seed` 문자열 자체를 금지하는 것과 반대로, 이 스크립트는 이 바이너리가
  **반드시** `--ecid`/`--seed`를 실제로 가져야 한다고 요구한다 (죽은 코드가 아니라 실제로 연결되어 있는지 확인).
- `.github/workflows/vm-installed-link-check.yml`: 실제 CI에서 Android arm64용으로 엔진+디바이스를 빌드하고, 이 새 바이너리를 링크하여 정적 검사를
  통과하는지 확인한다 (research/android-port, research/android-port-g2b1 브랜치 푸시 시 트리거).

### 로컬에서 확인한 것 / CI에서 확인할 것

로컬에는 arm64 크로스 툴체인(NDK)이 없어 실제 엔진에 링크하는 전체 과정은 로컬에서 수행할 수 없다 (이전 라운드들과 동일한 제약, 예:
Task #168의 "Part B"). 로컬에서 확인한 것:
- `host/ui_runner_vm_installed.c`/`runner_vm_core.c`/`frame_shm.c`/`display_buf.c`를 네이티브 x86_64에서 `-Wall -Wextra -Werror`로 컴파일 — 전부 통과.
- 새 체커 스크립트가 요구하는 모든 소스 레벨 조건(안전 필드 미대입, ecid/seed 대입 1회, `hv_runner_config_ok` 미호출, 옵션 목록 정확히 일치,
  `runner_vm_core.c`의 4개 안전 대입 불변)을 `grep`으로 직접 재확인 — 전부 일치.
- `tools/check-no-test-flags.sh`, `MODES=plain tools/c1vm-test.sh`의 Part A (손대지 않은 `runner_vm_core.c`/`frame_shm.c`/`display_buf.c`를 검증) —
  전부 통과, 회귀 없음.
- 실제 Android arm64 엔진 링크와 `tools/check-runner-vm-installed-safe.sh`의 바이너리 레벨 검사(심볼, 공유 라이브러리, ELF 머신 타입)는 이 커밋을
  푸시한 뒤 실제 CI(`vm-installed-link-check.yml`)에서 확인한다.

## 2. 명시적으로 하지 않은 것 (별도 승인 필요)

- **G7 (실제 복원)**: `restored_start_restore()`를 호출하는 것, 즉 ASR/FDR을 통한 실제 디스크 쓰기는 이번 작업에 포함되지 않았다. 이 바이너리는
  디스크를 읍리only로만 열며, 어떤 디스크 이미지를 어떻게 만들지는 다루지 않는다.
- **디스크 쓰기 전면 허용**: `readonly_storage`는 이 새 바이너리에서도 그대로 1이다.
- **`host/disk_policy.c`의 `macos_disk.img` 생성 로직 연결**: 아직 손대지 않았다.
- 이 세 가지는 모두 `docs/g6-vs-general-launcher-gap-2026-11-22.md`가 이미 정리한 대로, 디스크에 처음으로 쓰기가 일어난다는 공통점 때문에 별도의
  명시적 승인이 필요하다.

## 3. 이어서 진행할 구체적 구현 과제

### 2번: G7 (실제 복원) vs. 외부에서 가져온 사전 설치 이미지

ECID/seed 전달 경로(이번 작업)는 디스크가 **어떻게** 설치되었는지와 무관하게 두 경로 모두에 적용된다. 비교해야 할 두 경로:

1. **G7 (실제 restore)**: `docs/g6-vs-general-launcher-gap-2026-11-22.md`의 stop-E(221/222, restore-options-plist 구성 + reverse-proxy 시작)
   다음 단계로 `restored_start_restore()`를 실제로 호출. 구현 과제:
   - `host/install_g6.c`/`host/install_g7.c`(신규) 형태로, G0~G6과 동일한 "자체 바이너리 + 체크 스크립트 + 워크플로우" 패턴을 따를 것.
   - `restored_start_restore()` 호출 전/후 디스크 상태를 해시로 검증하는 기존 패턴(`hv_g4_begin`/`hv_g4_finish` 등)을 그대로 재사용.
   - 디스크에 실제로 쓰는 첫 단계이므로, 별도의 명시적 승인 없이는 이 바이너리를 실행하지 않음 (빌드는 가능, 실행은 승인 후).
2. **외부에서 가져온 사전 설치 이미지**: 이미 설치가 완료된 `.img`를 외부(예: 다른 머신에서 만든 이미지, 공개 배포 이미지)에서 가져와 그대로
   `ui_runner_vm_installed`에 `--disk`/`--aux`로 넘기는 경로. 구현 과제:
   - 그 이미지의 ECID/seed를 **어떻게 알아내는가** (이미지 자체에 박혀 있는 값을 읍리only로 읍어오는 도구가 필요 — 디스크/aux 포맷을 파싱하는
     새 유틸리티, 쓰기 없음).
   - 라이선스/출처 문제 (Apple의 서명된 이미지를 재배포하는 것이 허용되는 경로인지)는 코드가 아니라 사용자가 먼저 결정할 사안.
3. 두 경로 모두 결과물은 "ecid/seed가 알려진, 읍리only로 열 수 있는 disk+aux 이미지 두 개"로 동일하므로, 이번에 만든
   `ui_runner_vm_installed`는 경로와 무관하게 재사용된다 — 이것이 이번 작업이 "별도 실행 모드"로 설계된 이유이기도 하다.

### 3번: Android GPU 백엔드

현재 `host/pvg_metal_stub.c`는 모든 Metal 호출에 NULL/-1을 반환하는 스텁이다. 구현 과제 (3개 수렴 아이템; 2번과 병행 준비 가능):

1. **PVG 명령 캡처 지점 확정**: 게스트가 보내는 PVG surface/Metal 명령이 실제로 어느 함수를 통해 호스트로 넘어오는지 (`devices/pvg_lite.c`의
   pvgmap/pvgdesc/pvgtally 경로) 정확히 매핑. 이미 Task #168의 `frames_from_pvg`/`frames_from_pvg_surface` 카운터가 이 경로가 "도달하는지"는
   보여주지만 "무엇을 그리라고 하는지"는 아직 해석하지 않는다.
2. **백엔드 선택**: Android에서 OpenGL ES 3.x 대 Vulkan 중 선택 (SM-S948N/Adreno/Xclipse 쪽 드라이버 성숙도 조사 필요 — 실기기 벤치마크로
   결정할 사안, 코드 작업 전에 결정 필요).
3. **스텁 교체**: `host/pvg_metal_stub.c`와 같은 인터페이스를 유지하는 새 `host/pvg_<backend>_android.c`를 작성하고, 기존 `ANativeWindow` 기반
   프레임 전달 경로(`android/app/src/main/cpp/ui_jni.c`의 `impl_nativeRender`)와 공존시킬 것 — 지금의 "PVG/boot_args에서 뽑아낸 정지 이미지"
   경로를 완전히 대체하는 것이 아니라, 실제 GPU 렌더링이 준비되기 전까지는 폴백으로 유지하는 것이 안전.

이 두 트랙(2번, 3번) 모두 아직 구현을 시작하지 않았으며, 각각의 착수에는 별도의 명시적 승인이 필요하다.
