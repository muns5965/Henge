# 조사 기록: reims-vgpu

- 저장소: steelbrain/reims-vgpu (HEAD 818868564e31f9023c074f73840de97542592c91, 2026-11-22 확인)
- 목적(README): QEMU를 사용하는 Linux 위의 x86-64 macOS 게스트에 Vulkan 기반 가상 GPU를 제공합니다. 게스트 드라이버는 프로젝트가 제공해 `reims-ctl`이 OpenCore 이미지에 넣습니다.
- 상태(README): Beta
- 라이선스: README 배지 기준 GPL-2.0-or-later OR LGPL-3.0-or-later (이중 라이선스). LICENSE와 LICENSES 디렉터리 포함.
- 구성: crates, firmware, guest, docs, tests, vendor, xtask 등
- 관련성: libapplegfx-vulkan과 같은 목표(QEMU·Linux 호스트의 macOS 게스트 GPU)를 가지므로 vulkan-backend.md와 pvg-protocol.md에서 대조합니다. 특히 메모리 연결부와 게스트 드라이버 전달 방식이 비교 대상입니다.
- 미확인: 게스트 명령 형식, 메모리 매핑 방식, 시험 결과
