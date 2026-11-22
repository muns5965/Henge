# GPU 연구 문서 색인 (Android GPU 백엔드)

이 디렉터리는 Henge의 Android GPU 구현을 결정하기 위한 기준 문서 모음입니다.
기존 실험·실측 기록은 `docs/agents/` 및 `docs/*.md`에 그대로 두고, 여기서는 링크로만 참조합니다.

## 기준 커밋

| 저장소 | 브랜치 | 커밋 SHA | 비고 |
|---|---|---|---|
| muns5965/henge | research/android-port | 082a4ebe35ea5a50fd94ebb53b6a011277ca7670 | 이 문서의 기준 브랜치 |
| muns5965/henge | main | 17e9ed9de5dd2aead31468c55ad4af0cd8df7a9b | 비교용, 수정 금지 |
| muns5965/libapplegfx-vulkan | main | 7574eac0d7396a9f49366f007c7fbeb44245826c | 포크 기준선 |

## 문서 목록

- [project-comparison.md](project-comparison.md): 기능별 비교와 채택 상태
- [target-architecture.md](target-architecture.md): Android 목표 구조 (초안)
- [pvg-protocol.md](pvg-protocol.md): PVG 명령·헤더·채널 대조
- [metal-shader.md](metal-shader.md): AIR·metallib·SPIR-V
- [vulkan-backend.md](vulkan-backend.md): 렌더링·메모리·동기화
- [android-port.md](android-port.md): NDK·bionic·기기 호환성
- [test-matrix.md](test-matrix.md): 단계별 시험 결과
- [open-issues.md](open-issues.md): 미해결 문제와 차단 사항
- [implementation-plan.md](implementation-plan.md): 구현 순서와 완료 기준
- [sources/](sources/): 저장소별 조사 기록

## 기존 문서와의 관계

| 기존 문서 | 이 문서에서의 역할 |
|---|---|
| docs/architecture.md | Henge 전체 구조의 기준 |
| docs/android-port-notes.md | Android 부팅·장치 호환성 조사 |
| docs/android-install-integration-design.md | 설치 실행기·상태·안전 계약 |
| docs/agents/colorprobe.md | GPU 화면 색상 처리의 실측 기록 |
| docs/g4-real-device-baseline-2026-11-22.md | Android 실기기 부팅 기준선 |
| docs/g6-vs-general-launcher-gap-2026-11-22.md | 복원 실행과 일반 VM 실행의 차이 |

## 작성 규칙

1. 모든 기술 판단은 파일 경로, 함수명, 커밋 SHA 중 하나 이상을 남깁니다.
2. 근거를 확인하지 못했으면 "미확인"으로 적고 추측하지 않습니다.
3. 해석이 바뀌면 최초 판단과 정정 근거를 함께 남깁니다.
4. 기술 판단은 `project-comparison.md`의 상태 체계(채택·조건부 채택·참고·보류·미확인)를 따릅니다.
5. 이 문서는 구현 변경이 아니며, `pvg_lite.c`·VM 메모리·실행 경로를 수정하지 않습니다.
