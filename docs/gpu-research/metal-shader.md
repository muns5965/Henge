# Metal 셰이더 변환 (AIR → SPIR-V)

함수 단위 비교 기록입니다 (읽기 전용 분석, 기준 커밋: libapplegfx 7574eac0, metal2vulkan d9236a8a, indium 1bf59a85). 경로는 저장소 루트 기준입니다.

## 1. 경로 정정 (이전 문서 대비)

- libapplegfx의 SPIR-V 생성기는 **`src/air2spv/`** 에 있습니다. 이전 문서가 가리킨 `src/air2spirv/`는 llc를 호출하는 별도 경로입니다.
- `src/air2spv/`는 llc 없이 동작하며, 단위 시험 대부분이 이 경로를 씁니다.
- Indium에는 AIR 번역이 있습니다. `src/iridium/air.cpp`와 `spirv.cpp`가 libLLVM을 `dynamic-llvm.cpp`(17–21행)에서 동적 로드해 비트코드를 읽고 SPIR-V를 직접 만듭니다. 따라서 "Indium은 AIR 변환 대상이 아니다"는 이전 가정은 틀렸습니다.

## 2. 기능별 비교

### 2.1 메탈립/MTLB 컨테이너 파싱

| 프로젝트 | 실제 위치 | 검증 수준 | 지원 범위 | Henge 적용 제안 |
|---|---|---|---|---|
| libapplegfx | `src/air2spirv/metallib_extract.c:102` (has_magic), `:111` (`lagfx_metallib_extract_functions`) | `tests/air2spirv.c`의 실바이트 게이트, fixture `default.metallib`(MTLB 매직 확인) | 함수별 비트코드 추출 | 참고 (C, MIT) |
| metal2vulkan | `crates/metal2vulkan-air/src/container/mtlb.rs:242`(parse), `:550`(function); `container/mod.rs:184`, `:212` | 단위 테스트 다수, 실 macOS 코퍼스는 저장소에 없음 (`validation/README` 21행) | 메타데이터·공유 섹션, 링크 정보(`:449`) | 어댑터 (Rust, C ABI 감싸기 필요) |
| Indium | `src/iridium/air.cpp:4423` (Library::Library), `:4431–4432` (MTLB 검사), `:129–132` (비트코드 매직) | 런타임 테스트 디렉터리 기반 | MTLB + 래퍼 | 참고 |

### 2.2 LLVM 비트코드 읽기

| 프로젝트 | 실제 위치 | 검증 수준 | 지원 범위 | Henge 적용 제안 |
|---|---|---|---|---|
| libapplegfx | `src/air/bitcode_reader.c:435`(`lagfx_air_module_open`), `:401`(parse_wrapper), `:1862`(function_body_open); `src/air/bitstream.c:25` | `tests/air-bitcode-reader-unit.c` 등 | 자체 파서, libLLVM 미사용 | 재사용 후보 (자체완결 C) |
| metal2vulkan | `bitstream/mod.rs:383`(read_record), `bitcode/mod.rs:115`(decode_module), `bitcode/function.rs:142`(decode) | 테스트 567개 (metal2vulkan-air) | 함수 블록·메타데이터 디코드, 미지원 레코드는 명시 오류 | 어댑터 또는 참고 |
| Indium | `air.cpp:2315`(Function::analyze); `dynamic-llvm.cpp:17`(init), `:56`(resolveSymbol) | 런타임 테스트 | host libLLVM에 의존 | 참고 (libLLVM 설치 전제) |

### 2.3 함수 시그니처와 버퍼·텍스처·샘플러 바인딩

| 프로젝트 | 실제 위치 | 검증 수준 | 지원 범위 | Henge 적용 제안 |
|---|---|---|---|---|
| libapplegfx | `src/air2spv/translate.c:33`(discover_stage: air.vertex/air.fragment만), `:103`(lagfx_air_arg_bindings) | `tests/air2spv-arg-bindings-unit.c` | 버퍼·텍스처·샘플러 종류와 Metal index 추출. kernel은 UNKNOWN (`:37`) | 재사용 가능하나 아래 바인딩 한계 반영 필요 |
| libapplegfx (바인딩 번호) | `src/air2spv/translate_function.c:1794–1803` (버퍼: DescriptorSet 0 + 순차 Binding), `:1916–1929` (텍스처·샘플러 순차) | 일부 단위 테스트 | **Metal index 대신 순차 번호 사용**. 코드 주석도 metadata 기반 번호를 개선 과제로 적음 | 재구현 필요 (Metal index를 Binding에 반영) |
| metal2vulkan | `abi/args.rs:571`(role), `:597`(Buffer), `:705`(Texture), `:723`(Sampler), `:201`(location) | `abi/tests.rs` 등 테스트 다수 | Buffer·ArgumentBuffer·Texture·Sampler·CommandBuffer 역할 분류 | 참고 (가장 완성도 높은 ABI 분류) |
| Indium | `air.cpp:2260–2267`(kind 판별), `:2286`(air.location_index), `:2788`(Buffer binding), `:3027`(Texture binding), `:2722`(uboPointers binding 0) | 런타임 테스트 (interface-locations, entry-metadata) | location 기반 바인딩 | 참고 (location 기반이 libapplegfx보다 나음) |

### 2.4 SPIR-V 생성 (vertex / fragment / compute)

