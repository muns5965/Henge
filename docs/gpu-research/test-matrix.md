# 단계별 시험 결과

환경: 샌드박스 Linux(Ubuntu 24.04), Meson debug, `-Dtests=true`, 커밋 7574eac0 (libapplegfx-vulkan).

## T0 기준선 (실행 완료)

| 항목 | 결과 |
|---|---|
| 설정 | 성공, Vulkan 1.3.275 탐지, 실제 Vulkan 경로 활성화 |
| 빌드 | 241/241 성공 |
| 단위 시험 | 75개 중 72 OK, 3 SKIP, 0 FAIL |
| CI 필수 Vulkan 시험 | vulkan init, command, render, translate render 4개 PASS |
| SKIP 3건 | shader translate, pipeline build (llc 경로), triangle lavapipe e2e (조건 미확인) |

주의: 실행 드라이버는 CPU(lavapipe)입니다.

## T1 Android 준비 (분석)

- Linux 전용 API 사용처 확인 (vulkan-backend.md)
- memfd_create API 30 확인 (android-port.md)

## T2 NDK 빌드

미실행 (NDK 확보 필요)

## T3 실기기 Vulkan

미실행 (기기 시험 필요)

## T4 Henge 호환성 분석

부분 완료 (pvg-protocol.md). 명령 네임스페이스 확정과 헤더 파서 확인이 남아 있습니다.
