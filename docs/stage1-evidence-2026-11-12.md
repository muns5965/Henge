# I3~I6 1단계: 검증 기반 시험 결과 (2026-11-12)

범위: 시험 코드와 하네스 확장(`tests/test_stage1_blocking.c`, `tests/stage1_closure_main.c`, `tests/fake_device/fake_dev.{c,h}`의 분할 전송 옵션, `tools/stage1-test.sh`, `tools/stage1-link-trace.sh`)만. **벤더링 소스·I10·`main`은 변경하지 않았다.** 이 시험은 **Linux 일반 빌드(sanitizer 없음)에서 한 번씩** 돌려 동작을 **기록**한 것이다. 종료 코드는 "케이스가 끝났다"만 뜻하며 동작이 좋다는 뜻이 아니다. **Android에서는 검증되지 않았다.** 환경: 가짜 usbmuxd와 가짜 restored를 루프백에서 쓰는 기존 하네스.

## 1. 부분 프레임 (6-2의 선행 조건)

호출: `property_list_service_receive_plist_with_timeout(client, &plist, 1000)`을 반복 호출.

| 케이스 | 송신 방식 | 결과 |
|---|---|---|
| pf_full (대조) | 프레임 2개를 지연 없이 | 두 프레임 모두 정상 수신(`FRAMES_INTACT`) |
| pf_idle (대조) | 아무것도 안 보냄 | 8번 모두 `RECEIVE_TIMEOUT`(-5), 각 약 1000 ms, 프레임 소비 없음 |
| pf_p1 / p2 / p3 | 첫 프레임의 4바이트 길이를 1/2/3바이트만 먼저 보내고 2500 ms 쉰 뒤 나머지 | **세 경우 모두 `FRAMES_LOST_OR_DESYNCED`**: 1번째 호출이 **즉시(0 ms)** `PLIST_ERROR`(-2)로 실패하고, 그 뒤 호출 2·3은 1초 타임아웃, 호출 4는 **약 30.5초** 막힌 뒤 `RECEIVE_TIMEOUT`, 이후에도 프레임이 하나도 복구되지 않음 |
| pf_b | 길이는 온전히, 본문을 10바이트만 먼저 보내고 2500 ms 쉰 뒤 나머지 | 두 프레임 모두 정상(`FRAMES_INTACT`), 첫 프레임은 2500 ms 시점에 수신 |

해석(시험이 보여 준 것과 코드를 대조한 것을 구분):
- **[확인]** 길이 접두 4바이트가 쪼개져 도착하면 `internal_plist_receive_timeout`(`property_list_service.c:194~`)이 읽힌 1~3바이트로 길이를 계산(`bytes != 4` 검사 없음)해 프레임을 잘못 해석하고, 남은 바이트가 다음 호출의 "길이"가 되어 스트림이 어긋난다. 어긋난 뒤의 호출은 본문 대기(타임아웃 없는 `service_receive`, 약 30초)에서 막힌다.
- **[확인]** 이 결함은 **타임아웃 값과 무관**하다. 첫 호출은 타임아웃 1000 ms를 기다리지 않고 바이트가 도착하자마자(0 ms) 실패했다. 타임아웃은 "첫 바이트가 올 때까지 기다리는 시간"일 뿐이다. 따라서 **6-2의 타임아웃 단축이 이 결함을 만들거나 키우지 않는다**(기존 30초 경로에도 그대로 있다). 앞선 문서의 "타임아웃이 짧아지면 위험이 커질 수 있다"는 가설은 이 시험으로 **기각**한다.
- **[확인]** 본문이 쪼개져 오는 것은 이 시험에서 안전했다(pf_b).
- **[가설]** 실제 USB 터널(usbmuxd)에서 길이 접두가 쪼개져 도착할 확률은 측정하지 않았다. 하네스는 인위적으로 쪼갰다.
- **영향**: 이 기존 결함은 취소 응답성과도 연결된다(어긋난 뒤 약 30초 정지). 수정은 벤더링 변경(`property_list_service.c`의 길이 읽기를 4바이트가 찰 때까지 반복)이며 **승인 전이다.** 6-2는 이 결함과 독립적으로 진행할 수 있으나, 결함 수정 여부는 별도 결정 사항이다.

## 2. `shutdown()` 차단 해제 (S1~S5)

각 케이스는 먼저 `shutdown` 없이 1500 ms 동안 호출이 막혀 있음을 확인(대조)한 뒤 다른 스레드에서 `shutdown(fd, SHUT_RDWR)`을 호출했다.

