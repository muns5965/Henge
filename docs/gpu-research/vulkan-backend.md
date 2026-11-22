# Vulkan 백엔드 (렌더링·메모리·동기화)

읽기 전용 함수 단위 분석 기록입니다. 커밋: libapplegfx-vulkan 7574eac0, pvg-vulkan 59fd73a, reims-vgpu 818868564e (요청값 81886885는 실제 HEAD와 다름), Henge research/android-port 1069b98. 이번 분석에서 어떤 테스트도 실행하지 않았습니다. "검증 수준"은 소스 확인과 테스트 함수 존재 여부입니다.

## 1. 렌더링·블릿·컴퓨트 (함수 단위)

### 요약

- libapplegfx: Vulkan 호출에 도달하는 opcode는 draw 5개와 blit 2개뿐입니다. render 테이블 98행은 Vulkan 호출이 0이고 compute dispatch도 0입니다. CLAUDE.md의 `~10 of 95 wired`는 그대로 재현되지 않았습니다 (검증: 소스 확인).
- pvg-vulkan: 가장 넓은 Vulkan 실행 경로(draw, dispatch, copy, render pass, present)가 소스에 있습니다. 실행 로그는 체크아웃에 없습니다.
- reims-vgpu: 자체 Vulkan 크레이트에는 guest draw/blit/dispatch가 없습니다. 실제 인코딩은 외부 의존성 metal2vulkan-runtime에 위임됩니다 (Cargo.toml rev 911692b5 고정).
- Henge: Apple Metal 백엔드만 있고 Android GPU 실행 경로는 없습니다.

### 그리기 (draw)

