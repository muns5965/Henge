# Vulkan 백엔드 (렌더링·메모리·동기화)

## 렌더링 상태 (libapplegfx-vulkan, 커밋 7574eac0)

- 렌더 명령은 약 95개이며, 그중 약 10개가 연결되어 있습니다 (`CLAUDE.md`: "~10 of 95 wired (M5 stage 20%)").
- README는 "consolidation of the winning render path to default-on is in progress"라고 기록합니다. 즉 기본 경로 통합은 진행 중입니다.
- 99.7% 프레임 커버리지는 lavapipe 조건입니다.

## 메모리 (Linux 전용 API 사용처)

| API | 발생 건수 (src, include, tools) | 주요 위치 |
|---|---|---|
| memfd_create | 16 | src/memory/task.c |
| mremap | 15 | src/memory/task.c |
| MAP_FIXED | 19 | src/memory/task.c, src/memory/task.h |

- `task.c`에는 Linux 경로(`__linux__`)와 Darwin 경로가 함께 있습니다.
- Linux 경로는 `mremap(old_size=0, MREMAP_MAYMOVE|MREMAP_FIXED)`로 메모리를 aliasing하고, 실패하면 `MAP_FIXED`로 예약 영역을 복구합니다 (task.c 약 259–288행).
- `task.h`에는 `mremap`을 쓰지 않는 이유(QEMU memory_region 불변식)가 기록되어 있습니다.

## 동기화

- 완료 스탬프는 단조 증가해야 합니다 (`CLAUDE.md`).
- 인터럽트는 MSI 벡터 0만 사용합니다 (`CLAUDE.md`).

## Henge와의 차이

- Henge는 VM 메모리를 `hv_phys_ptr(p->m, …)`로 접근합니다 (`devices/pvg_lite.c`).
- libapplegfx는 QEMU 메모리 매핑 구조를 가정합니다.
- 두 방식의 연결부는 어댑터가 필요하며, 현재 "보류"입니다.

## 미확인 항목

- Android에서 `mremap` aliasing이 실제로 동작하는지 (런타임 시험 필요)
- 실기기 Vulkan 하드웨어 경로의 동기화 동작