| 케이스 | 차단 호출 | `shutdown` 뒤 결과 (Linux) |
|---|---|---|
| s1 | `socket_receive`(untimed), 연결된 침묵 소켓 | 대조 통과(막힘) → 즉시(≈0 ms) 반환, 오류 코드 반환 |
| s5 | `socket_receive_timeout` 30000 ms | 대조 통과 → 즉시 반환 |
| s1b | `idevice_connection_receive`(asr.c가 쓰는 호출), usbmuxd 터널 위 | 대조 통과 → 즉시 반환 |
| s2 | `socket_send`가 상대가 읽지 않아 가득 찬 버퍼에서 막힘 | 대조 통과 → 즉시 반환, `EPIPE` |
| s3 | 원시 `connect()`가 리스너 큐가 가득 차 SYN_SENT에서 막힘, fd는 시험이 알고 있음 | 대조 통과 → 즉시 반환, `ECONNRESET` |
| s3lib | 라이브러리 `socket_connect`(같은 조건) | **3000 ms 넘게 막힘**(자체 시간 제한 없음). fd는 반환 전까지 라이브러리 밖에 공개되지 않으므로 다른 스레드가 `shutdown`을 할 **수단이 없다** |

해석:
- **[확인, Linux]** 연결이 이미 성립한 소켓의 `recv`·`send`는 `shutdown`으로 즉시 풀린다(s1, s5, s1b, s2). 이는 6-3(ASR), 6-4(프록시 수신·송신 루프)의 `shutdown` 방식이 Linux에서는 성립함을 뜻한다.
- **[확인]** `connect` 진행 중인 소켓은 fd를 알면 풀리지만(s3), 라이브러리 `socket_connect`는 시간 제한이 없고 fd를 노출하지 않아 **프록시의 외부 호스트 연결(`reverse_proxy.c:191`)은 `shutdown`으로 취소할 수 없다**(s3lib). 이 경로는 별도 수단(연결 전 fd 공개, 논블로킹+폴링, 시간 제한)이 필요하며 지금은 **미결**이다.
- **[미확인]** Android(bionic 커널 소켓)에서도 같은 결과인지는 시험하지 않았다. Linux 결과를 Android 보장으로 쓰지 않는다. 참조 반환(다른 스레드가 같은 fd를 쓰는 중 `close` 금지) 시험과 fd 재사용 경쟁 시험은 **이번에 하지 않았다**(C14 구현 뒤 시험).

## 3. libusbmuxd 모니터 (F)

| 케이스 | 조건 | 결과 |
|---|---|---|
| s4 | Listen에 응답하고 이후 침묵하는 가짜 usbmuxd 상대로 구독 후 해제 | `usbmuxd_events_unsubscribe`가 약 0 ms에 `rc=0`으로 반환 (Linux에서 `shutdown(listenfd)`가 모니터의 대기를 풀고 join이 성공) |
| s4j | 같은 조건에서 `pthread_join` 실패를 **한 번** 주입(링크 시 래퍼, 스레드는 join되지 않음) | 해제 함수가 `rc=22(EINVAL)`을 반환했고, **`thread_free`가 아직 살아 있는 모니터 스레드에 대해 호출되었다**(`g_free_while_alive=1`) |

해석:
- **[확인, Linux]** 정상 경로에서는 모니터가 즉시 종료한다. 이 빌드에서 `thread_cancel`이 항상 실패하는데도(§7 문서) `socket_shutdown(listenfd)`+join으로 종료된다.
- **[확인, 주입]** join이 실패하면 해제 함수는 오류를 반환하지만 스레드 핸들을 해제하고 `devmon=NULL`로 만든다. 이 시험에서 스레드는 곧 스스로 끝났지만 **종료 확인 없이** 해제되었다. 호출자 `idevicerestore_client_free`는 반환값을 확인하지 않는다 [코드 확인]. 따라서 join이 실제로 실패하는 상황(드문 오류)에서는 콜백이 `client`를 참조하는 중에 해제가 완료될 수 있다. 발생 조건은 **[미확인]**(`pthread_join`이 실제로 `EINVAL`·`EDEADLK`을 내는 경우는 호출 규약 위반에 가깝다).
- F의 조건부 제외는 유지한다. 이 시험은 "join 성공 시 종료 보장"과 "join 실패 시 보장 없음"을 둘 다 보였다. 호스트가 `client_free` 반환값을 확인할 수 없는 구조라 **실패 경로는 감지되지 않는다**는 점을 위험으로 기록한다.

## 4. 서비스 스레드의 링크 닫힘

