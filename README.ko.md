<p align="center"><img src="docs/icon.png" width="120" alt="Henge"></p>

# Henge — Android 포팅 (실험적)

[English](README.md) · [이전 Android README](docs/archive/README-android-2026-11-25.md) · [원본 README](docs/upstream/README.md) · [원본 프로젝트](https://github.com/Leviidev/Henge)

Leviidev/Henge의 독립 Android/ARM64 포팅 프로젝트입니다. QEMU 기반 재작성 대신 Henge 자체 ARM64 에뮬레이션 엔진을 사용합니다. macOS 데스크톱 부팅은 아직 검증되지 않았습니다.

**기준일: 2026-11-25.** 시험 기기: 삼성 갤럭시 S26 Ultra, Android 17, Adreno 840. 아래 결과는 개발자가 제공한 실기기 시험 결과이며 모든 기기의 호환성을 보장하지 않습니다.

## 그래픽 호환 라이브러리

[libapplegfx-vulkan](https://github.com/muns5965/libapplegfx-vulkan)([시험 리비전](https://github.com/muns5965/libapplegfx-vulkan/tree/7574eac0d7396a9f49366f007c7fbeb44245826c))은 Android ARM64에서 실험적으로 검증된 Metal AIR→SPIR-V 변환 및 Vulkan 그래픽 호환 라이브러리입니다. 독립 GPU 시험 APK에서 교차 컴파일 및 실행을 검증했으며, Henge 실제 PVG 백엔드 연결은 미완료입니다.

[GPU 시험 브랜치](https://github.com/muns5965/Henge/tree/research/gpt-vulkan-render-test) · [4.4 빌드 결과](https://github.com/muns5965/Henge/actions/runs/45575251001) · [4.4 APK 다운로드](https://github.com/muns5965/Henge/actions/runs/45575251001/artifacts/15004136515)

## GPU 호환성 검증표

PASS는 실제 시험 통과, FAIL은 실제 실패, NOT TESTED는 검증 미완료입니다. 음성 대조군의 예상 실패는 정상 시험 실패와 구분합니다.

| 시험 | 결과 | 근거 |
| --- | --- | --- |
| Android ARM64 빌드 및 실행 | ✅ PASS | S26 Ultra |
| Vulkan 동적 함수 연결 | ✅ PASS | 네이티브 엔진 시험 |
| AIR 삼각형 렌더링(1단계) | ✅ PASS | 3,724픽셀 검증, 불일치 0 |
| 텍스처 샘플링(2단계, GLSL) | ✅ PASS | 4,096/4,096 |
| 알파 블렌딩(3단계, GLSL) | ✅ PASS | 4,096/4,096 |
| FP16 렌더링 및 변환(4단계, GLSL) | ✅ PASS | 원시 및 변환 픽셀 각각 4,096개 |
| 기기 내부 AIR 변환 및 그리기(5단계) | ✅ PASS | 호스트 SHA-256 일치; 3,724/3,724 |
| 외부 엔진 초기화 및 픽셀 읽기(6단계) | ✅ PASS | 4,096/4,096 |
| ColorFill (7단계) | ✅ PASS | 4,096/4,096; RGBA 00FF00FF |
| TextureCopy (8단계) | ✅ PASS | 4,096/4,096; RGBA 4080FFFF |
| APK 원본 .bc 패키징 (4.2 / control B) | ❌ FAIL | 설치 거부 |
| APK .airdata 패키징 (4.3/4.4) | ✅ PASS | 설치 및 실행 성공 |
| ColorFill Draw 생략 대조군 | ❌ FAIL (예상된 실패) | 4,096개 불일치, 종료 코드 1, 대조군 검증 PASS |
| TextureCopy Draw 생략 대조군 | ❌ FAIL (예상된 실패) | 4,096개 불일치, 종료 코드 1, 대조군 검증 PASS |
| 실제 로그인 정점·프래그먼트 조합 | ⬜ NOT TESTED | 7~8단계 GLSL 기준 정점 사용 |
| 패턴 텍스처·UV 좌표 정확성 | ⬜ NOT TESTED | 8단계 균일 색상 사용 |
| UberComposite·Vfx·Xgc 렌더링 | ⬜ NOT TESTED | 변환만 검증 |
| Henge 실제 PVG 통합 및 macOS 데스크톱 | ⬜ NOT TESTED | 독립 GPU APK 시험 |

## macOS 로그인 셰이더 검증

AIR 모듈 11종, 정의된 함수 66개 변환 성공 보고. 생성 SPIR-V는 Linux 호스트 SHA-256과 일치하고 호스트 Vulkan 1.2 spirv-val을 통과했습니다. 변환·구조 검증은 Metal 전체 의미론 검증과 다릅니다. Xgc 크기는 주 함수 기준이며 나머지 55개 함수는 별도 변환되었으나 GPU 개별 실행은 미검증입니다.

| 함수/모듈 | 단계 | 변환 | 호스트 해시·규격 | 주 SPIR-V 바이트 | GPU 출력 |
| --- | --- | --- | --- | ---: | --- |
| ViewportToNDC | 정점 | 1/1 ✅ PASS | ✅ PASS | 2,056 | ⬜ NOT TESTED |
| UberCompositeVertex | 정점 | 1/1 ✅ PASS | ✅ PASS | 1,952 | ⬜ NOT TESTED |
| Vfx | 정점 | 1/1 ✅ PASS | ✅ PASS | 2,136 | ⬜ NOT TESTED |
| ColorFill | 프래그먼트 | 1/1 ✅ PASS | ✅ PASS | 900 | ✅ PASS |
| TextureCopy | 프래그먼트 | 1/1 ✅ PASS | ✅ PASS | 1,136 | ✅ PASS |
| UberCompositeFragment A | 프래그먼트 | 1/1 ✅ PASS | ✅ PASS | 1,544 | ⬜ NOT TESTED |
| UberCompositeFragment B | 프래그먼트 | 1/1 ✅ PASS | ✅ PASS | 1,524 | ⬜ NOT TESTED |
| VfxU11 | 정점 | 1/1 ✅ PASS | ✅ PASS | 2,940 | ⬜ NOT TESTED |
| TvcmXc_Isrc | 프래그먼트 | 1/1 ✅ PASS | ✅ PASS | 6,740 | ⬜ NOT TESTED |
| VfxXgb | 정점 | 1/1 ✅ PASS | ✅ PASS | 10,208 | ⬜ NOT TESTED |
| Xgc | 프래그먼트 | 56/56 ✅ PASS | ✅ PASS | 337,892 | ⬜ NOT TESTED |

7~8단계는 기기에서 새로 변환한 실제 Metal 프래그먼트와 GLSL 기준 정점을 사용했습니다. 64×64 픽셀씩 총 8,192개 전부 일치했으며, Draw 생략 대조군은 8,192개 전부 불일치했습니다.

## Android VM 개발 현황

[research/android-port](https://github.com/muns5965/Henge/tree/research/android-port)에서 Android NDK 빌드, 네이티브 실행기, JIT/MMU, 복원 통신, 희소 디스크·AUX, 복제본 부팅, 공유 메모리 미리보기 및 진단 기능을 개발하고 있습니다.

| VM 시험 | 결과 |
| --- | --- |
| ASR 시스템 이미지 전송·검증 | ✅ PASS |
| Cryptex 및 RecoveryOS 설치 | ✅ PASS |
| 부팅 정책 및 부팅 가능 볼륨 설정 | ✅ PASS |
| 시스템 볼륨 봉인 및 복원 완료 | ⬜ NOT TESTED — 완료 미확인 |
| 복제본 iBoot Stage 1 및 2 | ✅ PASS (후속 실기기 보고) |
| macOS 정상 데스크톱 부팅 | ⬜ NOT TESTED — 성공 사례 미확인 |
| 화면·입력·오디오·네트워크 전체 호환 | ⬜ NOT TESTED |

게스트 시험 환경: macOS 15.6.1 (24G90), VirtualMac2,1. 원본 디스크와 AUX를 보존하고 쓰기 시험은 복제본을 사용하십시오. [G7 분석](docs/g7-analysis-notes.md), [Android G7 빌드](.github/workflows/android-g7exec.yml), [Actions](https://github.com/muns5965/Henge/actions). 아티팩트는 만료되거나 로그인이 필요할 수 있습니다.

## 출처 및 라이선스

원본 Henge: [Leviidev](https://github.com/Leviidev). Henge는 GPL-2.0-or-later이며 [LICENSE](LICENSE), [라이선스 안내](docs/licensing.md)를 참조하십시오. 외부 라이브러리의 저작권·라이선스는 해당 저장소를 따릅니다. macOS와 Mac 부트 ROM은 Apple 소유입니다. Metal 전체 API 또는 macOS 데스크톱 호환을 주장하지 않습니다.
