# 원본 Henge 대비 기능 동등성 감사 (2026-11-25)

범위: 읽기 전용 분석. 소스·설치쌍·복제본 변경 없음. 6개 영역(A 엔진, B 생명주기, C 복원·부팅, D 하드웨어, E 그래픽·입력, F 빌드·시험·이력)을 병렬로 비교했다.
근거는 코드·커밋 읽기이며 기기 실행은 없다. 이 문서의 "원인" 항목은 모두 후보이고 입증되지 않았다.

## 1. 기준점과 Git 구조

- 포크(research/android-port)는 원본과 Git 이력을 공유하지 않는다(`merge-base` 없음). 루트 커밋 082a4eb 하나에 1253개 파일이 들어 있다. 포크 커밋 71개.
- `17e9ed9`는 공통 조상이 아니라 **파일 내용 일치도로 추정한 기준 커밋**이다(일치 893/1253, 인접 후보 892). 정확도는 커밋 1~수 개 이내.
- 원본은 기준 이후 165커밋, 153개 파일 변경. 포크는 기준 대비 신규 635, 수정 68개 파일.
- 병합 비용(`git merge-file` 충돌 수): machine.c 5, exception.c 3, vm.c 3, vm.h 3, cpu.h 2, metallib.c 2, pvg_lite.c 1, 그 외(xhci.c, dispatch.c, translate.c, jit_region.c, test_mmu.c) 0.
- 권장: 기존 브랜치 이력은 보존(강제 푸시·재작성 금지). 원본 이력 기반 신규 통합 브랜치에 Android 계층을 이식하는 방안을 비교 대상으로 하고, 단기에는 현재 포크 유지 + 개별 패치 반영. 실행은 별도 승인 후.

## 2. 현재 HALT(iBootStage1, reason 7)와의 관계

- 원본의 후속 수정 중 HALT를 설명하는 항목은 발견되지 않았다. 직접 원인은 **미확인**.
- reason 7은 HV_EXIT_HALT이며 PSCI 정지와 host stop/stop_pc를 구분하지 못한다. 복제본 실행기에는 시간제한 정지가 없고, 모드 A의 PSCI 기록(`psci SYSTEM_OFF/RESET`, pc, lr, stack)이 구분의 열쇠다.
- 복원 미완료(seal_system_volume 이후 기록 없음)와 `auto-boot=false` 잔존은 같은 원인을 공유할 가능성이 있다(미검증). 원본도 호스트에서 auto-boot를 되돌리지 않으며 RestoreOS의 restored가 한다.
- 후보(미입증): CNTVOFF 초기화 차이, Android의 `HV_WRITE_CACHE` 무효(F_NOCACHE 없음), 복원 RAM 2048MiB(원본 2048~4096), 복원 로그의 outcome(TIMEOUT/CANCELLED/FAILED/NO_SIGNAL), reverse-proxy 해제 불확실 시 -1 반환(restore.c:6176).

## 3. 분류표 (9분류)

