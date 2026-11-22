# PVG 프로토콜 대조

읽기 전용 함수 단위 분석 기록입니다. 커밋: Henge research/android-port 1069b98, libapplegfx-vulkan 7574eac0, pvg-vulkan (medfa12 저장소) 59fd73a. 경로는 각 저장소 루트 기준입니다.

## 0. 요약

헤더 레이아웃은 세 출처가 서로 다르게 정의하며, 이전 판단의 "양쪽 모두 12바이트"는 정확하지 않았습니다. 12바이트인 것은 여러 곳에 있지만 필드의 의미가 다릅니다.

| 출처 | 계층 | 레이아웃 |
|---|---|---|
| Henge 채널 ring | 외곽 명령 | +0 u32에서 하위 16비트 = opcode, 상위 16비트 = 배리어 개수 (skip = 개수×8), +4 length, +8 stamp |
| libapplegfx ch0 (라이브) | 외곽 명령 | +0 u16 opcode, +2 u16 arg_count_8b, +4 u32 length, +8 u32 stamp, payload +12 |
| libapplegfx upstream-pr | 외곽 명령 (별도 트리) | `lagfx_fifo_parse_header` 정의 (12바이트) |
| pvg-vulkan 자식 FIFO | 패킷 | +0 u16 opcode, +2 u16 stamp_count, +4 u32 total_size, +8 u32 completion_stamp, 레코드 stamp는 +12부터 8바이트×count |
| pvg-vulkan-wire op | 직렬화 레코드 | +0 u32 opcode, +4 u32 length, payload +8 (12바이트 해석은 인코더 레코드를 잘못 읽어 폐기됨) |

**판단:** 12바이트 레이아웃끼리도 +2, +4, +8의 의미가 다릅니다. libapplegfx ch0와 pvg-vulkan 자식 FIFO는 크기가 같지만 +2는 arg_count와 stamp_count, +4는 length와 total_size로 다릅니다. 바이트 비교는 계층과 필드 의미를 확인한 뒤에만 가능합니다.

## 1. 링에서 처음 읽는 위치

| 구현 | 위치 | 읽는 순서 | opcode | length | stamp |
|---|---|---|---|---|---|
| Henge root ring | devices/pvg_lite.c:136 | +0, +4, +8 (4바이트 단위, 링 끝 wrap 처리) | +0 하위 16비트 (143, 148) | +4 (137) | +8 |
| Henge 채널 ring | devices/pvg_lite.c:867 | +0, +4, +8 (바이트 단위 `ring_read`) | +0 하위 16비트 (784) | +4 (870) | +8 (924) |
| Henge 채널 추가 읽기 | pvg_lite.c:902, 811 | 배리어 레코드 rd+12+8k (metal일 때만), payload rd+12+skip | 상위 16비트 = 개수 (784) | | |
| libapplegfx ch0 | src/dispatchers/channel_0_dispatcher.c:419–422 | 12바이트 한 번에 (wrap 시 DMA 두 번) | u16 +0 | u32 +4 | u32 +8 |
| libapplegfx payload | channel_0_dispatcher.c:468–470 | +12 고정, skip 없음 | | | |
| pvg-vulkan 자식 FIFO | vendor/pvg-vulkan/crates/pvg-vulkan/src/runtime/drain/mod.rs:1418–1427 | +0 u16 opcode, +2 u16 stamp_count, +4 u32 total_size, +8 u32 completion_stamp | | | |

- libapplegfx에서 이전에 "실제 파서"로 지목한 `lagfx_fifo_parse_header`는 `upstream-pr/src/protocol/fifo.c:52`에 정의되어 있습니다. `src/protocol/`에는 없습니다. 호출처는 `upstream-pr/src/protocol/fifo.c:161`, `protocol.c:219`입니다.
- 라이브 ch0 경로(`src/dispatchers/channel_0_dispatcher.c`)는 이 함수를 호출하지 않고 419–422행에서 인라인으로 파싱합니다.
- pvg-vulkan의 `decode/fifo.rs`에는 헤더를 읽는 코드가 없고 자식 레코드 상수만 있습니다. 헤더 framing은 `drain/mod.rs`에 있습니다 (`fifo.rs:15–17` 주석).

## 2. 같은 링을 읽는가

