# 조사 기록: muns5965/henge

- 커밋: research/android-port 082a4ebe35ea5a50fd94ebb53b6a011277ca7670 (main 17e9ed9de5dd2aead31468c55ad4af0cd8df7a9b)
- 이 문서가 추가된 커밋: research/android-port 45c004f12eb73d64b10eaccaed7595a847d32804 (082a4ebe 위에 docs/gpu-research 추가)
- 라이선스: GPL-2.0-or-later (LICENSE, docs/licensing.md 기준). 외부 코드별 라이선스는 docs/licensing.md 표 참조.
- 관련 파일: devices/pvg_lite.c, devices/pvg_lite.h, host/pvg_metal_stub.c, devices/pvg_metal.m, devices/pvg_mac.m
- 확인: 명령 처리 함수 `channel_command`, 0x38·0x43·0x44·0x45·1·2·8 처리, Metal 스텁 no-op
- 플랫폼 가드: 브랜치에서 CoreFoundation include가 `__APPLE__`로 감싸여 있음
- 미확인: 백엔드 제출 호출 라인의 정확한 위치