| 분류 | 항목 | 근거/비고 |
|---|---|---|
| 원본 후속 개선 미반영 | CNTVOFF (8bde813): 포크는 게스트 카운터가 호스트 카운터 그대로. machine.c:616, vmapple.c:351 차이 | 목적은 macOS uptime/idle 문제. iBoot 영향 증거 없음. cpu.h 레이아웃·translate.c 오프셋과 함께 이식해야 함 |
| 원본 후속 개선 미반영 | 종료/재시작 생명주기 (6aefa03): 보조 CPU·kicker join, virtio_blk stop, hv_vm_destroy, net/dns/audio/pvg 해제, 스레드 생성 순서(CPU0 마지막) | 포크 cpu0_thread는 run 직후 flush. 프로세스 종료는 메모리·fd 회수만 대체 |
| 원본 후속 개선 미반영 | hv_vm_stop이 halted=3 덮어쓰기, pvpanic halted=4 직접 기록 → hv_machine_halt 사용 | 포크에 hv_machine_halt 이미 있음. 호출자 전수 확인 필요 |
| 원본 후속 개선 미반영 | SCTLR_EL1 ITD/SED/CP15BEN (f512e8d) | 2줄, 위험 낮음, ROM 검사 증거 없음 |
| 원본 후속 개선 미반영 | xHCI slot-ID 검증 (fc58667) 외 xHCI 확장(Magic Trackpad 2 등) | xhci.c는 기준과 동일, 충돌 0. 기능은 P3 |
| 원본 후속 개선 미반영 | RestoreRamDisk 스트리밍 (c20420a) | 피크 약 360MiB→100KiB 미만. seal 정지의 해결책 아님. UNSUPPORTED 대체 경로 추가·img4 전후 해시 비교 필요 |
| 원본 후속 개선 미반영 | virtio_blk 스레드 실패 검사, hv_disk_open fd=-1, vmapple create NULL 검사 | 저위험 |
| 원본 후속 개선 미반영 | pvg_lite 구형 macOS 지원, debug, vblank | 기능 단위 병합. 충돌 1+헤더 1, test_pvg 138/138 |
| 불필요한 동작 변경(후보) | 포크 PC 샘플링이 디스패치 핫패스에 있음 | 옵션화 또는 제거 검토 |
| Android 호환성 변경 | memfd JIT(RW/RX), clear_cache, futex ulock, HV_SWITCH_FLUSH 기본 off | 유지 |
| 플랫폼별 동등 구현 | 프로세스당 러너(메모리·fd·스레드·소켓 회수), PSCI wake/CPU_ON denial, resumable IPSW 다운로드, 중복 설치 방지 | 메모리 회수에 한정해 동등 |
| Android 진단 기능 | psci_halt_* 기록, panic probe | 공통 진단 인터페이스+별도 모듈 분리 검토 |
| 공통 VM 관리 API 개선 후보 | hv_vm_flush_disks_checked, hv_vm_start_failed | 시작 실패·저장 성공 여부를 호출자에 전달하는 정상 관리 기능. 진단 코드가 아님 |
| 포크 독자 개선(공통 개선 후보) | HvPvgBackend 테이블, 링·길이 검증 강화, USB 하드닝, 읽기 전용 저장소 | 원본 기여 후보 |
| 실험적 변경 | clone 러너 USB 모드 0~4, 모드 E(autoboot_fix) | 모드 E를 거친 clone-001은 AUX가 바뀐 시험본. 비교 시 분리 |
| iOS 전용 | tl/(Mac 앱 변환 62커밋), Swift UI·앱 상태 관리, Metal, iOS JIT | Android 불필요 |
| 공통 기능 + 플랫폼별 구현 | 스냅숏(CPU·RAM·장치 상태 저장·복원), VM 즉시 재개 | 현재 부팅과 무관해 후순위. Android에 영구 불필요로 분류하지 않음. snapshot.c는 Apple compression.h·GCD 의존이라 Android용 대체 구현 필요 |

D 정정: 원본 `recovery.c`(113·119행)에 `setenv auto-boot`/`saveenv`가 있고 포크에도 있다. 일반 부팅 중 getenv(모드 D)·auto-boot 수정(모드 E)은 포크 시험 기능이다.

## 4. 별도 결함(부팅 무관)

- android ui_jni.c가 `hv_touch_set_display`를 1280x720 고정 1회 호출. 게스트 기본 1920x1080이라 화면 가장자리 접근 불가 가능(P2, 코드 읽기 기준).
- CI: build-henge.yml이 push마다 IPA 빌드(원본은 fa6182a에서 수정). test_devices는 continue-on-error. `make test`가 CI에 없음.
- b66bb59가 빌드 산출물 252개(약 5.5MB, 시험용 키·인증서 포함)를 추적 중. `.gitignore` + `git rm --cached` 제안(승인 필요).

