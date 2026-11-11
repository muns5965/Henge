# T5 처리기 조사와 시험 방식 결정 (S2) — 기준선 기록 (2026-11-11)

범위: `docs/fake-device-impl-scope.md` §4.2·S2. (1) 데이터 요청 처리기의 첫 외부 자원 의존을 소스로 조사(읽기 전용), (2) T5 비동기 부분의 시험 방식 (A)/(B)/(C) 결정, (3) 가짜 장치로 T5 동기·비동기 부분의 **변경 전** 동작 기록.
제품 코드, 벤더링 소스, 확정 설계(D1~D8, C1~C19)는 변경하지 않았다. 후크 없음. 통과/실패 판정 없음(기록만). 실기기·Android 실행 아님.
표기: [관측]=실행 결과, [소스]=소스 독해(실행 안 함), [추론], [미검증].

## 1. 처리기 조사 [소스]

대상: `restore_handle_data_request_msg`(`restore.c`)가 분기하는 DataType. 비동기 스레드(`_restore_handle_async_data_request`)도 같은 함수를 호출한다(F7). 아래는 "build_identity가 빈 dict이고 client->tss·ipsw·recovery_variant가 NULL이며 가짜 장치만 있을 때" 각 처리기가 어디까지 가는지다. `restore_device`는 호출자가 준 build_identity 포인터를 `client->restore->build_identity`에 넣으므로, 빈 dict면 NULL 검사는 통과한다.

| DataType | 처리기 | 첫 자원 의존 | 빈 build_identity로 장치 접속까지? |
|---|---|---|---|
| FDRTrustData | restore_send_fdr_trust_data | DataPort가 있으면 장치에 서비스 접속 후 빈 dict 송신 | **예**(build_identity 미사용) — 실행함(t5s_fdr) |
| BasebandUpdaterOutputData | restore_handle_baseband_updater_output_data | `idevice_connect(DataPort)` 최대 9회 재시도(실패 시 1초 대기) 후 연결이 닫힐 때까지 수신 | **예** — 실행함(t5s_bb) |
| URLAsset | restore_send_url_asset | Arguments 검사 → 호스트 정책(`hv_urlasset_guard`)이 거부하면 DataPort로 거부 응답 송신 | **예**(정책 거부 경로) — 실행함(t5a_*) |
| BootabilityBundle(V2) | restore_send_bootability_bundle_data | `idevice_connect(DataPort)` 재시도 | 예(추정; 앞부분만 읽음, 실행 안 함) |
| BuildIdentityDict | restore_send_buildidentity | 서비스 접속이 먼저 | 예(추정; 실행 안 함) |
| FirmwareUpdaterPreflight, DeviceRestoreInfoPreflight | restore_send_firmware_updater_preflight | 서비스 접속이 앞부분에 있음 | 예(추정; 뒷부분 미정독, 실행 안 함) |
| RootTicket, RecoveryOSRootTicketData | restore_send_root_ticket 등 | `client->tss` 없고 `FLAG_CUSTOM` 아니면 -1. FLAG_CUSTOM이면 서비스 접속 후 송신 | 기본 아니오 |
| SystemImageData, RecoveryOSASRImage | restore_send_filesystem | OS 컴포넌트 경로를 build_identity에서 못 찾아 -1 | 아니오 |
| KernelCache, DeviceTree, SystemImageRootHash, SystemImageCanonicalMetadata | restore_send_component | 컴포넌트 경로 실패 -1 | 아니오 |
| NORData, BasebandData, PersonalizedData, FUDData, EANData, FirmwareUpdaterData | (각 처리기) | build_identity 의존(앞부분만 확인) | 아니오(추정) |
| ReceiptManifest | restore_send_receipt_manifest | build_identity에 `Manifest`가 없으면 -1 | 아니오 |
| PersonalizedBootObjectV3, SourceBootObjectV4/V5, RecoveryOSLocalPolicy | (각 처리기) | 초기 실패 경로(TSS·build_identity) | 아니오(추정) |
| RecoveryOSFileAssetImage, RecoveryOSIBootFWFilesImages, RecoveryOSImage, RecoveryOSVersionData | (각 처리기) | `client->recovery_variant` 필요 | 아니오 |
| StreamedImageDecryptionKey | restore_send_streamed_image_decryption_key | 인수 검사 후 외부 HTTP 요청(curl 대체) | 이번 시험 대상 아님 |

대기 한도 [소스]:
- usbmuxd 접속 응답 대기: 5000 ms(`usbmuxd_get_result`). 응답이 없으면 접속 실패.
- 접속이 성립한 뒤 `idevice_connection_receive`(BasebandUpdaterOutputData의 수신 루프): 시간 제한 없는 수신.
- restored plist 수신: 기본 30000 ms.
- 비동기 인수 광고: `restore_device`는 `SupportedAsyncDataTypes`에서 URLAsset만 true로 둔다. 비동기 분기는 DataType을 가리지 않고 같은 함수를 호출한다.

## 2. 결정: (A) 성립 [관측 + 소스]

T5 비동기 부분은 **빈 build_identity와 가짜 장치만으로** 재현할 수 있다. 현실에서 비동기로 광고되는 URLAsset이 호스트 정책 거부 경로에서 DataPort 접속에 도달하기 때문이다. IPSW·빌드 아이덴티티 고정물((B))과 후크((C))는 필요하지 않다. 결정은 이 시험 한 형태에 대한 것이며, 실제 복원에서의 비동기 요청 형태 전부를 다루지는 않는다([미검증]).
S3의 범위(가짜 프록시, T12·T13)는 이 결정으로 바꾸지 않았다.

