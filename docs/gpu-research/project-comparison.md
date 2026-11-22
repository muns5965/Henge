# 프로젝트 비교와 채택 상태

두 열을 분리합니다.

- **근거 수준**: 무엇을 확인했는지 (소스 존재, 소스 함수 확인, 실행 시험, 실기기 실행)
- **채택 상태**: 설계 결정 (채택 / 조건부 채택 / 참고 / 보류 / 미확인)

"코드가 존재한다"는 근거 수준이지 채택 결정이 아닙니다.

## 대상 프로젝트

| 프로젝트 | 기준 SHA | 라이선스 | 역할 | 근거 수준 | 채택 상태 |
|---|---|---|---|---|---|
| muns5965/libapplegfx-vulkan (원본 MattJackson) | 7574eac0 (main) | MIT (meson.build) | Vulkan 렌더 후보 | 실행 시험 (lavapipe) | 조건부 채택 |
| muns5965/henge | 082a4ebe (research/android-port) | GPL-2.0-or-later | 최종 통합 대상 | 소스 함수 확인 | 기준 |
| pvg-vulkan (medfa12/apple-silicon-anyos-project, vendor/pvg-vulkan) | 59fd73aa (저장소 HEAD) | LGPL-3.0 | PVG 와이어 참고 | 소스 함수 확인 (op.rs 프레이밍) | 보류 (라이선스 검토) |
| metal2vulkan (steelbrain) | d9236a8a | GPL-2.0-or-later OR LGPL-3.0-or-later | AIR→Vulkan 참고 | README 확인 | 보류 (라이선스 선택) |
| reims-vgpu (steelbrain) | 81886885 | GPL-2.0-or-later OR LGPL-3.0-or-later | QEMU·macOS 게스트 GPU 참고 | README 확인 | 보류 (라이선스 선택) |
| indium (darlinghq) | 1bf59a85 | 0BSD (LICENSE 허가 문구 확인) | Metal API의 Vulkan 구현 참고 | 소스 미확인 | 참고 |

SHA는 저장소 HEAD 기준입니다. 0BSD는 LICENSE 본문의 허가 문구로 확인했으며, GitHub 메타데이터는 확인하지 않았습니다. 0BSD라는 사실이 기술적 적합성을 뜻하지는 않습니다.

## 기능별 비교

| 기능 | libapplegfx-vulkan | Henge (research) | 기타 후보 | 근거 수준 | 채택 상태 |
|---|---|---|---|---|---|
| Vulkan 초기화 | 실제 경로 구현 | 해당 없음 | reims-vgpu (Vulkan 가상 GPU) | lavapipe 실행 시험 통과 | 조건부 채택 |
| 렌더 명령 연결 | 약 10/95 연결 | 렌더 명령 처리 없음 | - | CLAUDE.md 54행 | 조건부 채택 |
| PVG 명령 디스패치 | 외곽 명령 체계 | 채널 명령 1·2·8·0x38·0x44·0x43·0x45 | pvg-vulkan-wire 직렬화 레코드 | 소스 함수 확인 | 보류 (계층 대조 필요) |
| 메모리 aliasing | memfd + MAP_FIXED, mremap (Linux 전용) | VM 메모리 함수 접근 | reims-vgpu (방식 미확인) | 소스 함수 확인 | 보류 |
| AIR → SPIR-V 변환 | 구현 존재, 일부 시험 SKIP | 해당 없음 | metal2vulkan (AIR→Vulkan 파이프라인) | 일부 시험 PASS, 2건 SKIP | 조건부 채택 |
| Metal API 표면 구현 | 해당 없음 | 해당 없음 | indium (Vulkan 위 Metal C++ 유사 API) | README 확인 | 참고 |
| Metal 백엔드 (Android) | 해당 없음 | 스텁 (create NULL, submit no-op) | - | 소스 확인 | 미구현 |
| 화면 출력 | lavapipe 기준 출력 성공 보고 | display 명령 처리 존재 | reims-vgpu (macOS 게스트 화면) | 보고 수치(README), 실기기 미검증 | 보류 |

## 선정 결정 (현재 단계)

- libapplegfx-vulkan의 Vulkan 렌더링 코드는 직접 사용 후보입니다. 실기기 시험을 거쳐 채택 여부를 정합니다.
- 명령 해석과 메모리 연결부는 Henge와 인터페이스가 달라 "어댑터 필요"로 분류합니다.
- metal2vulkan과 libapplegfx의 셰이더 변환은 같은 영역입니다. 기능 비교 전까지 어느 쪽도 채택하지 않습니다.
- pvg-vulkan과 reims-vgpu는 QEMU·macOS 게스트 목표가 libapplegfx와 겹치므로, 함수 단위 비교 후 채택 여부를 정합니다.
- 라이선스: Henge는 GPL-2.0-or-later입니다. LGPL-3.0(pvg-vulkan)과 이중 라이선스(metal2vulkan, reims-vgpu)의 결합 조건은 법률 검토가 필요하며, 이 문서는 법률 판단을 대신하지 않습니다.
- 이번 단계까지 함수 단위 비교는 PVG 프레이밍과 명령 번호 대조만 수행했습니다. 메모리·셰이더·렌더링 비교는 남아 있습니다.