## 5. 수정 순서 (제안, 미적용)

1. 원본 시험 이식: test_shutdown.c(코어 순수 C, hv_machine_free·HV_HALT_* 필요), test_mmu.c 블록. test_dt_fixup.c는 dt_fixup 기능을 이식하지 않는 동안 제외. 포크에는 hv_machine_halt의 엔진 시험이 없다.
2. 종료 사유 보존: hv_vm_stop·pvpanic → hv_machine_halt. 모든 호출자(복원/installed/clone/g4 러너) 영향 분석 후. 설치 러너의 USB·복원 작업자 생명주기에 같은 절차를 무조건 적용하지 않는다.
3. 종료 순서: 스레드 핸들 보존, CPU0 마지막 생성, 보조 CPU·kicker join, virtio_blk stop, 그 뒤 flush(`hv_vm_flush_disks_checked`), 콜백.
4. VERDICT에 구조화 필드(outcome, psci, uptime_s, write_errors 등) 추가. 기존 정규식과 호환.
5. 재시작: 명시적 SYSTEM_RESET, 이전 프로세스 종료 확인, 디스크 완료·flush 성공, 같은 ECID·설치쌍 유지. 최소 가동 60초·화면 확인은 요구하지 않는다(원본 동등성). 반복적 초기 리셋에만 횟수 제한(설정 가능, 예: 10분 3회)과 대기(2/8/30초)를 적용. 오류·host stop·종료 미확인·flush 실패에서는 재시작하지 않는다.
6. 저위험 이식(virtio_blk, disk.c, vmapple), SCTLR, CNTVOFF(cpu.h·translate.c 오프셋 동반, host/enter.S 재검증).
7. 복원 코드 이식(스트리밍, F_NOCACHE/HV_WRITE_CACHE, 타임아웃·RAM 설정)은 새 설치쌍이 필요한 시험이므로 별도 승인 후.
8. 빌드: 15개 스크립트의 vm_*.o 목록과 stubs_portable.c(hv_net_detach, hv_dns_proxy_*, hv_audio_detach 스텁) 갱신.

## 6. 검증 기준

- 엔진: test_shutdown(첫 사유 유지: RESET 후 host stop에서도 halted==2), test_mmu 블록, test_pvg 138/138 유지.
- 생명주기: 종료 후 보조 CPU·kicker join 확인, blk idle 확인 뒤 flush, 콜백 순서 단위시험(Linux에서 가능한 범위).
- VERDICT: 해석 표기를 "VM 종료 확인 및 flush 성공"으로 하되 전체 CPU 종료·비동기 쓰기 완료는 별도 필드로만 주장한다.
- 보존: 설치쌍, clone-001~003, 보관 폴더는 변경하지 않는다.

## 7. 한계

- x86_64 샌드박스라 arm64 엔진 시험·NDK 빌드를 실행하지 않았다. 4개 동등 판정은 소스 읽기 기준이다. tl/ 62커밋은 제목·경로로 일괄 분류했고 나머지 103커밋은 개별 확인했다. 포크는 얕은 이력이라 장치별 포크 커밋은 코드 주석 날짜로 대신했다.

## 8. 진행 상태 (2026-11-25, 승인 후 구현)

반영됨(커밋 5784315, 68fd9b4, 931f7a2, 124835f 외): 종료 사유 보존(hv_vm_stop, pvpanic → hv_machine_halt), 스레드 핸들 보존과 CPU0 마지막 생성, 보조 CPU·kicker join(5초 상한), virtio-blk stop/start/idle/destroy, 그 뒤 flush와 종료 콜백,
HV_HALT 이름, test_shutdown(CI의 native arm64에서 실행), VERDICT 구조화(outcome/psci/quiesced/uptime_s/disk_writes/disk_write_refused), G7RunVerdict·G7RestartPolicy와 단위시험, 재시작 연결(같은 복제본, E→D, 반복 제한 3회/10분, 2/8/30초),
xHCI 슬롯 검증(fc58667), disk.c fd=-1, vmapple 디스크 생성 실패 검사, 빌드 산출물 추적 해제.

