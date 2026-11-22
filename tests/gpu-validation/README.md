# GPU 검증 시험 (Android GPU 백엔드 연구)

이 디렉터리는 외부 GPU 구현(libapplegfx-vulkan 등)의 실행 동작을 재현하기 위한 독립 시험 도구입니다. Henge의 VM·PVG·GPU 실행 소스는 바꾸지 않습니다. 결과 해석과 판정은 `docs/gpu-research/component-validation.md`와 `test-matrix.md`에 있습니다.

## 파일

| 파일 | 역할 | 외부 의존 |
|---|---|---|
| `stage1_pixels.c` | Vulkan 자체 동작(클리어·사각형 클리어·버퍼→이미지 복사) 픽셀 시험. 셰이더 없음 | Vulkan 로더 |
| `air2spv_probe.c` | AIR 모듈 열기(A1)와 air2spv 변환(A2) | libapplegfx-vulkan 정적 라이브러리 |
| `air2spv_stub_compare.c` | 생성된 SPIR-V가 폴백 스텁과 같은지 비교 (A2 보조 증거) | libapplegfx-vulkan 정적 라이브러리 |
| `draw_triangle.c` | SPIR-V 두 개로 파이프라인 생성(A4)과 삼각형 드로우·픽셀 비교(A5) | Vulkan 로더 |
| `run_triangle_chain.sh` | 위 도구를 묶어 A1–A5와 음성 대조군을 한 번에 실행 | 위 전부 |

## 환경

- Linux, gcc, Vulkan 헤더와 로더(`libvulkan`). 이 저장소에서 확인한 버전은 Vulkan 헤더 1.3.275입니다.
- Vulkan 장치: 시험은 드라이버를 고르지 않고 첫 번째 장치를 씁니다. 장치 이름과 종류를 출력에 기록합니다. lavapipe를 쓰려면 `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`을 지정합니다.
- A3(`spirv-val`)는 선택 도구입니다. 없으면 A3는 BLOCKED로 기록되고 PASS로 세지 않습니다.

## 실행

libapplegfx-vulkan 소스를 읽기 전용으로 두고, 그 소스로 만든 `libapplegfx-vulkan.a`가 필요합니다.

```bash
cd tests/gpu-validation
LIBAPPLEGFX_SRC=/path/to/libapplegfx-vulkan \
LIBAPPLEGFX_LIB=/path/to/libapplegfx-vulkan/build \
AIR_INPUT=/path/to/libapplegfx-vulkan/examples/triangle/out/triangle.air \
WORK=/tmp/gpu-validation-run \
./run_triangle_chain.sh
```

`WORK`는 비어 있어야 합니다. 스크립트가 이미 내용이 있는 디렉터리를 거부합니다.

단일 단계만 실행하려면 `stage1_pixels.c`, `draw_triangle.c`를 `gcc ... -lvulkan -lm`으로 빌드해 실행합니다. `draw_triangle`은 인자로 `vertex.spv fragment.spv`를 받습니다.

엔트리 이름: 기본값은 `main`입니다. 엔트리 이름이 다른 SPIR-V(예: libapplegfx의 `examples/triangle/out/triangle_*.spv`, 엔트리 `triangle_vertex`/`triangle_fragment`)를 쓰면 `ENTRY_V`, `ENTRY_F` 환경 변수로 이름을 지정해야 합니다. 지정하지 않으면 파이프라인 생성이 `VkResult=-13`으로 실패합니다.

## 판정 규칙

- 픽셀 시험은 기대값을 시험 코드 안에서 독립 계산합니다. 구현의 출력을 정답으로 쓰지 않습니다.
- A5는 삼각형 내부·외부 픽셀만 판정합니다. 경계에서 1픽셀 이내의 띠는 래스터화 규칙이 확정되지 않아 보고만 하고 판정에 쓰지 않습니다.
- 음성 대조군은 시험이 실패를 잡는지 확인하는 용도입니다. 대조군이 실패하지 않으면 해당 시험은 신뢰하지 않습니다.
- A2의 폴백 판정은 두 가지 증거를 씁니다. 로그에 `falling back` 또는 `emitting reference` 표식이 있는지, 생성된 바이트가 폴백 스텁과 같은지입니다.

## 한계

- 이 도구는 Vulkan 파이프라인을 직접 만듭니다. libapplegfx의 PVG 명령 처리 경로를 거치지 않으므로, 여기서 PASS여도 libapplegfx나 pvg-vulkan의 명령 경로가 맞다는 뜻이 아닙니다.
- 실행 드라이버가 CPU(lavapipe)이면 실기기 GPU 결과가 아닙니다.
- 로그 파일(`lagfx.log`)에는 실행 환경 변수가 들어갈 수 있습니다. 외부 공유 전에 확인합니다.
