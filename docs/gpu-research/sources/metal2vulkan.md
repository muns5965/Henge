# 조사 기록: metal2vulkan

- 저장소: steelbrain/metal2vulkan (HEAD d9236a8a11d3b90a1aa27b54e2d74d3891c4d2e5, 2026-11-22 확인)
- 목적(README): Metal AIR(metallib 또는 AIR bitcode)을 읽어 Vulkan 파이프라인으로 컴파일하고, Vulkan 장치 위에 Metal API를 제공합니다.
- 라이선스: README 배지와 LICENSE 파일 기준 GPL-2.0-or-later OR LGPL-3.0-or-later (이중 라이선스). LICENSE-GPL-2.0, LICENSE-GPL-3.0, LICENSE-LGPL-3.0 포함.
- 하위 crate: `metal2vulkan`, `metal2vulkan-air`, `metal2vulkan-conformance`, `metal2vulkan-devtools`, `metal2vulkan-runtime`, `metal2vulkan-validation`
- 관련성: libapplegfx의 `src/air2spirv/`(AIR→SPIR-V)와 같은 영역이므로 metal-shader.md에서 비교합니다.
- 미확인: AIR 명령 지원 범위, conformance 시험 결과, libapplegfx 변환기와의 기능 차이
