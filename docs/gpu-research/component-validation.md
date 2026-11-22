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
| triangle e2e (기본 설정) | **FAIL** (env 설정 실행) | `USE_APPLE_SPV=1`에서 Apple SPV 파이프라인 생성 VkResult=-13. 원인은 엔트리 이름 불일치(기존 SPV 엔트리 `triangle_vertex`, 파이프라인은 `main` 지정)로 확인됨. 자세한 내용은 아래 E절 |
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

---

## C. 1단계 픽셀 하네스 (Vulkan 자체 동작, 셰이더·AIR 없음)

하네스: `/home/claude/validation/harness/stage1_pixels.c` (외부 저장소 밖에 있으며, 이 저장소에는 결과 요약만 기록합니다)

| 시험 | 내용 | 상태 |
|---|---|---|
| T1 full clear | 전체 이미지 클리어, 4096픽셀 전부 비교 | PASS |
| T2 rect clear | 렌더 패스에서 사각형 클리어, 경계 포함 전부 비교 | PASS |
| T3 buffer→image | 픽셀별 패턴 복사, 3회 반복 | PASS ×3 |
| 음성 대조군 | 기대값을 일부러 틀리게 바꿔 실패 확인 | FAIL (의도된 결과, 35픽셀 검출) |

이 결과가 보여주는 것: lavapipe의 clear·복사·readback 경로가 독립 계산한 기대값과 일치합니다.
보여주지 않는 것: libapplegfx나 pvg-vulkan의 구현 정확성, 실기기 GPU 동작, 삼각형 래스터화, 셰이더 연산.

## D. AIR 입력 픽스처 조사 (출처 미확인, 사용 가능 여부 미판정)

| 위치 | 파일 | 출처 상태 |
|---|---|---|
| libapplegfx `examples/triangle/out/` | `triangle.air`, `triangle.metallib` | `build_spirv.sh`가 `xcrun metal`(macOS)로 생성한다고 설명. 이 사본의 생성 과정은 미확인 |
| libapplegfx `tests/fixtures/` | `default.metallib`, `triangle.metallib` | 출처 미확인 |
| metal2vulkan `fixtures/public/` | 다수의 `.air` | 저장소가 public 픽스처로 분류. 출처 문서 미확인 |
| reims-vgpu `metallibs/` | 여러 `.metallib` | 출처 미확인. pvg-vulkan 입력 구조와 호환 여부 미확인 |
| Indium `test/` | 여러 `.metallib` | 출처 미확인 |
| pvg-vulkan | `render_tri.air` 등 | 저장소에 없음 (BLOCKED 확인) |

후속: 위 픽스처 중 출처가 확인되고 libapplegfx 또는 pvg-vulkan의 입력 구조와 맞는 것만 2단계 Draw 시험에 사용합니다. 출처가 확인되지 않으면 해당 Draw 시험은 BLOCKED로 둡니다.

## E. libapplegfx air2spv 경로: triangle.air A1–A5 (실행 확인)

입력: `examples/triangle/out/triangle.air` (출처: `build_spirv.sh`가 macOS `xcrun metal`로 생성한다고 설명. 이 사본의 생성 과정은 미확인). 재현 도구와 기대 결과는 `tests/gpu-validation/`에 있습니다.

| 단계 | 상태 | 근거 |
|---|---|---|
| A1 AIR 파일 읽기 | PASS | `lagfx_air_module_open` status=0. 함수 `triangle_vertex`, `triangle_fragment` 확인 |
| A2 AIR→SPIR-V (스텁 폴백 없음) | PASS | vertex 1496 bytes, fragment 644 bytes를 `translate_function`으로 생성. 로그에 `falling back`·`emitting reference` 표식 0건. 두 blob 모두 폴백 스텁 바이트와 다름. 같은 입력 두 번 실행 시 해시 동일 |
| A3 spirv-val | BLOCKED | 도구 미설치. 이 단계 PASS가 없으므로 A4·A5를 SPIR-V 유효성의 근거로 쓰지 않음 |
| A4 파이프라인 생성 | PASS | 생성된 SPIR-V 엔트리 `main`, `vkCreateGraphicsPipelines` VkResult=0 |
| A5 Draw·readback | PASS | 삼각형 내부·외부 3724픽셀 기대값 일치. 경계 1px 띠(372픽셀)는 판정에서 제외하고 보고만 함 |
| 음성 대조군 (드로우 제거) | 실패 검출 (의도된 결과) | 1012픽셀 불일치, 경계 빨강 0 |

**`VkResult=-13` 원인:** 기존 `triangle_*.spv`(llc 경로 산출)는 엔트리가 `triangle_vertex`/`triangle_fragment`입니다. 이 이름 대신 `main`으로 파이프라인을 만들면 -13(VK_ERROR_UNKNOWN)이 나고, 이름을 맞추면 PASS입니다. 따라서 -13은 이 환경에서 변환 오류가 아니라 엔트리 이름 불일치로 확인되었습니다. libapplegfx e2e 시험이 엔트리 이름을 지정하지 않는다는 것은 코드 읽기로 확인한 사실이며, e2e를 직접 실행해 같은 결과를 본 것은 아닙니다.

**판정 한계**
- 이 시험은 Vulkan 파이프라인을 직접 만들었습니다. libapplegfx의 PVG 명령 처리 경로를 거치지 않았습니다. 따라서 이 PASS는 "변환기가 만든 SPIR-V가 이 삼각형을 그린다"는 뜻이며, 명령 처리 경로가 정상이라는 뜻이 아닙니다.
- A2 산출은 변환기가 새로 만든 SPIR-V입니다. 기존 `.spv`는 증거로 쓰지 않았습니다.
- 드라이버는 CPU(lavapipe)입니다.
