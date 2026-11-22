# 단계별 시험 결과

환경: 샌드박스 Linux(Ubuntu 24.04), Meson debug, `-Dtests=true`, 커밋 7574eac0 (libapplegfx-vulkan).
판정 구분: **실행 확인** (로그 있음) / **소스 확인** (코드만 읽음) / **미실행** (시험 존재 여부만 확인).

## T0 기준선 (실행 확인)

| 항목 | 결과 |
|---|---|
| 설정 | 성공, Vulkan 1.3.275 탐지, 실제 Vulkan 경로 활성화 |
| 빌드 | 241/241 성공 |
| 단위 시험 | 75개 중 72 OK, 3 SKIP, 0 FAIL (`build/meson-logs/testlog.junit.xml`) |
| CI 필수 Vulkan 시험 | vulkan init, command, render, translate render 4개 PASS |
| SKIP 3건 | shader translate, pipeline build (llc 경로 고정), triangle lavapipe e2e |

SKIP 원인 정정:
- llc 경로 2건: `tests/shader_translate_test.c:19–20`에 Homebrew llc 경로가 고정되어 있어 SKIP됩니다 (exit 77).
- triangle e2e 1건: 로그상 `LAGFX_TRIANGLE_SPV_DIR`이 설정되지 않아 SKIP되었습니다. 이전의 "조건 미확인"은 해소되었습니다.

주의: 실행 드라이버는 CPU(lavapipe)입니다. 실기기 GPU 결과가 아닙니다.

## T1 Android 준비 (소스 확인)

- Linux 전용 API 사용처 확인 (vulkan-backend.md 3절)
- memfd_create API 30 확인 (android-port.md)
- 동기화 규칙과 Henge 차이 확인 (vulkan-backend.md 5절)

## T2 NDK 빌드 (미실행)

NDK를 로컬에서 받을 수 없습니다. GitHub 러너 워크플로로 진행해야 합니다.

## T3 실기기 Vulkan (미실행)

실기기 접근이 없습니다. 확인 항목: Vulkan 장치 탐지, 하드웨어 장치 선택, 독립 렌더링 결과.

## T4 Henge 호환성 분석 (부분 완료, 소스 확인)

- 프레이밍과 명령 번호 대조: pvg-protocol.md 완료
- 셰이더 변환 경로 대조: metal-shader.md 완료
- 남은 항목: 명령 네임스페이스 확정, 헤더 파서 최종 확인

## 외부 구현의 시험 상태

| 프로젝트 | 시험 존재 | 실행 여부 |
|---|---|---|
| libapplegfx-vulkan | 단위·통합 시험 75개 | T0에서 실행 확인 |
| pvg-vulkan | 통합 시험(`vk_engine_parity.rs` 62개, `vk_engine_compute.rs` 22개, 11개는 비공개 캡처 필요로 #[ignore]) | **미실행** (ICD 필요, 로그 없음) |
| reims-vgpu | `#[test]` 존재 (vulkan 1245, graphics 191 위치) | **미실행** |
| metal2vulkan | 저장소 내 시험 확인 안 함 | **미실행** |
| Indium | 확인 안 함 | **미실행** |

"시험 존재"는 소스에 시험 함수가 있다는 뜻일 뿐, 통과를 뜻하지 않습니다.
