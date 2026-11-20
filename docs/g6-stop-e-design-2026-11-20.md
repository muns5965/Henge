# G6 "정지점 E" 상세 설계 (2026-11-20)

G5(커밋 `0ae5436`)가 `restore_device()`의 `restored` 핸드셰이크·HardwareInfo/SavedDebugInfo 조회·BBTicket 복사까지 확인하고 멈춘 지점(stop D) 다음을 읽기 전용으로 확장한다. 이 문서는 네 차례의 검토 교환(①~④, 2026-11-20)에서 소스로 확인·합의된 내용만 정리한 것이며, 새 아키텍처는 없다. **이 문서 작성 시점까지 소스는 수정하지 않았다.**

## 1. 실제 컴파일 분기 확인

`third_party/restore_config/config.h:12`가 `#define HAVE_REVERSE_PROXY 1`을 정의하고, `tools/vmrun-install-g4-build.sh`/`vmrun-install-g5-build.sh`/`vmrun-install-g6-build.sh`(신규)/`tools/restore-selftest-build.sh`가 모두 `-DHAVE_CONFIG_H -I$TP/restore_config`를 쓴다. 따라서 이 프로젝트의 모든 빌드에서 `restore.c:5556`의 `#ifdef HAVE_REVERSE_PROXY` 분기(리버스 프록시)가 컴파일되고, `#else` 쪽의 FDR 스레드 코드(`fdr_connect`/`thread_new(fdr_listener_thread)`)는 컴파일 대상이 아니다. G6은 리버스 프록시 경로만 다룬다.

## 2. 정지점 E의 위치

`restore.c:5774`(`plist_dict_set_item(opts, "SystemPartitionPadding", spp);`, 복원 옵션 plist 구성의 마지막 줄) 바로 다음, `:5776~5777`의 `// start the restore process` / `restored_start_restore(restore, opts, client->restore->protocol_version)` 호출 바로 전. (라인 번호는 stop D 삽입 이후 현재 파일 기준.)

## 3. 새 플래그/반환 코드/구조체 (hv_stop.h에 추가될 내용, 아직 미작성)

기존 stop A/B/C/D가 `(1 << 20)`~`(1 << 23)`, RC 217~220을 쓰므로 다음 번호를 쓴다.

```c
#ifdef HV_STOP_AFTER_RESTORE_OPTIONS
#define FLAG_STOP_AFTER_RESTORE_OPTIONS (1 << 24)
#define HV_STOP_AFTER_RESTORE_OPTIONS_RC 221          /* 정상 정지: 정리 완료, restored_start_restore() 호출 안 함 */
#define HV_STOP_E_CLEANUP_UNCERTAIN_RC 222            /* 리버스 프록시 정리 결과가 불확실 — G6 실패로만 처리, 재시도/재실행 금지 */
enum { HV_RP_FAILED = 0, HV_RP_OK = 1 };              /* rproxy_status: start_proxy()가 실제로 성공했는지 (rp_started) */
struct hv_stop_e_info {
    const char *hardware_model;
    const char *product_type;
    int mode;                         /* 4 = RESTORE 기대 (stop D와 동일 검증) */
    uint64_t restored_protocol_version;
    int rproxy_status;                /* HV_RP_OK / HV_RP_FAILED: rp_started 기준 */
    int opts_required_fields_ok;      /* §5의 필수 키·자료형 검사 결과 */
    int tss_enabled;
};
extern void (*hv_stop_e_hook)(const struct hv_stop_e_info *info);
```

## 4. 종료 경로

**정상 경로** (리버스 프록시 정리가 안전하게 끝난 경우 — 대부분의 실행에서 기대되는 경로):
1. `plist_free(opts)`
2. `rproxy != NULL`이면 `reverse_proxy_client_free(rproxy)` 호출, 반환값 확인
3. 반환값이 `REVERSE_PROXY_E_UNKNOWN_ERROR`가 **아니면** → 안전하게 해제된 것으로 간주(§6 근거)
4. `hv_stop_e_hook` 호출 → `restore_client_free(client)` → `HV_STOP_AFTER_RESTORE_OPTIONS_RC`(221) 반환

