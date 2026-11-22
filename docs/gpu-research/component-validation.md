# 기능별 실행 검증표

이 문서는 기능이 실제로 동작하는지를 실행 로그로 판정한 결과입니다. 소스 확인이나 CI 통과는 여기서 PASS로 세지 않습니다.

## 판정 기준

| 상태 | 의미 |
|---|---|
| PASS | 실행했고 기대 결과와 일치 (로그 있음) |
| FAIL | 실행했지만 기대 결과와 불일치 |
| SKIP | 조건 부족으로 실행되지 않음 |
| BLOCKED | 필수 도구·픽스처·장치·권한이 없어 실행 불가 |
| NOT RUN | 실행 가능하지만 아직 실행하지 않음 |
| NOT IMPLEMENTED | 해당 기능의 호출 경로가 없음 (소스 확인) |

PASS의 종류를 나눕니다. "파싱 PASS"는 구조·반환값만 확인한 것이고, "픽셀 PASS"와 "GPU 값 PASS"는 실제 출력을 비교한 것입니다.

## 실행 환경

| 항목 | 값 |
|---|---|
| 호스트 | Linux x86_64 (Ubuntu 24.04.5), CPU 2개, 메모리 7GB |
| Vulkan 장치 | llvmpipe (Mesa 25.2.8, lavapipe), deviceType=CPU. 실제 GPU 아님 |
| 실행 제약 | 이 샌드박스에는 glslang, spirv-val, glslc, spirv-as, llvm-20이 없음 |

---

## A. libapplegfx-vulkan (커밋 7574eac0)

보고서: 에이전트 A 실행 결과 (`/home/claude/validation/A/report.md`, 이 저장소에 복사되지 않음)

| 기능 | 상태 | 근거 |
|---|---|---|
| 초기화·기본 명령 | PASS (파싱 중심) | 단위시험 71개 PASS. 구조·반환값 확인 |
| 렌더 타겟 픽셀 | PASS (픽셀 1점) | `vulkan render`에서 (32,32) readback 1점만 비교 |
| triangle e2e (기본 설정) | **FAIL** (env 설정 실행) | `USE_APPLE_SPV=1`에서 Apple SPV 파이프라인 생성 VkResult=-13. 원인은 추정이며 재현하지 않음 |
| triangle e2e (`USE_APPLE_SPV=0`) | PASS | 중앙·모서리 2점 비교 |
| shader translate | BLOCKED | 시험이 `/opt/homebrew/opt/llvm@20/bin/llc`를 하드코딩 (`tests/shader_translate_test.c:19–20`) |
| pipeline build | BLOCKED | 같은 하드코딩 경로 (`tests/pipeline_build_test.c:34–35`) |
| compute dispatch | NOT IMPLEMENTED | `vkCmdDispatch` 호출 없음, 파싱 stub |
| macOS xcrun 단계 | NOT RUN | macOS 환경 없음 |
| Docker 경로 e2e | NOT RUN | CLAUDE.md 배포 절차에 해당하여 실행하지 않음 |

**셰이더 검증 한계 (중요)**

- glslang 부재로 커밋된 **스텁 SPIR-V**를 사용했습니다. 셰이더 관련 PASS는 실제 변환 결과의 검증이 아닙니다.
- spirv-val 부재로 air2spv 여러 건은 내부 검증을 건너뛰고도 meson에서 OK로 집계됩니다.
- air bitcode reader는 픽스처 부재로 내부 SKIP 후 OK로 집계됩니다.

따라서 기본 실행의 "72 OK"는 기능 동작 PASS 72건이 아닙니다.

---

## B. pvg-vulkan (커밋 59fd73aa)

보고서: 에이전트 B 실행 결과 (`/home/claude/validation/B/report.md`, 이 저장소에 복사되지 않음)

테스트를 개별 프로세스로 실행한 결과입니다. 통합시험 84개(parity 62, compute 22) 기준입니다.

