# Android 목표 구조 (초안)

이 문서는 확정된 설계가 아니라 현재까지 확인된 사실로 그린 초안입니다. 각 항목의 근거는 해당 문서를 참조합니다.

## 구성 요소

1. **게스트 명령 입력**: Henge의 PVG 채널 명령 (근거: pvg-protocol.md)
2. **명령 디스패처**: 채널 종류·명령 형식·opcode 기준으로 분기. 현재 미정 (보류)
3. **Vulkan 렌더 백엔드**: libapplegfx 기존 렌더 경로를 후보로 사용 (근거: vulkan-backend.md)
4. **셰이더 변환**: AIR → SPIR-V (근거: metal-shader.md)
5. **메모리 연결부**: VM 메모리 접근과 aliasing 방식의 어댑터 (보류, 근거: vulkan-backend.md)
6. **화면 출력**: Android 측 표시 경로 (미정, 근거: android-port.md)

## 미결정 사항

- 명령 디스패처의 키 구조 (`(채널, 형식, opcode)`로 할지 여부)
- 헤더 정의 중 어느 것을 실제 와이어 포맷으로 쓸지
- 메모리 aliasing을 mremap으로 유지할지, Henge VM 접근으로 대체할지

## 변경 조건

다음 중 하나가 확인되면 이 구조를 갱신합니다.

- 명령 번호 충돌이 실제 디스패치 오류로 확인될 때
- Android 실기기에서 Vulkan 하드웨어 경로가 실패할 때
- 셰이더 변환의 실제 호환성 결과가 확인될 때