**정리 불확실 경로** (`reverse_proxy_client_free()`가 정확히 `REVERSE_PROXY_E_UNKNOWN_ERROR`를 반환한 경우):
1. `plist_free(opts)`는 그대로 수행(리버스 프록시와 무관, 항상 안전)
2. `client->restore`/`client->restore->device`는 **해제하지 않음** (join 실패로 스레드가 아직 device를 참조 중일 가능성 — use-after-free 방지)
3. `hv_stop_e_hook`에 `rproxy_status`를 알 수 없음으로 보고
4. `HV_STOP_E_CLEANUP_UNCERTAIN_RC`(222) 반환 — G6 진단 빌드는 이 값을 **무조건 FAILED로 처리**, 통과로 세지 않음

## 5. 리버스 프록시 시작 성공 판정: `rp_started`

`restore.c`의 재연결 재시도 루프(`for (rp_attempt = 0; rp_attempt < 2; ...)`)에서 `rproxy != NULL`만으로는 성공을 판정할 수 없다(구형 프로토콜 재시도가 실패해도 포인터가 NULL로 재설정되지 않는 경로가 실제로 존재함, 소스 확인됨). 그래서 `reverse_proxy_client_start_proxy(rproxy, N) == REVERSE_PROXY_E_SUCCESS`인 두 호출 지점에만 새 로컬 변수 `int rp_started = 0;`을 `rp_started = 1;`로 설정하고, G6 보고는 `rproxy != NULL`이 아니라 `rp_started`를 기준으로 한다.

**첫 번째 정리 호출(`start_proxy(rproxy,2)` 실패 후의 기존 `reverse_proxy_client_free(rproxy); rproxy=NULL;`)에는 추가 보호가 필요 없다** — `reverse_proxy_client_start_proxy()`를 끝까지 추적한 결과, 이 함수가 실패를 반환하는 모든 경로(프로토콜 협상 실패/작업 등록 실패/`thread_new()` 실패)에서 `client->th_ctrl`은 반드시 `THREAD_T_NULL`이다. 스레드가 실제로 살아있으려면 `thread_new()`가 성공해야 하고, 그 경우 함수는 반드시 SUCCESS를 반환한다. 따라서 "실패를 반환했는데 스레드가 살아있는" 경로는 현재 소스에 없다(향후 `start_proxy()`의 반환 규약이 바뀌면 재검토 필요). UNKNOWN_ERROR의 모호성(§6)이 실제로 문제가 되는 곳은 정지점 E의 마지막 정리 호출, 그 중에서도 `rp_started==1`인 객체 하나뿐이다.

## 6. UNKNOWN_ERROR 반환값의 의미와 한계

`reverse_proxy_error()`(`reverse_proxy.c:76`)는 `service_client_free()`의 미매핑 오류를 `REVERSE_PROXY_E_UNKNOWN_ERROR`로 떨어뜨리는데, `reverse_proxy_client_free()`의 join-실패 조기 반환도 같은 상수를 쓴다. 그래서 **`REVERSE_PROXY_E_UNKNOWN_ERROR`만이 모호하다**(join 실패/스레드 생존 가능성 vs. join 성공 후 해제 완료+부차적 서비스 오류, 이 둘을 구분할 수 없음) — 그 외 5개 코드(SUCCESS 포함)는 반드시 "join 성공, 객체 이미 해제됨"을 뜻한다. G6은 이 모호한 경우를 전부 §4의 "정리 불확실 경로"로 보수적으로 처리한다.

## 7. 상위 세션 보호 — 신규 구현 아님, 기존 I3~I6 레지스트리 재사용

리버스 프록시 제어 스레드는 `hv_life_task_begin(HV_LIFE_K_PROXY_CTRL, &client->ctrl_gen)`으로 등록하고 스레드의 마지막 동작으로 `hv_life_task_end(...)`를 호출한다(`reverse_proxy.c:663`). `host/restore_registry.c`의 `hv_reg_wait_and_close()`는 등록된 작업 카운터가 모두 0이 될 때까지 CLOSING→CLOSED 전이를 보류하고, 기한 내 끝나지 않으면 `HV_REG_QUIESCENCE_FAILED`(이유 `HV_REG_QF_TASKS_NOT_QUIESCENT`)로 전이해 `hv_reg_release_permitted()`가 해제를 금지하고 최종 보고가 `HV_REG_R_SESSION_NOT_QUIESCENT`로 나간다.