| 프로젝트 | 실제 위치 | 검증 수준 | 지원 범위 | Henge 적용 제안 |
|---|---|---|---|---|
| libapplegfx | `src/air2spv/translate_function.c:7735`(translate_function), `:6614`(명령 디스패치) | 41개 air2spv-*.c 단위 테스트 | vertex, fragment. stage enum(`translate_function.h:49–50`)에 **compute 없음** | 재구현 또는 참고 |
| libapplegfx (llc 경로) | `src/air2spirv/shader_translate.c:142`(run), `:102`(run_llc) | `tests/shader_translate_test.c` | llc 외부 의존 | 재사용 금지 (경로 하드코딩) |
| metal2vulkan | `pipeline/compile/api.rs:179`(compile_function), `pipeline/compile.rs:579`(compile_stage), `spirv/serialize/mod.rs:114`(serialize) | 테스트 3,593개, spirv-val 사용 | vertex, fragment, kernel(GLCompute), mesh/geometry 링크 (`link/mesh.rs:138`) | 어댑터 또는 참고 |
| Indium | `src/iridium/iridium.cpp:20`(translate), `air.cpp:2392`(Vertex), `:2439`(Fragment), `:2465`(GLCompute) | 런타임 테스트 (basic-compute, lighting 등) | vertex, fragment, compute 모두 | 참고 (C++, 전체 번역기로 가장 가까움) |

## 3. 실패 처리 (안전성 관점)

| 프로젝트 | 알 수 없는 opcode 처리 | 위험 |
|---|---|---|
| libapplegfx | `src/air2spv/translate_function.c:6672–6682`의 `default:`에서 **로그만 남기고 명령을 생략** | 깨진 SPIR-V가 성공으로 반환될 수 있음 |
| libapplegfx (폴백) | `src/air2spv/translate.c:198–210`에서 번역 실패 시 하드코딩된 stub SPIR-V로 폴백 (`emit_position.c:51`, `emit_render_target.c:22`) | 실패가 조용히 가려짐 |
| metal2vulkan | `bitcode/function.rs:1236–1242`(Unsupported, UnknownRecord 명시 오류) | 명시 오류 |
| Indium | `air.cpp:4409–4410`에서 `runtime_error` 발생, `iridium.cpp:28`에서 catch 후 nullptr 반환 | 명시 실패 |

**판단:** libapplegfx의 생략·폴백 경로는 그대로 쓰면 안 되며, 실패를 오류로 바꾸는 재구현이 필요합니다.

## 4. 시험 상태

| 항목 | 결과 |
|---|---|
| libapplegfx `shader_translate` (llc 경로) | SKIP. `tests/shader_translate_test.c:19–20`에 `/opt/homebrew/opt/llvm@20/bin/llc` 하드코딩, `:300–304`에서 미존재 시 `_exit(77)` (확인됨) |
| libapplegfx `air2spv-*` 단위 시험 | PASS 다수 |
| metal2vulkan | 공개 fixture `validation/fixtures/public`에 `.metal` 746개, `.air` 8개. 실 macOS 코퍼스는 체크아웃에 없음 |
| Indium | `test/<name>/` 약 80개 디렉터리, Vulkan 1.3 GPU 필요 |

Linux에 `/usr/bin/llc`가 있어도 LLVM 버전과 호출 인수가 맞는지 확인하기 전에는 시험 통과로 판단하지 않습니다.

## 5. 미확인 항목

- libapplegfx `shader_translate.h:28–29` 주석은 "NOT_IMPLEMENTED"로 적혀 있는데 코드는 llc를 호출합니다. 현행 경로가 어느 쪽인지 확인이 필요합니다.
- libapplegfx에서 생략된 opcode가 실제로 어떤 단계에서 걸리는지
- 실제 macOS 게스트 셰이더 전체의 변환 성공률
- Android Vulkan 드라이버에서의 SPIR-V 검증 결과
- Indium 라이선스 외 세부 (0BSD는 LICENSE 본문으로 확인, 메타데이터 미확인)

## 6. Henge 관점 요약

1. 재사용 가치가 가장 큰 것은 metal2vulkan의 파서와 ABI 분류입니다. 다만 Rust라서 C ABI 어댑터가 필요하며, Henge의 빌드 체계와의 호환은 미확인입니다.
2. libapplegfx `src/air2spv/`는 C이고 자체완결적이지만, 순차 바인딩, opcode 생략, compute 부재, stub 폴백이라는 네 가지 결함이 있어 재구현이 현실적입니다.
3. Indium은 번역 구조와 오류 처리가 가장 명확하나 libLLVM 런타임 의존이 있습니다.
4. libapplegfx `src/air2spirv/`의 llc 경로는 하드코딩 때문에 재사용하지 않습니다.

## 7. 정정 기록

- 이전 판단: "libapplegfx 변환기는 `src/air2spirv/`에 있다" → 정정: SPIR-V 생성기는 `src/air2spv/`, `air2spirv/`는 llc 경로.
- 이전 판단: "Indium은 AIR 변환 대상이 아니다" → 정정: Indium은 libLLVM을 동적으로 불러 AIR를 번역한다.
- 이전 판단: "libapplegfx 셰이더 변환 조건부 채택" → 정정: 바인딩 번호, opcode 생략, stub 폴백을 재구현하는 조건을 명시한 "조건부 채택"으로 유지하되, 현 상태 그대로의 채택은 아님을 명확히 한다.
