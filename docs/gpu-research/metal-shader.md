# Metal 셰이더 변환 (AIR → SPIR-V)

## 확인된 사실 (libapplegfx-vulkan, 커밋 7574eac0)

- 변환기 소스는 `src/air2spirv/`에 있습니다. 예: `shader_translate.c`.
- 로그인 화면 셰이더 시험(`air2spv-login-corpus-unit`)이 존재하며 PASS 했습니다 (T0, 메인 빌드 시험 53번).
- README에 따르면 변환된 vertex·fragment 셰이더로 Linux lavapipe 기준 약 99.7% 프레임 커버리지를 보고했습니다. 이는 CPU 드라이버 기준이며 실기기 결과가 아닙니다.

## 시험 상태 (T0)

| 시험 | 결과 | 원인 |
|---|---|---|
| shader translate (unit) | SKIP | llc 경로가 macOS Homebrew 경로(`/opt/homebrew/opt/llvm@20/bin/llc`)로 고정됨 (로그 951행) |
| pipeline build (unit) | SKIP | llc 미발견 (로그 962행) |
| air2spv 계열 단위 시험 | PASS 다수 | - |

Linux에 `/usr/bin/llc`가 있어도 LLVM 버전과 호출 인수가 맞는지 확인하기 전에는 시험이 통과한다고 판단하지 않습니다.

## 미확인 항목

- 실제 macOS 게스트 셰이더 전체에 대한 변환 성공률
- 지원하지 않는 AIR 명령 목록
- Android Vulkan 드라이버에서의 SPIR-V 검증 결과

## Henge와의 관계

Henge에는 현재 AIR 변환 경로가 없습니다. 따라서 이 영역은 libapplegfx 결과를 기준으로 "조건부 채택"입니다.
