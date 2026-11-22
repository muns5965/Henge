# 조사 기록: indium

- 저장소: darlinghq/indium (HEAD 1bf59a850b56087ce37026b1bbd7b1eafdf7e6e5, 2026-11-22 확인)
- 목적(README): Vulkan 위에 구현한 Metal 라이브러리입니다. 공식 Metal C++ API와 비슷한 C++ API를 제공하며, ABI 호환 Metal 구현의 내부 라이브러리로 쓰이도록 설계되었습니다. 기존 코드에 대한 drop-in 대체재가 아닙니다.
- 요구 사항(README): Vulkan 1.3을 기준으로 하며, 동적 상태 확장(`VK_EXT_extended_dynamic_state`, `…2`)을 대체로 쓸 수 있다고 적혀 있습니다.
- 라이선스: LICENSE 파일의 저작권 표기는 Darling developers이며, 라이선스 종류는 이번 조사에서 확인하지 못했습니다 (미확인).
- 구성: CMakeLists.txt, include, private-include, src, test, devnotes.md
- 관련성: Metal API 구현 방식 비교 대상입니다. 셰이더 변환이 아니라 Metal API 표면의 Vulkan 구현이므로 vulkan-backend.md에서 다룹니다.
- 미확인: 라이선스 전문, 구현된 Metal API 범위, 시험 결과
