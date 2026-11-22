# Android 목표 구조 (초안)

이 문서는 확정된 설계가 아니라 현재까지 확인된 사실로 그린 초안입니다. 외부 엔진의 채택은 아직 정하지 않았습니다. 각 항목의 근거는 해당 문서를 참조합니다.

## 정정 기록

- 초안 1차: libapplegfx-vulkan을 전체 GPU 백엔드의 단독 후보로 봤습니다.
- 함수 단위 분석 후: libapplegfx의 Vulkan 호출 경로는 draw 5개, blit 2개로 확인되었습니다 (조사 기준, 구현률이 아님). pvg-vulkan은 더 넓은 실행 경로가 소스에 있으나 실행은 미확인입니다. 따라서 단독 후보 판단을 철회하고 비교 구조로 바꿨습니다.

## 설계 원칙

1. Henge의 VM, PVG 장치, 게스트 메모리 관리 구조를 우선 유지합니다.
2. 외부 프로젝트의 PVG 프로토콜과 Henge 프로토콜 사이에 어댑터를 둡니다. 프로토콜 차이(프레이밍, 명령 번호)는 어댑터에서 흡수합니다.
3. GPU 실행 엔진과 Metal AIR 변환기는 개별적으로 평가하고 선정합니다.
4. 렌더링 실패를 성공으로 위장하는 stub SPIR-V 폴백은 허용하지 않습니다. 실패 원인과 미지원 명령을 구분해 보고합니다.
5. 실기기 시험에서 확인된 기능만 구현 완료로 판정합니다. CI 통과와 소스 확인은 완료가 아닙니다.

## 구성 요소 (교체 가능한 구조)

```
macOS AppleParavirtGPU (게스트 드라이버)
  ↓
Henge 기존 PVG 장치 (채널·MMIO·완료 스탬프·게스트 메모리)
  ↓
플랫폼 독립 GPU 명령 어댑터 (명령 디코딩·리소스·동기화 계약)
  ↓
셰이더 변환 (metal2vulkan 우선 비교, libapplegfx air2spv, Indium 비교 대상)
  ↓
GPU 실행 엔진 (pvg-vulkan 우선 비교, libapplegfx Vulkan 경로 비교 대상)
  ↓
Android Vulkan GPU → 화면 표시
```

이 그림은 검토할 목표 구조이며, 두 라이브러리의 결합 가능성이 검증되었다는 뜻이 아닙니다.

## 구성 요소별 상태

1. **게스트 명령 입력**: Henge의 PVG 채널 명령 (근거: pvg-protocol.md). 프레이밍은 세 출처가 다릅니다.
2. **명령 디스패처**: 채널 종류·명령 형식·opcode 기준으로 분기합니다. 키 구조는 미정 (`implementation-plan.md` S3).
3. **Vulkan 렌더 백엔드**: libapplegfx 경로와 pvg-vulkan 경로를 비교 대상으로 둡니다. 어느 쪽도 미확정 (근거: vulkan-backend.md 1절).
4. **셰이더 변환**: AIR → SPIR-V 경로가 세 가지입니다. 비교 전까지 미확정 (근거: metal-shader.md).
5. **메모리 연결부**: Henge의 `hv_phys_ptr` 경계 검사를 유지하는 어댑터 (근거: vulkan-backend.md 2~3절). mremap 별칭은 Android 런타임 시험 전까지 필수로 보지 않습니다.
6. **완료 스탬프와 인터럽트**: Henge 무조건 기록의 위험은 재현 시험 대상입니다 (근거: vulkan-backend.md 5절).
7. **화면 출력**: Android 측 표시 경로 (미정, 근거: android-port.md).

## 미결정 사항

- 명령 디스패처의 키 구조 (`(채널, 형식, opcode)`로 할지 여부)
- 헤더 정의 중 어느 것을 실제 와이어 포맷으로 쓸지
- 렌더 백엔드 선택 (libapplegfx 또는 pvg-vulkan 또는 둘 다의 부분 채택)
- 셰이더 변환기 선택 (metal2vulkan, libapplegfx air2spv, Indium)
- Rust 구성요소와 C 본체 사이의 ABI 경계

## 변경 조건

다음 중 하나가 확인되면 이 구조를 갱신합니다.

- 명령 번호 충돌이 실제 디스패치 오류로 확인될 때
- 스탬프 재현 시험에서 역순 완료 또는 순환 오류가 실제로 확인될 때
- Android 실기기에서 Vulkan 하드웨어 경로가 실패할 때
- pvg-vulkan 또는 libapplegfx 실행 시험에서 결과가 확인될 때
- 셰이더 변환의 실제 호환성 결과가 확인될 때