- **레지스터 맵은 같습니다.** Henge `pvg_lite.c:121–124`와 libapplegfx `doorbell.c:154, 168, 200`는 모두 0x1004(크기), 0x1008(쓰기 포인터), 0x1010(시작 오프셋), 0x1030(base PFN)을 씁니다.
- **기하는 다릅니다.**
  - PFN 단위: Henge는 `<<14`(16K, `pvg_lite.c:121`), libapplegfx는 `<<12`(4K, `doorbell.c:168, 200`)
  - 링 길이: Henge는 `size - off`(`pvg_lite.c:124`), libapplegfx는 base에 시작 오프셋을 더하고 전체 크기를 씀(`doorbell.c:168`)
  - 페이지 분산: Henge는 16KB 테이블 간접 참조(`pvg_lite.c:167–178`), libapplegfx ch0는 ring_base+rp로 연속이라고 가정(`channel_0_dispatcher.c:369, 387`). 반면 libapplegfx compute 드레이너는 데이터 페이지가 물리적으로 연속이 아니라고 주석합니다.
  - 게스트 구조: libapplegfx는 x86-64 리틀엔디언을 가정합니다(`src/protocol/ops_misc.c:44`). pvg-vulkan `regs.rs:291, 293`은 ARM64E와 x86 페이지 크기를 모두 정의합니다.
- **두 구현이 같은 파일에 나오지 않습니다.** libapplegfx에는 Henge를 가리키는 문자열이 없습니다.

## 3. 숫자 opcode 비교 (계층 확인 전에는 판정 불가)

| opcode | Henge | libapplegfx | pvg-vulkan | 비고 |
|---|---|---|---|---|
| 0x01 | display set page (`pvg_lite.c:808–811`) | 채널 0: DELETE_TASK. display 채널: setupSharedState (`channel_display_dispatcher.c:61`) | 자식 레코드: DRAW (렌더 인코더) | 세 계층에서 다른 의미 |
| 0x02 | display ack | MAP_MEMORY2. display 채널: displaySubmit (`:64`) | DRAW_INSTANCED_WIDE | 같은 패턴 |
| 0x08 | display swap | DELETE_RESOURCE | DRAW_INDEXED_INSTANCED_WIDE | 같은 패턴 |
| 0x38 | 작업 루트 등록: task +0, root page +12 (`pvg_lite.c:143–146`) | DEFINE_HOST_TASK: task id +0, root page +12 (`channel_0_dispatcher.c:298–312`) | DEFINE_TASK2 (`regs.rs:513`) | **페이로드 배치 일치**, 번호 일치 |
| 0x44 | 작업 객체 목록: task +0, objects u64 +4, placement u64 +12, 크기 +20/+24, 최소 28바이트 (`pvg_lite.c:793–801`) | 없음 | SET_OBJ_AND_PLACE (`drain/mod.rs:4722–4735`, `regs.rs:553`) | **배치 일치**, libapplegfx 쪽 없음 |
| 0x42 | 없음 | 동기화 (opcodes.h 주석, `ch0:247`) | 자식 동기화는 0x35 (`regs.rs:509`) | 같은 개념, 번호 다름 |
| 0x37 | 처리 안 함 | compute 채널의 exec (`compute:97–98`) | 자식 EXEC_INDIRECT2 (`regs.rs:512`) | 번호 같음, 채널 다름 |
| 0x43 / 0x45 | tally 집계 (`pvg_lite.c:765–770, 806`), metal이면 반환 (807), 없으면 버림 (808) | enum 없음, ch0·compute switch에 case 없음 (`ch0:335–338`, `compute:295–296`) | EXEC_INDIRECT3 (`regs.rs:550, 556`) | 0x43/0x45 배치는 pvg-vulkan EXEC_INDIRECT3과 맞음 (`regs.rs:548–549`) |

**정정 기록**
- 0x43과 0x45를 "진단 전용"으로 본 초기 판단은 틀렸습니다. Henge에서는 tally가 켜졌을 때만 집계하고, metal 백엔드가 있으면 `pvg_metal.m:2497–2498`에서 실행 경로로 넘깁니다.
- 0x38과 0x44는 libapplegfx와 pvg-vulkan 사이에서 페이로드 배치가 맞습니다. 번호 충돌이 없다고 판정하는 것이 아니라, 같은 번호라도 계층이 다르면 의미가 다를 수 있다는 점은 그대로 둡니다.
- "ExecIndirect3" 번호는 libapplegfx `upstream-pr/src/protocol/opcodes.c:104`에서 0x21, pvg-vulkan에서는 0x43/0x45 계열로 서로 다릅니다.

## 4. 8바이트 내부 헤더가 적용되는 경로