| 구분 | 개수 | 내용 |
|---|---|---|
| PASS | 4 | parity 1개(문자열 파서, GPU 검증 아님), compute 3개(타일 shuffle·quad의 GPU 값 비교) |
| FAIL | 0 | 통합시험 기준 |
| SKIP | 13 | compute: glslc·spirv-as 부재로 본문 미실행. 단일 cargo 실행에서는 ok로 집계되므로 주의 |
| BLOCKED | 67 | AIR 픽스처 부재 57, `#[ignore]` 10 (parity 4, compute 6) |

추가 실행:

| 항목 | 결과 |
|---|---|
| batch 시험 | BLOCKED 10 (픽스처) |
| reflection 시험 | BLOCKED 3 (픽스처) |
| 라이브러리 단위시험 | 1903 PASS / **4 FAIL** (단독 실행에서도 재현, 원인 분석 안 함) |

기능별 판정

| 기능 | 상태 | 근거 |
|---|---|---|
| draw | BLOCKED | GPU 검증 0건. AIR 픽스처(`render_tri.air`, `textured_quad.air`, `render_frag_texture.air`)가 저장소에 없음. Metal 산출물이라 Linux에서 생성 불가 |
| blit (copy) | NOT IMPLEMENTED (Vulkan 경로) | `runtime/blit_exec`가 Vulkan 백엔드를 참조하지 않는 호스트 메모리 복사. 단위시험은 FakeHost 기반 |
| compute | PASS 3 / SKIP 13 / BLOCKED 6 | GPU 값 비교 PASS 3건. 나머지는 도구 부재 또는 `#[ignore]` |

**발견 사항**

1. 기본 기능(`backend-metal`)이 Linux에서 `compile_error`로 빌드 실패합니다. `--no-default-features --features backend-vulkan`으로 우회해 실행했습니다(소스 변경 없음).
2. 지시서의 복사 범위 밖 의존성(`../../../metal2vulkan`)을 함께 복사했습니다.
3. `#[ignore]`는 compute 6개입니다. 지시서에 적힌 11개와 다릅니다. 캡처 환경변수는 모두 미설정입니다.
4. 단일 프로세스 cargo 실행에서는 한 시험의 패닉이 뮤텍스를 오염시켜 9개가 PoisonError로 실패합니다. 판정은 개별 프로세스 결과를 기준으로 했습니다.
5. 라이브러리 단위시험의 FAIL 4건은 원인을 분석하지 않았습니다. 미해결 항목입니다.

**미검증**

- draw 전부 (픽스처 부재)
- blit 전부 (Vulkan 경로 없음)
- compute SKIP 13건과 ignored 10건
- 실제 GPU 결과
- pvg-vulkan-paging, pvg-vulkan-wire 멤버의 개별 시험

---

## 교차 비교 (같은 기능, 다른 구현)

| 기능 | libapplegfx-vulkan | pvg-vulkan | 비교 가능 여부 |
|---|---|---|---|
| triangle 렌더링 | FAIL (기본), PASS (`USE_APPLE_SPV=0`) | draw 픽스처 부재로 BLOCKED | 불가. 입력이 다름 |
| compute | NOT IMPLEMENTED | PASS 3 | 기능 범위가 달라 비교 불가 |
| blit/copy | 2 opcode 호출 경로 확인, 시험은 파싱 중심 | Vulkan 경로 없음 | 불가 |

## 이 문서의 한계

- 두 에이전트가 실행한 결과입니다. 실행 로그는 각 에이전트 작업 디렉터리에 있으며, 이 저장소에는 요약만 기록했습니다.
- 실행 환경은 CPU 가상 Vulkan(lavapipe)입니다. 실제 GPU 결과가 아닙니다.
- 로그에는 환경 변수 덤프가 포함되어 있습니다. 외부 공유 전 확인이 필요합니다.
- metal2vulkan, reims-vgpu, Indium, Henge Android는 아직 실행하지 않았습니다.
