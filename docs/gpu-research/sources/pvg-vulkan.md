# 조사 기록: pvg-vulkan

- 저장소: medfa12/apple-silicon-anyos-project (HEAD 59fd73aac4ae386939bc69dc58dffbce1b2167b2, 2026-11-22 확인)
- 대상 경로: `vendor/pvg-vulkan/` (Rust 워크스페이스)
- 별도 원본 저장소 여부: 미확인 (이 조사에서는 통합 저장소 내부 경로만 확인)
- 하위 crate: `pvg-vulkan`, `pvg-vulkan-efi`, `pvg-vulkan-paging`, `pvg-vulkan-wire`
- 라이선스: `vendor/pvg-vulkan/LICENSE`는 LGPL-3.0 전문 (Henge의 GPL-2.0-or-later와 결합 조건은 검토 필요)
- 관련성: `pvg-vulkan-wire` crate는 PVG 와이어 형식을 다루는 것으로 보이므로 pvg-protocol.md의 대조 대상입니다. 함수 단위 대조는 아직 하지 않았습니다.
- 미확인: 와이어 헤더의 필드 레이아웃, Henge·libapplegfx와의 명령 번호 대응, 실행 검증 로그