- libapplegfx의 8바이트 내부 헤더 처리는 `lagfx_compute_exec_cmdbuf` 안에만 있습니다 (`src/handlers/compute/exec_cmdbuf.c:480`). 진입은 외부 0x20(`ch0:107–108`, `compute:82–83`)과 0x37(`compute:97–98`)뿐입니다.
- 워커 경로: `exec_cmdbuf.c:172`(inner_walk_segment), `:185–187`(PGCmdHeader {opcode u32, totalLen u32}), `:463`(호출), `:282`(locate_segment_header).
- 외부 0x21(`ch0:242–245`)과 0x41(`ch0:292–293`)은 로그만 남깁니다.
- 외부 헤더의 arg_count_8b는 바이트 건너뛰기에 쓰이지 않습니다 (`opcodes.h:204`, 로그와 `test_api.c:33–39`에서만 사용).
- **반대로** Henge는 모든 외부 명령에 `skip = count×8`을 적용하고(`pvg_lite.c:784, 831`, `pvg_metal.m:2492`), pvg-vulkan도 `min_payload_off = 12 + 8×stamp_count`를 씁니다(`drain/mod.rs:1442`). 상위 16비트가 0이 아니면 libapplegfx ch0 파서는 payload를 어긋나게 읽습니다.
- 0x20 페이로드 배치: libapplegfx는 24바이트 테이블을 descriptor, 16바이트를 resource로 읽고(`exec_cmdbuf.c:502–504, 546–548`), pvg-vulkan 0x37은 24바이트를 resource, 16바이트를 cmdbuf로 읽습니다(`fifo.rs:106–116`). 바이트 배치는 같고 이름만 다릅니다.

## 5. 비교 키

명령 번호만으로 비교하지 않습니다. `(계층, 채널 또는 레코드 종류, 헤더 필드 의미, opcode)` 네 값으로 비교합니다.

- 계층 A: 외곽 채널 명령 (Henge `channel_command`, libapplegfx `channel_0_dispatcher.c`)
- 계층 B: 자식 FIFO 패킷 (pvg-vulkan `drain/mod.rs`)
- 계층 C: 직렬화 레코드 (pvg-vulkan-wire `op`)

libapplegfx의 0x20 외부 명령과 pvg-vulkan의 자식 FIFO가 같은 계층인지는 아직 확인하지 못했습니다.

## 6. 미확인 항목

1. 게스트가 ch0 명령의 word0 상위 16비트를 0이 아닌 값으로 쓰는지. Henge와 pvg-vulkan은 쓰고, libapplegfx는 무시합니다.
2. PFN 단위(4K 또는 16K)가 어느 게스트 캡처에서 맞는지. libapplegfx는 x86, Henge는 arm64e로 보이지만 캡처 근거는 없습니다.
3. Henge `do_root`는 배리어 개수를 무시합니다(`pvg_lite.c:136–160`). 루트 링에서 개수가 0이 아닌 경우가 있는지 미확인.
4. Henge의 op 1, 2, 8이 어느 채널에서 오는지 미확인.
5. Henge `pvg_mac.m:541`의 `[0x43]=55`, `[0x45]=78` 테이블 용도 미확인.
6. Metal 백엔드가 0x44를 처리하는지 미확인 (`pvg_metal.m`에 0x44 분기 없음, `:1145`는 다른 의미의 상수).
7. libapplegfx compute default 경로(`compute:295–296`)에 0x43–0x45가 실제로 도달하는지 실행으로 확인하지 않음.
8. libapplegfx 0x20 페이로드의 16바이트 테이블 역할(resource 또는 cmdbuf) 미확인.
9. libapplegfx `src/protocol/opcodes.h`와 `upstream-pr/src/protocol/opcodes.h`의 내용이 다릅니다. 빌드 대상이 어느 쪽인지, `upstream-pr/src/protocol/fifo.c`가 src 빌드에 포함되는지 미확인.
10. libapplegfx `state.h:72–82`의 wire 구조체는 `opcodes.h`와 레이아웃이 다릅니다. 실사용 위치 미확인.
11. libapplegfx `doorbell.c`와 Henge `pvg_lite.c:1–12`의 "ring offset" 해석 차이(링 길이에 시작 오프셋 포함 여부)는 실행으로 확인하지 못했습니다.

## 7. 정정 기록 (이전 문서 대비)

- "양쪽 모두 12바이트" → 세 출처의 레이아웃과 필드 의미가 다름 (0절).
- "libapplegfx `lagfx_fifo_parse_header`가 실제 파서로 보인다" → 해당 함수는 `upstream-pr/`에 있고, 라이브 ch0는 인라인 파싱 (1절).
- "0x43/0x45는 진단 전용" → tally와 백엔드 실행 경로가 모두 있음 (3절).
