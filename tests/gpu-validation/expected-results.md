# 기대 결과와 기록된 실행 결과

이 문서는 `run_triangle_chain.sh`를 빈 디렉터리에서 실행했을 때의 기준 결과입니다. 같은 입력이면 같은 해시가 나와야 합니다. 실행 환경이 바뀌면 해시 비교 전에 환경을 먼저 기록합니다.

## 환경 (기록 시점)

| 항목 | 값 |
|---|---|
| 호스트 | Linux x86_64, Ubuntu 24.04.5, CPU 2개, 메모리 7GB |
| Vulkan 헤더 | 1.3.275 |
| 장치 | llvmpipe (LLVM 20.1.2, 256 bits), 종류 CPU (lavapipe). 실제 GPU 아님 |
| ICD | `/usr/share/vulkan/icd.d/lvp_icd.json` |
| spirv-val | 설치되지 않음 (A3 BLOCKED) |
| libapplegfx-vulkan | 커밋 7574eac0, 정적 라이브러리 빌드 |

## 입력 해시

| 파일 | sha256 |
|---|---|
| `examples/triangle/out/triangle.air` | `5ceb15690662f6a7d9b6ad82c9a6ccdaf257e76286deafd4f2a7d0c395e5e414` |

## 단계별 기준 결과

| 단계 | 기대 결과 | 기록된 결과 |
|---|---|---|
| Stage 1 T1 전체 클리어 | 4096/4096 픽셀 일치 | PASS |
| Stage 1 T2 렌더 패스 사각형 클리어 | 4096/4096 픽셀 일치 | PASS |
| Stage 1 T3 버퍼→이미지 복사 (3회) | 각 회 4096/4096 일치 | PASS ×3 |
| Stage 1 음성 대조군 | 기대값 오류 시 불일치 검출 | FAIL 검출 (35픽셀) |
| A1 AIR 모듈 열기 | status=0, 함수 목록에 `triangle_vertex`, `triangle_fragment` | PASS |
| A2a vertex 번역 (`translate_function`) | status=0, 1496 bytes | PASS |
| A2a fragment 번역 (`translate_function`) | status=0, 644 bytes | PASS |
| A2b 폴백 표식 (`translate_module` 로그) | 0건 | PASS (`translating fn[0]`만 나옴) |
| A2 스텁 바이트 비교 | 두 blob 모두 스텁과 다름 | PASS |
| A3 spirv-val | 도구 필요 | BLOCKED |
| A4 파이프라인 생성 | VK_SUCCESS | PASS (엔트리 이름 `main`으로 지정, 생성된 SPIR-V 엔트리 이름도 `main`) |
| A5 픽셀 (삼각형 내부·외부) | 3724 픽셀 기대값 일치 | PASS (0 불일치) |
| A5 경계 띠 (1px, 보고만) | 판정 없음 | 372 픽셀 중 140 빨강 |
| A5 음성 대조군 (드로우 제거) | 불일치 검출 | FAIL 검출 (1012 불일치, 경계 빨강 0) |

## 생성된 SPIR-V 해시 (기록 시점)

| 파일 | 크기 | sha256 |
|---|---|---|
| `triangle_vertex.spv` (A2a 산출) | 1496 | `92d17cd3aae2bfa842cf7fdeb064f9b4dfaaed75a9a17ebd6a142960ddb17886` |
| `triangle_fragment.spv` (A2a 산출) | 644 | `63aa24e8c104fa5de85ecc91180c775dab26993ca63fe351cba3e5f982d0bf85` |

같은 입력으로 두 번 실행해 두 해시가 모두 같았습니다.

## 저장소의 기존 SPIR-V와의 대조

`examples/triangle/out/triangle_vertex.spv`, `triangle_fragment.spv`는 이전 빌드(llc 경로)의 산출물입니다. A2의 증거로 쓰지 않으며, 아래 대조는 원인 분석용입니다.

| 입력 | 엔트리 지정 | 결과 |
|---|---|---|
| 기존 `triangle_*.spv` (엔트리 `triangle_vertex`/`triangle_fragment`) | 기본값 `main` | `VkResult=-13` (`VK_ERROR_UNKNOWN`) |
| 기존 `triangle_*.spv` | `ENTRY_V=triangle_vertex ENTRY_F=triangle_fragment` | PASS (A5 0 불일치) |
| 기존 `reference_vert.spv`/`reference_frag.spv` (GLSL 기준, 엔트리 `main`) | 기본값 `main` | PASS |
| 기존 `triangle_*.spv`, libapplegfx e2e 바이너리 (E1·E2) | 기본값 `main` (`tests/triangle-lavapipe-e2e.c` 624·628행) | `VkResult=-13` (2회 재현) |
| 기존 `triangle_*.spv`, libapplegfx e2e 바이너리 (E2) | `OpEntryPoint` 이름으로 변경 (인터셉터, 나머지 동일) | `VkResult=0`, 기존 e2e 픽셀 검증 통과 (3회) |
| `air2spv` 산출 `triangle_*.spv` (1496·644바이트, 엔트리 `main`) (E3) | 기본 설정 | e2e 통과 (3회) |

따라서 `VkResult=-13`의 원인은 엔트리 이름 불일치로 확인되었습니다(E1·E2). 이는 llc 산출 입력과 lavapipe에 한정된 결과입니다. 변환 결과가 틀렸다는 근거는 아닙니다. 이전 기록의 "e2e는 엔트리 이름을 지정하지 않는다"는 잘못된 기술이었습니다. 이 시험은 `"main"`을 명시합니다.

## 해석 한계

- A4·A5 PASS는 Vulkan 파이프라인이 만들어지고 이 삼각형이 기대 위치·색으로 그려졌다는 뜻입니다. SPIR-V 유효성(A3)은 검증되지 않았습니다.
- 이 파이프라인은 libapplegfx의 PVG 명령 처리 경로를 거치지 않았습니다. 명령 처리 경로 검증은 별도 단계입니다.
- 실행 드라이버는 CPU(lavapipe)입니다.
