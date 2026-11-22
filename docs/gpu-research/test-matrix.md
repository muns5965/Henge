# 단계별 시험 결과

환경: 샌드박스 Linux(Ubuntu 24.04), CPU 2개, 메모리 7GB. 실행 드라이버는 lavapipe(CPU Vulkan)이며 실제 GPU가 아닙니다.
판정 구분: PASS / FAIL / SKIP / BLOCKED / NOT RUN / NOT IMPLEMENTED. 기능별 상세는 `component-validation.md`를 봅니다.

## T0 기준선 (실행 확인)

libapplegfx-vulkan 커밋 7574eac0.

| 항목 | 결과 |
|---|---|
| 설정 | 성공, Vulkan 1.3.275 탐지 |
| 빌드 | 성공 |
| 단위 시험 | 75개 중 72 OK, 3 SKIP, 0 FAIL |
| CI 필수 Vulkan 시험 | vulkan init, command, render, translate render 4개 OK |

**주의:** "72 OK"는 기능 동작 PASS 72건이 아닙니다. 픽셀을 비교한 시험은 `vulkan render`의 1점뿐이고, 나머지는 파싱·구조 확인입니다. 셰이더는 glslang 부재로 스텁 SPIR-V를 썼고, spirv-val 부재로 일부 검증이 건너뛰어졌습니다.

SKIP 3건의 원인

| 시험 | 원인 | 상태 |
|---|---|---|
| shader translate | `/opt/homebrew/opt/llvm@20/bin/llc` 하드코딩 | BLOCKED |
| pipeline build | 같은 하드코딩 경로 | BLOCKED |
| triangle e2e | `LAGFX_TRIANGLE_SPV_DIR` 미설정. env 설정 후 실행하면 기본 설정(`USE_APPLE_SPV=1`)에서 **FAIL** (VkResult=-13). `USE_APPLE_SPV=0`에서는 PASS | FAIL 확인, 근본 원인 추정 |

## T1 Android 준비 (소스 확인)

- Linux 전용 API 사용처 확인 (vulkan-backend.md 3절)
- memfd_create API 30 확인 (android-port.md)
- 동기화 규칙과 Henge 차이 확인 (vulkan-backend.md 5절)

## T2 NDK 빌드 (NOT RUN)

NDK를 로컬에서 받을 수 없습니다. GitHub 러너 워크플로로 진행해야 합니다.

## T3 실기기 Vulkan (BLOCKED)

실기기 접근이 없습니다. 확인 항목: Vulkan 장치 탐지, 하드웨어 장치 선택, 독립 렌더링 결과.

## T4 Henge 호환성 분석 (부분 완료, 소스 확인)

- 프레이밍과 명령 번호 대조: pvg-protocol.md 완료
- 셰이더 변환 경로 대조: metal-shader.md 완료
- 남은 항목: 명령 네임스페이스 확정, 헤더 파서 최종 확인

## T5 외부 구현 실행 검증 (1차, 기존 시험 수정 없이 실행)

| 프로젝트 | 커밋 | 결과 요약 | 상세 |
|---|---|---|---|
| libapplegfx-vulkan | 7574eac0 | 기본 실행 72 OK / 3 SKIP. triangle은 env 설정 시 FAIL, 픽셀 비교 1점 | component-validation.md A절 |
| pvg-vulkan | 59fd73aa | 통합시험 PASS 4 / FAIL 0 / SKIP 13 / BLOCKED 67 (84개 개별 실행). 라이브러리 단위시험 1903 PASS / FAIL 4 | component-validation.md B절 |
| metal2vulkan | d9236a8a | NOT RUN (공개 CI 실패 기록은 이전 조사의 주장이며 이번 세션에서 확인하지 못함) | — |
| reims-vgpu | 818868564e | NOT RUN | — |
| Indium | 1bf59a85 | NOT RUN | — |
| Henge Android | 0ef6f1f | NOT RUN (NDK·실기기 필요) | — |

pvg-vulkan 실행 참고:
- 기본 기능(`backend-metal`)이 Linux에서 컴파일 오류입니다. `--features backend-vulkan`으로 우회해 실행했습니다.
- AIR 픽스처가 저장소에 없어 draw 검증은 전혀 실행되지 않았습니다(BLOCKED).
- 단일 프로세스 cargo 실행에서는 패닉 전파로 9개가 추가 실패하므로, 개별 프로세스 결과를 기준으로 했습니다.

## 외부 구현의 시험 존재 여부 (참고)

"시험 존재"는 소스에 시험 함수가 있다는 뜻일 뿐 통과를 뜻하지 않습니다.

| 프로젝트 | 시험 존재 | 실행 여부 |
|---|---|---|
| libapplegfx-vulkan | 단위·통합 시험 75개 | 실행 완료 (T5) |
| pvg-vulkan | 통합 시험 84개, 라이브러리 단위시험 | 실행 완료 (T5) |
| reims-vgpu | `#[test]` 존재 (vulkan 1245, graphics 191 위치) | NOT RUN |
| metal2vulkan | 확인 안 함 | NOT RUN |
| Indium | 확인 안 함 | NOT RUN |
