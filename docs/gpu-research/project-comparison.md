# 프로젝트 비교와 채택 상태

상태 정의: 채택 (시험·호환성 검토 후 채택) / 조건부 채택 (추가 검증 후 적용) / 참고 (분석 자료로만 사용) / 보류 (의존성·라이선스 등 검토 필요) / 미확인 (근거 부족)

## 대상 프로젝트

| 프로젝트 | 상태 | 근거 |
|---|---|---|
| muns5965/libapplegfx-vulkan (포크, 원본 MattJackson) | 확인됨 | 커밋 7574eac0 (main) |
| muns5965/henge | 확인됨 | 커밋 082a4ebe (research/android-port) |
| pvg-vulkan | 미확인 | 소유자·URL이 지정되지 않음 |
| metal2vulkan | 미확인 | 소유자·URL이 지정되지 않음 |
| reims-vgpu | 미확인 | 소유자·URL이 지정되지 않음 |
| indium | 미확인 | 소유자·URL이 지정되지 않음 |

## 기능별 비교

| 기능 | libapplegfx-vulkan | Henge (research) | 상태 | 근거 |
|---|---|---|---|---|
| Vulkan 초기화 | 실제 경로 구현, CPU 드라이버(lavapipe)에서 시험 통과 | 해당 없음 | 조건부 채택 | T0 시험, 실기기 미검증 |
| 렌더 명령 연결 | 약 10/95 연결 (M5 stage 20%) | 렌더 명령 처리 없음 (display 명령 중심) | 조건부 채택 | CLAUDE.md 54행 |
| PVG 명령 디스패치 | 외부 명령 체계 | 번호 1·2·8·0x38·0x44·0x43·0x45 처리 | 보류 | 명령 번호 의미 충돌, pvg-protocol.md 참조 |
| 메모리 aliasing | memfd + MAP_FIXED, mremap 기반 aliasing (Linux 전용) | VM 메모리 함수 접근 | 보류 | vulkan-backend.md 참조 |
| AIR → SPIR-V 변환 | 구현 존재, 일부 시험 SKIP | 해당 없음 | 조건부 채택 | T0 SKIP 2건 (llc 경로) |
| Metal 백엔드 (Android) | 해당 없음 | 스텁 (create는 NULL 반환, submit은 no-op) | 미구현 | host/pvg_metal_stub.c |
| 화면 출력 | lavapipe 기준 출력 성공 보고 (README) | display 명령 처리 존재 | 보류 | 실기기 미검증 |

## 선정 결정 (현재 단계)

- 기존 Vulkan 렌더링 코드는 "직접 사용 후보"로 두고, Android에서 실행 시험을 거쳐 채택 여부를 정합니다.
- 명령 해석과 메모리 연결부는 Henge와 인터페이스가 달라서 "어댑터 필요"로 분류합니다.
- 나머지 4개 프로젝트는 저장소 정보가 확인되기 전까지 "미확인"입니다.
