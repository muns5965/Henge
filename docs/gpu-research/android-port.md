# Android 이식: NDK·bionic·기기 호환성

## 확인된 사실

- `memfd_create`는 Android bionic에서 API 30(R)부터 제공됩니다. 첫 Android 시험의 최소 API는 30으로 둡니다. 이보다 낮은 API에서는 자체 시스템콜 대체 구현이 필요할 수 있습니다 (libapplegfx `task.c`에 대체 구현 있음).
- GitHub 호스티드 Ubuntu 러너에는 Android SDK가 미리 설치되어 있습니다. 다만 NDK 버전은 러너 이미지마다 다르므로 워크플로에서 버전을 명시해야 합니다.
- 러너 이미지 공지에 따르면 NDK 26이 제거되고 NDK 27이 기본값이 되었습니다. 기본 경로에 의존하지 않습니다.

## 현재 상태

| 항목 | 상태 |
|---|---|
| 로컬 샌드박스의 NDK | 없음, 다운로드 호스트 차단 (403) |
| GitHub 러너 NDK 빌드 | 미실행 |
| 실기기(Galaxy S26 Ultra) Vulkan | 미실행 |
| 기존 Henge Android 부팅 기준 | docs/g4-real-device-baseline-2026-11-22.md 참조 |

## 검증 조건

- 컴파일 성공은 Vulkan 실행 성공을 뜻하지 않습니다.
- CPU 드라이버(lavapipe) 시험 통과는 하드웨어 GPU 검증이 아닙니다.
- `mremap` aliasing과 `MAP_FIXED` 복구는 실기기 런타임 시험으로 따로 확인합니다.
