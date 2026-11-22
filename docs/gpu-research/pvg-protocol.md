# PVG 프로토콜 대조

## 1. 헤더 정의 (libapplegfx-vulkan, 커밋 7574eac0)

| 정의 | 위치 | 크기 | 상태 |
|---|---|---|---|
| `lagfx_cmd_header_wire_t` | src/protocol/state.h:81 | packed, 필드 합 13바이트 (주석은 12바이트) | 코드에서 참조되지 않음 (정의만 존재) |
| `lagfx_inner_cmd_header_t` | src/protocol/state.h:88 | packed | 참조 위치 미확인 |
| `lagfx_cmd_header_t` | src/protocol/opcodes.h | 12바이트 (`LAGFX_CMD_HEADER_BYTES`), 4개 와이어 필드 | 17개 파일에서 사용 |

- `opcodes.h`의 필드 순서: opcode(0), arg_count_8b(2), length(4), stamp(8). `arg_count_8b`는 16비트.
- `state.h`의 필드 순서: opcode, length, stamp, arg_count(8비트), payload_size. 두 정의는 레이아웃이 다릅니다.
- `lagfx_fifo_parse_header()`는 소스에서 구현을 찾지 못했고, `opcodes.h` 주석에만 나옵니다. 실제 파서는 미확인입니다.

**판단:** 실사용 후보는 `opcodes.h`의 정의입니다. `state.h`의 packed 정의는 미사용이므로 유지보수 결함으로 분류합니다.

## 2. Henge 명령 처리 (research/android-port, 커밋 082a4ebe, devices/pvg_lite.c)

- 헤더 첫 32비트: 하위 16비트가 opcode, 상위 16비트는 배리어 개수(×8바이트)로 해석합니다.
- 채널 명령 경로(`channel_command`)에서 처리하는 opcode:
  - 0x38: 작업 루트 등록 (16바이트 인수)
  - 0x44: 작업 객체 목록 등록 (28바이트 인수)
  - 0x43, 0x45: tally 플래그가 켜졌을 때 집계 (`tally_exec`)
  - 1, 2, 8: 디스플레이 페이지 설정, ack, swap (8번은 40바이트 미만이면 무시)
- 백엔드 제출 경로가 존재합니다 (`p->be->submit` 호출). 다만 본 문서 작성 시점에 호출 라인을 재확인하지 않았습니다.
- Android 측 Metal 스텁(`host/pvg_metal_stub.c`)은 `create`가 NULL을 반환하고 `submit`은 아무 일도 하지 않습니다. 따라서 Android에서 제출 경로는 실질적인 GPU 실행으로 이어지지 않습니다.

## 3. 명령 번호 대조

| opcode | Henge (브랜치) | libapplegfx (opcodes.h) | 판단 |
|---|---|---|---|
| 0x01 | 디스플레이 페이지 설정 | DELETE_TASK | 의미 충돌, 네임스페이스 확인 필요 |
| 0x02 | 디스플레이 ack | MAP_MEMORY2 | 의미 충돌, 네임스페이스 확인 필요 |
| 0x08 | 디스플레이 swap | DELETE_RESOURCE | 의미 충돌, 네임스페이스 확인 필요 |
| 0x38 | 작업 루트 등록 | DEFINE_HOST_TASK | 유사 개념으로 보이나 인수 비교 미완료 |
| 0x43 | tally 집계, 백엔드 제출 경로 | 이름 있는 상수 없음 | 미확인 |
| 0x44 | 작업 객체 목록 | 이름 있는 상수 없음 | 미확인 |
| 0x45 | tally 집계, 백엔드 제출 경로 | 정의 없음 | 미확인 |

**정정 기록:** 초기 분석은 0x43과 0x45를 진단 전용으로 보았으나, 백엔드 제출 경로가 있으므로 "진단 전용"이 아니라 "tally 활성화 시 집계, 백엔드 존재 시 제출"로 정정합니다.

## 4. 비교 키

명령 번호만으로는 비교하지 않습니다. `(채널 종류, 명령 형식, opcode)` 세 값으로 비교합니다. 채널 구분은 libapplegfx `channel_0_dispatcher.c`와 Henge `channel_command`의 대응을 확인해야 합니다.

## 5. 미확인 항목

- 두 측 명령 공간이 같은 네임스페이스인지 여부
- libapplegfx의 0x43, 0x44, 0x45 처리기 위치
- Henge 백엔드 제출 호출 라인