`tools/stage1-link-trace.sh`: 호스트 닫힘(`host/install.c -D__ANDROID__` 가 `idevicerestore_start`·`usbmuxd_main`을 통해 참조하는 모든 것)을 링커 맵으로 확인. 211개 보관 멤버 중 151개가 포함.

| 멤버 | 포함 | 비고 |
|---|---|---|
| `imd_ostrace.o`, `imd_bt_packet_logger.o`, `imd_syslog_relay.o`, `imd_companion_proxy.o`, `imd_installation_proxy.o`, `imd_notification_proxy.o` | **포함되지 않음** | 호스트 닫힘에서 참조되지 않음 |
| `imd_preboard.o` | 포함 | `idr_normal.o`(`preboard_client_new`)가 참조. 콜백이 NULL로 호출돼(`normal.c:469, 598`) 스레드는 만들어지지 않음 [코드 확인] |
| `idr_fdr.o`, `idr_deviceinterfaced.o` | **포함되지 않음** | B·C·G 제외 결정을 링크 닫힘으로도 확인 |
| `imd_reverse_proxy.o` | 포함 | `idr_restore.o`가 참조 (D·E 편입 대상) |
| `usbmuxdc_libusbmuxd.o` | 포함 | `imd_idevice.o`의 `usbmuxd_events_subscribe` (F) |
| `muxd_preflight.o` | 포함 | `muxd_device.o`가 참조. 단 `np_*`를 쓰는 본문은 `#ifdef HAVE_LIBIMOBILEDEVICE`(정의되지 않음) 안이라 컴파일되지 않고 `imd_notification_proxy.o`도 끌려오지 않음 |

해석과 한계:
- **[확인]** 서비스 스레드 6종은 호스트 링크 닫힘에서 **참조되지 않는다.** 이는 정적 호출 검색보다 강한 근거다. `preboard`는 포함되지만 스레드는 만들지 않는다.
- **한계**: 이는 **오브젝트 파일 단위의 참조**다. 포함된 멤버의 함수가 실행 중 호출되는지는 보여 주지 않고, 함수 포인터·`dlsym` 같은 간접 참조는 링크 시점에 보이지 않을 수 있다(포함된 멤버는 그렇다 쳐도 포함되지 않은 멤버는 링크에서 필요 없었다는 뜻이다). 그래서 **6종 제외는 "링크에서 참조되지 않음"으로 확정**하되, 실행 시 도달 여부 시험은 하지 않았다.
- `idevicerestore_deviceinterfaced`, `idr_fdr`가 이 빌드 구성에서 빠진다는 것은 `HAVE_REVERSE_PROXY` 설정과 일치한다. 설정이 바뀌면 이 시험을 다시 돌려야 한다.

## 5. 이 단계로 정리된 결정 영향

| 항목 | 전 | 후 |
|---|---|---|
| 6-2 수신 타임아웃 단축 | 부분 프레임 시험 선행 조건 | 시험 완료: 단축이 프레임 결함을 키우지 않음(결함은 타임아웃과 무관, 기존 결함). **프레이밍 수정은 별도 결정**(벤더링 변경) |
| 6-3 ASR 차단 해제 | 미확정 | 연결된 소켓의 `recv`는 Linux에서 `shutdown`으로 풀림. 오류 전파 경로(재시도 루프가 되살아나는지)는 **미확인**(구현 전 시험 필요) |
| 6-4 프록시 차단 해제 | 미확정 | 수신·송신은 `shutdown`으로 풀림. 외부 `socket_connect`는 풀 수 없음 → **별도 수단 필요(미결)** |
| F usbmuxd 모니터 | 조건부 제외 | 유지. join 실패 시 종료 확인 없이 해제되는 경로를 시험으로 확인, 호출자가 반환값을 안 봄 |
| 서비스 스레드 7종 | 잠정 제외 | 6종 제외 확정(링크 닫힘에서 미참조), preboard는 스레드 미생성 |
| usbmuxd 프리플라이트 np 스레드 | `-p` 조건부 제외 | 컴파일되지 않음(`HAVE_LIBIMOBILEDEVICE` 미정의) + `-p` |

## 6. 한계와 남은 시험

- 일반 빌드 1회씩만 실행. ASan/TSan으로 이 시험을 돌리지 않았다. S4/S4j의 `running`·`cancelling`·`listenfd` 경합은 TSan 대상이다(기준선 TSan 지적과 같은 계열).
- 이 문서는 Android 동작을 보증하지 않는다.
- 하지 않은 시험: 참조 반환·fd 재사용 경쟁(C14), 오류 전파(ASR 재시도), 실제 USB 터널의 길이 접두 분할 빈도, `thread_cancel`이 동작하는 구성.
