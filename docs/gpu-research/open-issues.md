# 미해결 문제와 차단 사항

| 번호 | 내용 | 영향 | 차단 여부 | 필요한 것 |
|---|---|---|---|---|
| 1 | Android NDK를 로컬에서 받을 수 없음 | T2 컴파일 | 차단 | GitHub 러너 워크플로 또는 NDK 경로 제공 |
| 2 | 실기기 접근 없음 | T3 | 차단 | 기기에서 확인할 항목 목록 |
| 3 | llc 경로 고정으로 SKIP 2건 | 셰이더 시험 | 부분 차단 | 환경 변수 또는 LLVM 버전 확인 |
| 4 | 헤더 프레이밍이 세 출처에서 다름 (Henge 12B, libapplegfx 12B+8B, pvg-vulkan 8B 레코드와 별도 FIFO 헤더) | PVG 파싱 | 차단 (디스패처 설계 전) | 계층별 대응 확인 (pvg-protocol.md 0절) |
| 5 | 명령 번호 0x01·0x02·0x08이 세 출처에서 서로 다른 뜻 | 디스패치 | 차단 (구현 전) | 계층이 같은지 확인. 같으면 충돌로 판정 |
| 6 | libapplegfx의 0x43·0x44·0x45 처리기 위치 미확인 | 디스패치 | 비차단 | libapplegfx 처리기 탐색 |
| 7 | `lagfx_fifo_parse_header` 위치 확인: `upstream-pr/src/protocol/fifo.c:52`. 실제 ch0 경로는 `channel_0_dispatcher.c:419–422`에서 인라인 파싱 | PVG 파싱 | 비차단 | upstream-pr 파일이 빌드 대상인지 확인 |
| 8 | libapplegfx 상위 규칙(`../mos/CLAUDE.md`) 접근 불가 | 규칙 준수 | 비차단 | 파일 제공 또는 적용 범위 확인 |
| 9 | 명령 제출 경로가 Android에서 no-op (`host/pvg_metal_stub.c`) | 렌더링 결과 | 차단 (구현 시) | Android 백엔드 구현 |
| 10 | 실제 Vulkan 하드웨어 실행 미검증 | 전체 | 차단 | 실기기 시험 |
| 11 | pvg-vulkan (LGPL-3.0), metal2vulkan·reims-vgpu (GPL/LGPL 이중) 결합 조건 | 채택 | 차단 (채택 전) | 법률 검토 (분석 문서는 법률 판단을 하지 않음) |
| 12 | indium 라이선스 0BSD는 LICENSE 허가 문구로 확인. 메타데이터는 미확인 | 채택 | 비차단 | 필요 시 메타데이터 확인 |
| 13 | 함수 단위 비교 완료 (프레이밍·명령·메모리·동기화·셰이더·렌더). 실행 검증 없음 | 채택 | 차단 (채택 전) | 실기기 시험 후 채택 판정 |
| 14 | 완료 스탬프: Henge는 무조건 기록. 역순 완료·32비트 순환·동시 완료 경쟁 미재현 | 동기화 정확성 | 차단 (수정 전) | vulkan-backend.md 5절의 재현 시험 4개 실행 |
| 15 | 셰이더 변환 실패 시 stub SPIR-V 폴백 존재 (libapplegfx `translate.c:198–210`) | 렌더 결과 오판 | 차단 (Henge 통합 전) | 실패 원인과 미지원 명령을 구분해 보고하는 경로 설계, 폴백 금지 |
| 16 | pvg-vulkan 실행 결과 없음 (통합 시험 미실행) | 엔진 선정 | 차단 (선정 전) | ICD 환경에서 시험 실행 |
| 17 | Rust/C ABI 경계 (pvg-vulkan, metal2vulkan은 Rust) | 통합 구조 | 차단 (구조 확정 전) | FFI 경계 설계, 메모리 소유권 규칙 |
| 18 | libapplegfx 렌더 연결 범위: Vulkan 호출 경로는 draw 5개, blit 2개뿐 | 기능 범위 | 비차단 (범위 판단) | 필요한 opcode 목록을 작업별로 정리 |
| 19 | Android에서 mremap 별칭 동작 미확인 (libapplegfx `task.c:251–260`) | 메모리 | 비차단 (독립 렌더링 시험에는 불필요) | 메모리 별칭 런타임 시험 (S10) |
