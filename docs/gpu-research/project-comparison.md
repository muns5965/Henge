# 프로젝트 비교와 채택 상태

상태 정의: 채택 (시험·호환성 검토 후 채택) / 조건부 채택 (추가 검증 후 적용) / 참고 (분석 자료로만 사용) / 보류 (의존성·라이선스 등 검토 필요) / 미확인 (근거 부족)

## 대상 프로젝트

| 프로젝트 | 기준 SHA | 라이선스 | 역할 | 상태 |
|---|---|---|---|---|
| muns5965/libapplegfx-vulkan (포크, 원본 MattJackson) | 7574eac0 (main) | MIT (meson.build) | Vulkan 렌더 후보 | 확인됨 |
| muns5965/henge | 082a4ebe (research/android-port) | GPL-2.0-or-later | 최종 통합 대상 | 확인됨 |
| pvg-vulkan (medfa12/apple-silicon-anyos-project 내 vendor/pvg-vulkan) | 59fd73aa (저장소 HEAD) | LGPL-3.0 | PVG 와이어 참고 | 확인됨, 라이선스 검토 필요 |
| metal2vulkan (steelbrain) | d9236a8a | GPL-2.0-or-later OR LGPL-3.0-or-later | AIR→Vulkan 참고 | 확인됨 |
| reims-vgpu (steelbrain) | 81886885 | GPL-2.0-or-later OR LGPL-3.0-or-later | QEMU·macOS 게스트 GPU 참고 | 확인됨 |
| indium (darlinghq) | 1bf59a85 | LICENSE 전문 미확인 | Metal API의 Vulkan 구현 참고 | 확인됨, 라이선스 미확인 |

SHA는 저장소 HEAD 기준이며, 개별 경로(예: pvg-vulkan)의 최종 수정 커밋은 아직 기록하지 않았습니다.

## 기능별 비교

| 기능 | libapplegfx-vulkan | Henge (research) | 기타 후보 | 상태 | 근거 |
|---|---|---|---|---|---|
| Vulkan 초기화 | 실제 경로 구현, CPU 드라이버(lavapipe)에서 시험 통과 | 해당 없음 | reims-vgpu (Vulkan 가상 GPU) | 조건부 채택 | T0 시험, 실기기 미검증 |
| 렌더 명령 연결 | 약 10/95 연결 (M5 stage 20%) | 렌더 명령 처리 없음 | - | 조건부 채택 | CLAUDE.md 54행 |
| PVG 명령 디스패치 | 외부 명령 체계 | 번호 1·2·8·0x38·0x44·0x43·0x45 처리 | pvg-vulkan-wire (와이어 형식) | 보류 | 명령 번호 의미 충돌, pvg-protocol.md |
| 메모리 aliasing | memfd + MAP_FIXED, mremap (Linux 전용) | VM 메모리 함수 접근 | reims-vgpu (메모리 방식 미확인) | 보류 | vulkan-backend.md |
| AIR → SPIR-V 변환 | 구현 존재, 일부 시험 SKIP | 해당 없음 | metal2vulkan (AIR→Vulkan 파이프라인) | 조건부 채택 | T0 SKIP 2건 (llc 경로) |
| Metal API 표면 구현 | 해당 없음 | 해당 없음 | indium (Vulkan 위 Metal C++ 유사 API) | 참고 | README 기준, 기능 범위 미확인 |
| Metal 백엔드 (Android) | 해당 없음 | 스텁 (create NULL, submit no-op) | - | 미구현 | host/pvg_metal_stub.c |
| 화면 출력 | lavapipe 기준 출력 성공 보고 | display 명령 처리 존재 | reims-vgpu (macOS 게스트 화면) | 보류 | 실기기 미검증 |

## 선정 결정 (현재 단계)

- libapplegfx-vulkan의 Vulkan 렌더링 코드는 직접 사용 후보로 두고, Android 실행 시험을 거쳐 채택 여부를 정합니다.
- 명령 해석과 메모리 연결부는 Henge와 인터페이스가 달라 "어댑터 필요"로 분류합니다.
- metal2vulkan과 libapplegfx의 셰이더 변환은 같은 영역입니다. 기능 비교 전까지 어느 쪽도 채택하지 않습니다.
- pvg-vulkan과 reims-vgpu는 QEMU·macOS 게스트 목표가 libapplegfx와 겹치므로, 구현 함수 단위 비교 후 채택 여부를 정합니다.
- 라이선스 검토: Henge는 GPL-2.0-or-later입니다. LGPL-3.0(pvg-vulkan)과 이중 라이선스(metal2vulkan, reims-vgpu)의 결합 조건은 법률 검토가 필요하며, 이 문서는 법률 판단을 대신하지 않습니다.
- 이번 단계는 저장소 확인과 구조 조사까지이며, 함수 단위 기능 비교는 아직 하지 않았습니다.
