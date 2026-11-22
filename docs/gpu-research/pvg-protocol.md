# PVG 프로토콜 대조

## 0. 요약 (정정 포함)

헤더 프레이밍은 세 프로젝트가 서로 다르게 정의합니다. 이전 판단에서 "양쪽 모두 12바이트"라고 적었던 것은 정확하지 않았으며, 아래 표로 정정합니다.

| 출처 | 프레이밍 | 필드 | 근거 |
|---|---|---|---|
| Henge (research) | 12바이트 외곽 헤더 | 첫 32비트의 하위 16비트 = opcode, 상위 16비트 = 배리어 개수(×8바이트) | devices/pvg_lite.c `channel_command` |
| libapplegfx-vulkan | 12바이트 외곽 헤더 + 8바이트 내부 헤더 | opcode(u16), arg_count_8b(u16), length(u32), stamp(u32) | src/protocol/opcodes.h, state.h 주석 |
| pvg-vulkan-wire | 8바이트 record head (직렬화기 레코드) | opcode(u32), length(u32), payload는 +8부터 | crates/pvg-vulkan-wire/src/op.rs 주석 |
| pvg-vulkan FIFO 패킷 | 별도 패킷 헤더 (op.rs 주석 기준) | opcode(u16), stamp count(u16), total size, completion stamp | op.rs 주석, 필드 세부는 미확인 |

pvg-vulkan-wire의 op.rs는 12바이트 해석을 시도했다가 인코더 레코드에서 페이로드 4바이트가 잘리는 문제로 8바이트로 바꾼 이력을 적어 두었습니다. 또한 FIFO 패킷 헤더와 상수를 서로 옮겨 쓰지 말라고 명시합니다.

**판단:** 세 정의를 하나의 와이어 포맷으로 취급하면 안 됩니다. 어느 계층(외곽 채널 명령, 직렬화 레코드, FIFO 패킷)을 비교하는지 먼저 정해야 합니다.

## 1. libapplegfx-vulkan 헤더 정의 (커밋 7574eac0)

| 정의 | 위치 | 크기 | 상태 |
|---|---|---|---|
| `lagfx_cmd_header_wire_t` | src/protocol/state.h:81 | packed, 필드 합 13바이트 (주석은 12바이트) | 코드에서 참조되지 않음 |
| `lagfx_inner_cmd_header_t` | src/protocol/state.h:88 | packed | 참조 위치 미확인 |
| `lagfx_cmd_header_t` | src/protocol/opcodes.h | 12바이트 (`LAGFX_CMD_HEADER_BYTES`), 4개 와이어 필드 | 17개 파일에서 사용 |

- `opcodes.h`: opcode(0), arg_count_8b(2), length(4), stamp(8).
- `state.h`: opcode, length, stamp, arg_count(8비트), payload_size. 레이아웃이 다릅니다.
- `lagfx_fifo_parse_header()`는 구현을 찾지 못했고 `opcodes.h` 주석에만 나옵니다. 실제 파서는 미확인입니다.

**판단:** 실사용 후보는 `opcodes.h` 정의입니다. `state.h`의 packed 정의는 미사용이므로 유지보수 결함으로 분류합니다.

## 2. Henge 명령 처리 (research/android-port, 커밋 082a4ebe, devices/pvg_lite.c)

- 채널 명령 경로(`channel_command`)에서 처리하는 opcode:
  - 0x38: 작업 루트 등록 (16바이트 인수)
  - 0x44: 작업 객체 목록 등록 (28바이트 인수)
  - 0x43, 0x45: tally 플래그가 켜졌을 때 집계 (`tally_exec`)
  - 1, 2, 8: 디스플레이 페이지 설정, ack, swap (8번은 40바이트 미만이면 무시)
- 백엔드 제출 경로가 존재합니다 (`p->be->submit`). 호출 라인은 이 문서 작성 시점에 재확인하지 않았습니다.
- Android 측 Metal 스텁(`host/pvg_metal_stub.c`)은 `create`가 NULL을 반환하고 `submit`은 no-op입니다. 따라서 Android에서 제출 경로는 실질적인 GPU 실행으로 이어지지 않습니다.

## 3. 명령 번호 대조 (세 출처)

| opcode | Henge (채널 명령) | libapplegfx (opcodes.h) | pvg-vulkan-wire (OPCODE_*) | 판단 |
|---|---|---|---|---|
| 0x01 | 디스플레이 페이지 설정 | DELETE_TASK | DRAW | 세 계층의 의미가 모두 다름 |
| 0x02 | 디스플레이 ack | MAP_MEMORY2 | DRAW_INSTANCED_WIDE | 세 계층의 의미가 모두 다름 |
| 0x08 | 디스플레이 swap | DELETE_RESOURCE | DRAW_INDEXED_INSTANCED_WIDE | 세 계층의 의미가 모두 다름 |
| 0x38 | 작업 루트 등록 | DEFINE_HOST_TASK | 정의 없음 | libapplegfx와 개념 유사, 인수 비교 미완료 |
| 0x43 | tally 집계, 백엔드 제출 | 이름 있는 상수 없음 | 정의 없음 | 미확인 |
| 0x44 | 작업 객체 목록 | 이름 있는 상수 없음 | 정의 없음 | 미확인 |
| 0x45 | tally 집계, 백엔드 제출 | 정의 없음 | 정의 없음 | 미확인 |

**해석:** 같은 번호가 세 출처에서 다른 뜻인 것은 계층이 다르기 때문으로 보입니다. pvg-vulkan의 OPCODE_*는 렌더 인코더 레코드이고, libapplegfx는 외곽 명령 버퍼, Henge는 채널 명령입니다. 따라서 "번호 충돌"은 디스패치 오류로 확정할 수 없고, 계층이 같은지 확인해야 하는 문제입니다.

**정정 기록:**
- 초기 분석은 0x43과 0x45를 진단 전용으로 보았으나, 백엔드 제출 경로가 있으므로 "tally 활성화 시 집계, 백엔드 존재 시 제출"로 정정합니다.
- 초기 분석은 헤더를 양쪽 모두 12바이트라고 보았으나, 위 0절의 세 가지 프레이밍으로 정정합니다.

## 4. 비교 키

명령 번호만으로는 비교하지 않습니다. `(계층, 채널 또는 레코드 종류, 명령 형식, opcode)`로 비교합니다.

- 계층 1: 외곽 채널 명령 (Henge `channel_command`, libapplegfx `channel_0_dispatcher.c`)
- 계층 2: 직렬화 레코드 (pvg-vulkan-wire `op`)
- 계층 3: FIFO 패킷 (pvg-vulkan `decode::fifo`)

## 5. 미확인 항목

- libapplegfx 외곽 명령과 pvg-vulkan 직렬화 레코드가 같은 계층인지
- pvg-vulkan FIFO 패킷 헤더의 정확한 바이트 오프셋
- libapplegfx의 0x43, 0x44, 0x45 처리기 위치
- Henge 백엔드 제출 호출 라인