미반영: SCTLR(f512e8d)·CNTVOFF(8bde813)는 "시험 후 반영" — 이 환경에는 arm64 실행기가 없어 시험하지 못했고 JIT 오프셋과 묶여 있다. RestoreRamDisk 스트리밍, F_NOCACHE, 복원 러너 설정은 별도 작업.
실기기 미검증: 재시작 동작, 종료 순서 변경이 복원 러너/installed 러너에 미치는 영향(복원 러너는 이번에 같은 hv_vm_start 경로를 쓴다).

## 9. 갱신 (2026-11-25 오후): 실기기 결과와 이후 반영

**실기기(APK 0.2+1a79de8, clone-001, 모드 A, 설치쌍·복제본 변경 없음):** 900초 시험 시간 제한까지 실행. iBootStage1 HALT 없음, CPU 샘플이 EL1(커널)과 EL0(사용자 공간)을 오감, 디스크 쓰기 요청 986건·거부 0건, 시간 제한에 의한 호스트 정지 후 `quiesced=1`, `disk_flush=ok`, 종료 코드 0, `outcome=host-stop psci=none`. 이로써 새 종료 구조(CPU·kicker·디스크 작업자 정리와 flush 순서)가 실기기에서 동작함이 확인됐다.

**아직 미검증:** 게스트 SYSTEM_RESET/OFF 종료 경로와 자동 재시작(이번 실행에서는 게스트 종료가 없었다). SCTLR·CNTVOFF 이식이 기존 HALT를 없앴는지의 인과(이식 전후 비교 실행 없음; 이전 소스로 되돌려 확인하지 않는다). macOS가 로그인 화면까지 갔는지(화면을 가져오지 못해 판단 불가). 이전 보고의 "복제본 실행기에 시간 제한이 없다"는 틀렸다: `--seconds` 시험 시간 제한(앱 기본 900초)이 있고 도달하면 `stop_kind=time-limit`로 끝난다.

**화면:** 게스트가 만든 화면 면은 fourcc `RGhA`(RGBA16Float, 픽셀당 8바이트)였고 `pvg_lite.c`는 `BGRA`만 허용해 PVG 프레임이 0장이었다(원본 `pvg_metal.m`은 이미 `RGhA`를 처리). 반영: 공통 모듈 `devices/pvg_half.h`(원본의 half→byte 표와 표시 인코딩 정책; `pvg_metal.m`도 같은 헤더 사용), `pvg_lite.c`의 live·last_surface·tally 경로가 `RGhA`를 `B,G,R,255`로 변환(픽셀 크기별 stride 검사, 표면이 페이지 목록 안에 들어가는지 검사), 거부 로그는 처음 1회 후 4096회마다. 시험: `tests/test_pvg.c`에 합성 데이터 13건(값·stride 패딩·페이지/오프셋 경계·로그 빈도·BGRA 회귀), x86 plain/ASan+UBSan 통과, half→float 변환은 65536개 값을 하드웨어 변환과 전수 비교해 일치. 한계: 실제 macOS 화면의 내용은 확인 전. `HV_PVG_HALF=linear`(모드 1)는 libm을 링크하는 빌드에서만 켜지고(`-DHV_HALF_LIBM`) Android 러너는 HV_* 환경변수를 제거하므로 모드 0 고정.

**시간 제한:** 복제본 부팅 대화상자에서 15분/30분/1시간/제한 없음 선택(`--seconds 900|1800|3600|0`, 그 외 값은 거부). 중지·quiesce·flush 경로와 재시작 제한(10분에 3회)은 그대로이며, 자동 재시작은 같은 제한 시간을 따른다.