**단, 등록 카운터가 0이라는 사실 자체가 `pthread_join` 성공을 의미하지는 않는다** — 레지스트리는 "작업이 등록된 상태"를 추적하고, `hv_life_task_end()` 호출 여부로 "그 작업이 끝났다고 선언했는지"만 안다. 스레드가 등록 해제(`task_end`) 이후 실제 OS 레벨 join이 어떻게 됐는지는 별개의 사실이다. 그래서 이 레지스트리는 "등록된 작업이 아직 안 끝났다"는 경우를 막아주지만, §4/§6의 "정리는 불확실하다고 보고됐지만 등록 카운터는 이미 0"인 경우까지 막아주지는 않는다 — 이 경우에 대한 보호는 정지점 E 자신의 보수적 비해제 분기(§4)가 맡는다. 문서 표현: *"리버스 프록시의 등록된 활성 작업은 기존 I3~I6 레지스트리가 관리하며, 종료되지 않은 작업이 있으면 세션 정리와 자원 해제를 차단한다. 별도로 리버스 프록시 객체 해제 결과가 불확실한 경우에는 정지점 E를 실패로 판정하고 관련 객체 해제를 보류한다."*

## 8. 복원 옵션(`opts`) plist 검증

`client->macos_variant`는 Henge의 모든 실제 대상에서 항상 true(설치 대상이 macOS뿐이므로)이지만, 다른 변형 경로가 생기면 재검토한다는 전제로 아래를 "현재는 필수"로 검사한다.

| 필드 | 검사 |
|---|---|
| `AutoBootDelay`, `SupportedDataTypes`, `SupportedMessageTypes` | 존재 |
| `SupportedAsyncDataTypes`, `RootToInstall` | 존재 |
| `CreateFilesystemPartitions`, `SystemImage` | 존재 + `PLIST_BOOLEAN` |
| `SystemPartitionPadding` | 존재 |
| `UUID` | 존재 + `PLIST_STRING` + 비어있지 않음 |
| `FormatForAPFS`, `MacOSVariantPresent`, `ShouldRestoreSystemImage` | macOS 분기(현재 항상 true)일 때 필수, 존재 + `PLIST_BOOLEAN` |
| `InstallRecoveryOS` | macOS 분기일 때 필수 |

## 9. 안전 정책 (변경 없음)

- `restored_start_restore()` 호출 0건 (정지점 E가 그 호출 전에 반환하는 것 자체가 구조적 증거, G5의 `restore_start_calls`/`fdr_init_calls`와 동일한 논리)
- ASR 이미지 전송 0건, 디스크/aux 읽기 전용(내용 해시 불변)
- 기존 TSS/네트워크 허용 목록 확대 없음 — 리버스 프록시 제어 스레드가 혹시 추가 연결을 시도하면 목적지·주체를 기록한 뒤 실패 처리하는 기존 방식 유지
- `check-no-test-flags.sh`에 `FLAG_STOP_AFTER_RESTORE_OPTIONS`/`HV_INSTALL_G6` 가드를 G4/G5와 동일한 패턴으로 추가

## 10. 구현 범위 (이 문서 승인 범위 — 아직 미구현)

포함: `hv_stop.h` 확장, `restore.c`에 정지점 E·`rp_started` 로컬 변수 추가, `host/install_g6.{h,c}`, `host/install.c`/`host/vmrun.c` 연결, `tools/vmrun-install-g6-build.sh`, `tools/restore-selftest-build.sh`/`check-no-test-flags.sh` 확장, Android `G6Verdict.java` + `[G6]` 버튼, CI 빌드 스텝.

제외(별도 명시적 승인 필요): `restored_start_restore()` 실제 호출, ASR 실제 이미지 전송, 디스크 쓰기, "G6~G8 통합 설치기" 파이프라인.

## 11. 검증 계획

- 정상 경로: 네이티브 `restore-selftest-build.sh` 링크 확인 + `-Wall -Wextra` 단독 컴파일 + `check-no-test-flags.sh` (이번 세션 환경은 ARM64 NDK 없음 — 실제 링크·실기기 시험은 CI와 실기기로)
- 실패 주입: 가능하면 `reverse_proxy_client_free()`가 `REVERSE_PROXY_E_UNKNOWN_ERROR`를 반환하도록 디버그 후크로 강제해, §4 "정리 불확실 경로"가 실제로 FAILED를 보고하고 `client->restore`를 해제하지 않는지 확인(코드 리뷰로 경로 존재만 확인하고 실기기 주입 시험까지 할지는 구현 단계에서 별도 판단)
- 실기기: G5와 동일하게 `[G6]` 버튼으로 1회 읽기 전용 실행, 판정표 전체 `ok` 확인