| 프로젝트 | Vulkan 호출·연결 | 위치 | 실행 증거 | 미확인 |
|---|---|---|---|---|
| libapplegfx | opcode 5개(0x00, 0x01, 0x03, 0x06, 0x07)가 `lagfx_emit_pending_draw`를 거쳐 vkCmdDraw/vkCmdDrawIndexed 호출 | `compute_inner_ops.c:154,205,259,306,365`; `vulkan/draw_record.c:218,223` | 단위시험 m4 draw 12 PASS, render-inner-ops 20 PASS, lavapipe "translate render" PASS(로그). render 테이블 98행은 TODO 로그뿐 | 0x06이 indexed를 unindexed로 그리는 것으로 보임(`compute_inner_ops.c:304-306`). 픽셀 정확성 |
| pvg-vulkan | cmd_draw_indexed, cmd_draw | `backend/vulkan/engine/exec.rs:3985,3995`(함수 1927); 진입 `engine/mod.rs:644,653`; 호출 `runtime/draw/vulkan.rs:8776` | 소스 + 통합시험 `vk_engine_parity.rs`(62 #[test], ICD 없으면 skip). 실행 로그 없음 | 시험 통과 여부. `runtime/exec/mod.rs` 헤더의 "Metal draw attempt"가 Linux에서 쓰이는지 |
| reims-vgpu | reims-vgpu-vulkan에 vkCmdDraw 0건. guest draw는 `gpu/encode_render.rs`가 metal2vulkan_runtime으로 위임 | `encode_blit.rs:122`; `Cargo.toml:36,38` | #[test] 1245(vulkan), 191(graphics). 실행 로그 없음 | 고정 rev의 draw 호출 수. 로컬 metal2vulkan은 d9236a8이라 개수 미확인 |

### 블릿·복사 (blit/copy)

| 프로젝트 | Vulkan 호출·연결 | 위치 | 실행 증거 | 미확인 |
|---|---|---|---|---|
| libapplegfx | 2개: 0x12f → vkCmdCopyImage, 0x141 → vkCmdClearColorImage. 블릿 테이블 31행 중 29행은 ack 스텁 | `blit_encoder.c:211,389`; `blit_inner_ops.c:232,250` | 단위시험 blit-inner-ops 33 PASS(파싱 중심). GPU 결과 검증 없음 | 0x12f/0x141의 GPU 결과 |
| pvg-vulkan | 디코드 50 arms. cmd_copy_buffer_to_image, cmd_copy_image, cmd_copy_buffer, cmd_copy_image_to_buffer 다수 | `exec.rs:1611,3232,3286,3346,3550,4140`; `exec_compute.rs:620,645,725,869`; `engine/mod.rs:2056,3262,3277,3303` | 소스. 실행 증거 없음 | FILL_BUFFER(`decode/blit.rs:494`)에 대응하는 cmd_fill_buffer가 0건. 매핑 미확인 |
| reims-vgpu | reims-vgpu-vulkan의 복사는 cmd_copy_image_to_buffer 1건(표시 readback) | `os/linux/plane_copy.rs:783` | 없음 | 게스트 blit은 `encode_blit.rs`에서 rt로 위임 |

### 컴퓨트 디스패치 (compute dispatch)

| 프로젝트 | Vulkan 호출·연결 | 위치 | 실행 증거 | 미확인 |
|---|---|---|---|---|
| libapplegfx | vkCmdDispatch 0건. 컴퓨트 테이블 16행에 dispatch opcode 없음. 렌더 테이블의 DispatchThreadsPerTile(0x9b, 0xa2, 0xa3)은 trace만 | `render_inner_ops.c:840-850`; `exec_cmdbuf.c:209-213`(주석은 draw 5개 기준으로 낡음) | 없음 | 없음 |
| pvg-vulkan | cmd_dispatch 2건 | `exec_compute.rs:807`(함수 246); 진입 `engine/mod.rs:1147,1157`; 호출 `runtime/compute_exec/mod.rs:4428`; `guest_scatter.rs:596` | 통합시험 `vk_engine_compute.rs`(22 #[test]). 11개는 비공개 캡처 필요로 #[ignore]. 로그 없음 | 시험 통과 여부 |
| reims-vgpu | reims-vgpu-vulkan에는 0건. 비디오(VA-API) 경로에만 cmd_dispatch | `reims-vgpu-video/.../spin.rs:811` | 없음 | 게스트 컴퓨트 호출 수(rt 위임) |

### 텍스처·렌더 타겟

| 프로젝트 | Vulkan 호출·연결 | 위치 | 실행 증거 | 미확인 |
|---|---|---|---|---|
| libapplegfx | vkCreateImage 4곳, vkCmdBeginRendering 6곳 | `render_target.c:130`; `iosurface.c:84`; `pipeline.c:490,1259`; `display.c:699,1068`; `draw_record.c:152` | 로그 "vulkan render" PASS(64x64 BGRA8 렌더 타겟 스모크) | attachment별 VkImageView(`render_inner_ops.c:332` TODO) 미구현 |
| pvg-vulkan | create_image 7곳, cmd_begin_render_pass 2곳, create_render_pass 1곳, create_framebuffer 4곳 | `pools/images_and_registry.rs:182,1016,1197,1407`; `exec.rs:3887,4050`; `caches.rs:1081` | 소스 + 통합시험. 로그 없음 | 실행 여부 |
| reims-vgpu | create_image 2곳(표시·비디오). render pass 0건 | `plane_copy.rs:631`; `surface.rs:892`(비디오) | 없음 | 게스트 텍스처 생성(rt 위임) |

### 표시 출력 (present/display)

| 프로젝트 | Vulkan 호출·연결 | 위치 | 실행 증거 | 미확인 |
|---|---|---|---|---|
| libapplegfx | vkCmdBlitImage 1, readback 복사 3, 커서 draw 1 | `vulkan/display_blit.c:219`; `display.c:1366`; `cursor.c:264` | render target 스모크 PASS. 화면 출력은 실기기 미검증 | 실제 화면 출력 |
| pvg-vulkan | queue_present 1, cmd_blit_image 1, cmd_clear_color_image 2 | `window_present.rs:1311,1787,1222,1247` | 소스. 로그 없음 | 실행 여부 |
| reims-vgpu | 스왑체인 + readback 1 | `plane_copy.rs:783`; `reims-vgpu-window` | README 수치(README.md:62 언급만). 저장소 내 실행 로그 없음 | README 수치 재현 |

### Henge 범위

- Apple Metal 백엔드는 있습니다. `devices/pvg_metal.m`(2642줄)은 mac/iOS 타깃에서만 빌드됩니다(Makefile:27-29,63-64,128). Metal 호출은 drawPrimitives 9, drawIndexedPrimitives 7, dispatch 5, copyFrom* 약 9, newTextureWithDescriptor 11입니다.
- Android GPU 실행 경로는 없습니다. `host/pvg_metal_stub.c:1-29`는 생성 시 NULL을 반환하고 명령을 버립니다. `android/app/src/main/cpp/CMakeLists.txt:16,34`는 log와 m만 링크하며 Vulkan/GL/EGL 참조가 없습니다. T3(실기기 Vulkan)는 미실행입니다.
- 앞서 "triangle lavapipe e2e 조건 미확인"이던 항목은 해소되었습니다. 로그상 `LAGFX_TRIANGLE_SPV_DIR`이 설정되지 않아 SKIP되었습니다.
- 근거 로그: `libapplegfx-vulkan/build/meson-logs/testlog.junit.xml`(75건, OK 72, SKIP 3, FAIL 0). 이 build 디렉터리는 git에 추적되지 않는 로컬 산출물입니다.

### Henge 제안

1. Android 첫 단계는 Vulkan 이식이 아니라 BGRA 오프스크린 결과를 기존 스냅샷 경로(`SnapshotViewActivity.java:25` 주석)에 연결하는 스파이크로 제한합니다.
2. 이식 범위는 draw 5개와 blit 2개로 제한하고, render 테이블 98행 이식은 보류합니다.
3. 실기기 Vulkan 검증(T3) 없이는 채택을 결정하지 않습니다.

(이전 섹션 2~7의 상세 내용은 아래에 그대로 유지합니다.)

## 2. 게스트 물리주소 → 호스트 포인터

| 프로젝트 | 실제 위치 | 판단 |
|---|---|---|
| Henge | `core/machine.c:531`(`hv_phys_ptr`), 내부 `hv_ram_lookup` (`machine.c:430`, 선형 탐색). 길이 검사(`pa+len-r->pa > r->size`) 후 호스트 포인터 반환. `devices/pvg_lite.c:598`(`hv_pvg_lite_phys`), `:167`(`ring_read`, 4KiB 단위 분할) | 길이만 검사하고 원시 포인터를 돌려줌 |
| libapplegfx | 호스트 포인터 변환 없음. GPA는 shell 콜백으로 접근 (`src/protocol/object_resolver.c:31,42,55` `read_memory`, `src/protocol/stamp.c:55–59` `write_memory`). `task.c:203`(`lagfx_task_map_host_memory`)은 `src` 안에 호출자가 없고 `tests/memory-task.c`만 호출 | 콜백 기반. Henge에서는 어댑터로 감쌀 수 있음 |
| pvg-vulkan | `runtime/guest_ram.rs:92`(`GuestRamRegion`), `:321`(`GuestRamImport::new`), `:460`(`host_base`), `:478`(`slice`), `:515`(`slice_for_gpa`), `:542`(`resolve`). `wire/src/mem.rs:47`(`GuestMemory::read_at`). `host_ptr_for`는 의도적으로 없음 | 경계를 타입으로 강제. 경계 검사를 거친 값만 만들 수 있음 |
| reims-vgpu | `reims-vgpu-vulkan/src/core/memory/map.rs:414`(`virtual_address_for_physical`: 첫 포함 범위 선택, 범위 걸침 거부), `:192`(`map_memory_at_virtual_address`). `reims-vgpu/src/memory.rs:46`(`push`, unsafe 호스트 포인터) | 범위 걸침을 거부 |

**핵심:** Henge의 `hv_phys_ptr`는 경계 검사를 호출부마다 하는 구조입니다. pvg-vulkan처럼 "경계 검사를 통과한 값만 생성 가능한 타입"으로 감싸는 것이 재구현 후보입니다.

## 3. 메모리 별칭·재매핑

| 프로젝트 | 실제 위치 | 판단 |
|---|---|---|
| Henge | RAM은 `vmapple.c:281`(`mmap(MAP_SHARED)`), `:297`(익명 폴백). `MAP_FIXED`는 `ios/Henge/Native/henge-ios-jit.c:423`(JIT 코드 페이지). Henge 본체에는 mremap 없음 | 재구현 불필요. Metal NoCopy가 필요해질 때만 검토 |
| libapplegfx | `task.c:111`(`lagfx_task_create`, PROT_NONE 예약), `:179–180`(copy 폴백 `mmap(MAP_FIXED|MAP_SHARED, memfd)`), `:251–260`(`munmap` 후 `mremap(old_size=0, MREMAP_MAYMOVE|MREMAP_FIXED)`로 복제 alias), `:277`(PROT_NONE 복구), `:316`(`lagfx_task_unmap`) | Linux 전용. src 안에 프로덕션 호출자가 없음 |
| pvg-vulkan | crates 전체에 mmap·mremap·memfd 호출 없음. 호스트 RAM은 import만 함 (`guest_ram.rs:460`) | 해당 없음 |
| reims-vgpu | `os/linux/vm.rs:514–521`(`anonymous`, MAP_FIXED 선택), `:544–553`(파일 매핑 `MAP_SHARED|fixed`), `:819`(`map_view`), `:1289`(`remap_runs_shared_overwrite`), `:1352`(`deallocate`). mremap 미사용 | 파일 기반 view 개념은 Henge에 과함 |

**정정:** libapplegfx의 `mremap` 복제 동작은 주석 근거일 뿐 실행으로 확인하지 않았습니다. 이 항목은 Android 런타임 시험(T5/T6)으로 확인합니다.

## 4. 리소스 객체 수명

| 프로젝트 | 실제 위치 | 판단 |
|---|---|---|
| Henge | `pvg_metal.m:2499–2502`(op 0x25/0x28, ref 제거), `:2509–2510`(op 0x20, task 제거), `:2573–2592`(`hv_pvg_metal_unmap`, surfaces·mapped_textures·planar 제거). `pvg_lite.c:682`(`tally_object`, 추적만) | 재구현. task 삭제 시 바인딩된 버퍼 무효화 검토 필요 (실행 검증 없음) |
| libapplegfx | `resource_registry.c:14`(`register`), `:104`(`unregister`), `:131`(`clear_task`). 호출처: `handlers/iosurface/iosurface.c:78,140,378`, `handlers/task/task.c:109`(CmdDeleteTask). `task.c:143`(`lagfx_task_destroy`)는 src 호출자 없음 | 레지스트리와 task 단위 일괄 정리 구조. 참고 |
| pvg-vulkan | `wire/src/ops/destroy.rs:75`(`Delete`), `:105`(`delete()`). `runtime/drain/mod.rs:271`(`apply_delete_object`), `:409`(`apply_delete_task`). `model/state.rs:2781`(`delete_object`, 호스트 복사 무효화), `:2688`(`delete_task`, bound buffer 회수 포함) | 삭제 시 캐시·바인딩 무효화 순서. 참고 |
| reims-vgpu | `os/linux/import.rs:1097`(`release`, deallocator와 keep-alive), `remap.rs:459`(`wait_unimported`), `:483`(`free_unimported`), `gpu/command.rs:940`(`Drop for Ending`), `gpu/device.rs:123`(`Drop for DeviceObject`) | 해제 대기 패턴. 참고. 객체 create/delete 표면은 외부 rt 크레이트에 있고 vendor에 없음 |

## 5. 완료 스탬프·펜스

**Henge 완료 스탬프: 무조건 기록은 확인, 역순·순환 위험은 미재현 (재현 시험 전에는 결함으로 판정하지 않음):**
- `devices/pvg_lite.c:612`(`hv_pvg_lite_complete`)은 `:621`에서 `__atomic_store_n(s, stamp, RELEASE)`로 **무조건** 기록합니다. 단조 가드가 없습니다.
- 배리어 비교는 `:905–909`에서 부호 있는 차이로 합니다. `:937–938`, `:924`도 무조건 기록입니다.
- 호출처: `pvg_metal.m:1843–1851`(MTLCommandBuffer completed handler), `:1857–1859`(`finishChannel`).
- `tests/test_pvg.c:280–288`은 역순 스탬프 22→25를 FINDING으로 출력하도록 작성되어 있습니다. 실행 결과는 확인하지 않았습니다.

| 프로젝트 | 실제 위치 | 판단 |
|---|---|---|
| Henge | 위 참조 | 수정 후보이나 확정 아님. 역순 완료 재현 시험 후 채택 여부를 결정 |
| libapplegfx | `stamp.c:28`(`lagfx_advance_stamp_cell`), `:55`(`next = max(target, cur+1)`, 최소 1), `:71`(`lagfx_protocol_complete_stamp_slot`), `:100`(`complete_stamp`). 호출처: `dispatchers/channel_0_dispatcher.c:494`, `channel_display_dispatcher.c:295`, `channel_compute_dispatcher.c:437`, `handlers/compute/exec_cmdbuf.c:580`. CLAUDE.md의 "0 금지, 단조" 규칙과 일치 | 단조 규칙은 참고 후보. 32비트 순환은 처리하지 않는 것으로 보이며(순환 비교 코드 미확인), 셀 읽기-쓰기는 원자적이지 않음 (단일 writer 가정) |
| pvg-vulkan | `runtime/drain/mod.rs:1600`(`write_stamp`): `:1565`(`stamp_word_ordered_on_gpu`), 쓰기 전 `settle_guest_writes`·`quiesce_guest_reads`로 게스트 쓰기를 먼저 확정. `backend/vulkan/engine/stamp_completion.rs:208`(`reserve`, timeline fetch_add AcqRel), `:279`(`run`), `:115`(`announce`) → `device/mod.rs:306`(`announce_stamp_interrupt`) | "게스트 쓰기 확정 후 스탬프 쓰기" 순서 원칙을 차용 |
| reims-vgpu | `reims-vgpu-graphics/src/ring.rs:576`(`post`): 데이터 기록 후 PRODUCER Release, `:612–613`(`COMPLETED_SEQNO`를 `fetch_max(Release)`로 단조 기록), `:478`(`rearm_or_raise`, SeqCst fence와 armed swap) | `fetch_max` 패턴이 Henge 수정에 가장 가까움 |

**정정 기록:** 초안은 이 항목을 "확인된 결함"으로 적었으나 과장된 표현이었습니다. 확인된 사실은 무조건 기록이라는 점뿐입니다. 역순 완료, 32비트 순환, 동시 완료 경쟁, 게스트 대기 해제 동작은 아직 재현하지 않았습니다.

**재현 시험 조건 (미실행):** (1) 같은 채널에서 완료 콜백을 역순으로 발생시켜 값이 후퇴하는지 확인, (2) 32비트 순환 시 비교가 어떻게 되는지 확인, (3) 여러 완료 경로의 동시 기록 경쟁, (4) 게스트 대기 해제가 기대대로 되는지 확인. 이 시험 후에만 단조 갱신 채택을 검토합니다.

## 6. 완료 인터럽트 벡터

| 프로젝트 | 실제 위치 | 판단 |
|---|---|---|
| Henge | `devices/vmapple.c:313`(`hv_pvg_lite_create(..., 32+0x11, 32+0x10)`), `pvg_lite.c:626`(`hv_gicv3_pend`), `:627`(ncpus==1이면 wake), `:981, 1012, 1015`(재펜딩) | 직접 재사용 (현행 유지) |
| libapplegfx | `stamp.c:91–92`(`raise_interrupt(opaque, 0u)`), `timer.c:114`. CLAUDE.md "MSI vector 0" 준수 | 벡터 0 규칙 참고. QEMU측 콜백 구현은 미확인 |
| pvg-vulkan | 벡터 번호 없음. `host.rs:239`(`irq_gfx()`) → `qemu/host_ops.rs:377`(`IrqGfxPulse`, 펄스 병합). `device/mod.rs:306–316`(`intr_gpu` fetch_or, 비트 index & 0x1f) | 펄스 병합 시 상태 비트 누적 원칙 참고 |
| reims-vgpu | `reims-vgpu-graphics/src/wire.rs:589`(`RGFX_CONTROL_VECTOR = 0`), `:53`(`RGFX_MSIX_VECTORS = 1`), `device.rs:3602`(`raise_interrupt(queue.vector)`, 큐별 벡터), `:2340, 2746`(control 벡터) | 큐별 벡터 분리. 벡터 할당 방식은 미확인 |

## 7. Henge와의 차이 요약

- 메모리: Henge는 경계 검사 후 원시 포인터를 돌려주고, pvg-vulkan은 경계를 타입으로 강제합니다.
- 스탬프: Henge는 무조건 기록하고, libapplegfx와 reims는 단조 규칙을 둡니다. 차이는 확인되었으나, Henge에 결함이 있는지는 재현 시험 전까지 미정입니다.
- 인터럽트: Henge의 벡터 방식은 현행 유지하고, 벡터 0 규칙은 libapplegfx와 같습니다.

## 8. 미확인 항목

- Android에서 `mremap` aliasing이 실제로 동작하는지 (런타임 시험 필요)
- 호스트 task 예약 해제의 실제 경로 (libapplegfx `task.c:143`의 호출처 없음)
- Henge 바인딩 무효화 누락 여부 (실행 검증 필요)
- `test_pvg.c` 실행 결과
- libapplegfx 셰이더 셀 읽기-쓰기의 원자성 (단일 writer 가정의 적절성)
- libapplegfx `CLAUDE.md`의 저장소 구조 설명이 실제 트리와 다름 (설명이 오래된 것으로 보임)
- 실기기 Vulkan 하드웨어 경로의 동기화 동작