## 3. 시험 구성 (모두 새 시험 코드)

- 가짜 mux가 `FAKE_DATA_PORT`(4444) 접속에 대해 동작을 고른다: 수락 후 침묵 / 응답 없음 / 지연 후 거절 / 지연 후 수락.
- 사례: 
  - t5s_bb: DataRequestMsg(BasebandUpdaterOutputData)+ 접속 수락 후 침묵. 감시 한도 45초.
  - t5s_fdr: DataRequestMsg(FDRTrustData), 접속 수락, 이어서 StatusMsg 0(완료).
  - t5s_fdr_nr: 같은 메시지, mux가 이 접속에 응답하지 않음.
  - t5a_free / t5a_nofree: AsyncDataRequestMsg(URLAsset, DataPort), StatusMsg(실패), 알 수 없는 MsgType 메시지 순으로 보냄(루프가 즉시 끝나도록). 데이터 포트 접속은 3초 뒤 거절. `restore_device` 반환 후 t5a_free는 호스트처럼 `idevicerestore_client_free`를 호출하고, 둘 다 6초 더 기다린 뒤 종료.
- 추가: 사례는 임시 디렉터리에서 실행한다(BasebandUpdaterOutputData가 `updater_output-<가짜 UDID>.cpio`를 현재 디렉터리에 쓰므로).

## 4. 관측 [관측] (같은 사례를 일반 빌드 2회, ASan 2회, TSan 2회 실행; 결과 동일)

| 사례 | 일반 빌드 | ASan | TSan |
|---|---|---|---|
| t5s_fdr | 0, 1.6 s. 가짜 데이터 포트가 185바이트 수신 | — | — |
| t5s_fdr_nr | **0**, 6.5 s. 로그: 5초 뒤 "Failed to start service connection", "Unable to send FDR Trust data", "Unable to successfully restore device", 이어서 "Status: Restore Finished" | — | — |
| t5s_bb | **45초 안에 반환 없음**(감시 만료 `HANG OBSERVED`, 종료 코드 124) | — | — |
| t5a_nofree | -1, 1.6 s. 로그: 3초 뒤 "Unable to send URLAsset data" | 지적 없음 | 지적 없음 |
| t5a_free | -1, 1.6 s. 일반 빌드는 증상 없음 | **heap-use-after-free**, 쓰기 4바이트, `restore.c:5161`(`client->async_err = err`), 해제는 `idevicerestore_client_free`(`idevicerestore.c:1723`) | **heap-use-after-free** 쓰기(5161)와 읽기(5162, `client->flags`) |

보조 관측:
- 모든 사례에서 net_guard 거부 0, 외부 연결 없음, 감시 만료는 t5s_bb 한 건뿐.
- 대조 사례(S1의 사례 0)는 새 코드로 재실행해도 0 / 1.6초, ASan·TSan 지적 없음.
- fd 수 측정은 가짜 장치의 소켓도 같은 프로세스에 있어 호스트 쪽 누수를 구분하지 못한다. t5s_fdr 한 번의 반복에서 끝 fd가 1개 늘었다(원인 미확인; 가짜 장치 쪽 소켓일 수 있음).

## 5. 추론 (설계 문서의 소스 분석과 대응)

- t5a_free의 해제 후 쓰기는 T5가 지적하던 위험(P-비동기 스레드가 해제된 `client`를 참조)이 **현재 코드에서 실제로 일어남**을 보인다. 조건: 비동기 처리기가 `restore_device`보다 오래 살고, 호스트가 반환 직후 `client`를 해제하며, 처리기가 음수를 돌려줌.
- t5a_nofree에서 지적이 없는 것은 해제가 없으면 같은 쓰기가 유효 메모리에 닿기 때문이다. 즉 문제는 스레드가 아니라 수명이다.
- t5s_fdr_nr: 동기 처리기가 -1을 돌려줘도 뒤따르는 StatusMsg(0)가 `err`를 0으로 덮어 최종 0이 된다. S1의 (b)와 같은 덮어쓰기 경로의 다른 실례다.
- t5s_bb: 연결이 성립한 뒤 장치가 침묵하면 수신 루프에 시간 제한이 없다는 소스 독해와 일치한다. 감시 시간(45초) 안에 반환하지 않았다는 것만 관측했고, 영구 정지임을 입증하지는 않았다.

## 6. 미검증
- Android, 실기기, 실제 restored. 가짜 장치 응답은 소스에서 구성한 것이다.
- 표의 "추정" 항목(BootabilityBundle, BuildIdentityDict, FirmwareUpdater*, NOR 등)은 일부만 읽었고 실행하지 않았다.
- 정책이 허용하는 URLAsset(외부 HTTP 요청) 경로, StreamedImageDecryptionKey 경로.
- t5a_free의 쓰기가 실제 호스트 흐름에서도 같은 시점에 일어나는지(호스트의 해제 시점은 이번 시험의 `idevicerestore_client_free` 호출로 대신함).
- 이 기록은 I1~I10 구현 승인이나 V5·V9·V11~V14 해소를 뜻하지 않는다.

## 7. 재현
`tools/fake-restore-test.sh <빌드 디렉터리>` (`CASES="t5s_bb t5s_fdr t5s_fdr_nr t5a_free t5a_nofree"`). 새니타이저는 같은 스크립트를 `CC="cc -fsanitize=address -g"`(또는 `thread`)로 만든 다른 빌드 디렉터리에 적용한다(이 경우 restore-selftest-build.sh도 같은 CC로 먼저 빌드되어야 한다). 로그는 `<빌드 디렉터리>/fd/case_<id>.out/.log`.
