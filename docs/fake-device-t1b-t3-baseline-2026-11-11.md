# T1b·T3 변경 전 기준선 (2026-11-11)

범위: 제품 코드 변경 전 `restore_device()`의 T1b(종료 뒤 장치 메시지 도착)와 T3(스레드 생성 실패)를 가짜 장치와 링크 단계 대체로 기록한다. 시험 코드만 추가했고 제품 코드·벤더링 소스·확정 설계는 바꾸지 않았다. 판정 없음(기록). 표기: [관측], [추론], [미검증].

## 1. 구성
- T1b (`t1b`): 가짜 restored가 StatusMsg 0으로 정상 종료시킨다. 호스트의 Goodbye에 답한 0.3초 뒤와 0.6초 뒤 같은 연결에 AsyncDataRequestMsg(URLAsset, DataPort 포함)를 보낸다. 데이터 포트는 접속을 수락하도록 해 두어, 호스트가 처리했다면 접속 기록이 남는다.
- T3 (`t3_inject`): `tests/fake_device/wrap_thread.c`가 `thread_new`를 가로채(`-Wl,--wrap=thread_new`, 평소에는 그대로 전달) 시험이 걸어 둔 1회에 -1을 돌려준다. 가짜 장치가 AsyncDataRequestMsg 뒤에 알 수 없는 메시지를 보내 루프를 끝낸다. 벤더링 소스 변경 없음.
- 이후 모든 사례는 이 래퍼를 링크한다(전달 모드). S1~S3 사례가 이 때문에 달라지지 않았음은 5단계 회귀에서 확인한다.

## 2. 관측 [관측] (일반 빌드 3회, ASan 1회, TSan 1회; 결과 동일)

| 사례 | 반환값 | 요점 |
|---|---|---|
| t1b | 0, 1.6 s | 장치의 늦은 송신 2회가 모두 실패(`rc=-1, errno=32` EPIPE): 호스트가 이미 연결을 닫은 뒤라 메시지는 전달되지 않았다. 데이터 포트 접속 0, `thread_new` 호출 0. ASan·TSan 지적 없음 |
| t3_inject | -1, 1.6 s | `thread_new` 호출 1회, 주입 실패 1회. 로그 "Failed to start async data request handler thread!". ASan 지적 없음(해제 후 접근 없음) |
| t3_inject + LeakSanitizer | 위와 같음 | 대조 사례(비동기 없음)의 누수 104바이트/4건에 비해 1211바이트/42건. 추가분의 할당 위치가 `restore_device`의 `plist_copy`(`restore.c:5785`, 비동기 인수의 `message` 사본). 소스에서 실패 분기가 `free(args)`만 하는 것과 일치 |

## 3. 추론
- T1b: 루프가 끝나고 호스트가 연결을 닫은 뒤 도착한 비동기 요청은 **호스트에 도달하지 않는다**(이 구성에서). 따라서 "종료 뒤 등록 거부"를 시험하려면 루프가 끝나기 전·끝나는 중에 요청이 오는 순서가 필요하다(T1a 영역). 장치 쪽은 EPIPE로 실패를 알 수 있을 뿐 응답 대기 상태가 되지 않는다. 시험 명세가 [미확정]으로 둔 "장치 측 대기 상태·종료 프로토콜 영향"은 이 구성에서 "장치의 송신이 EPIPE"로 좁혀졌다.
- T3: 생성 실패 때 오류가 `err = -1`로 루프에 전달되고(반환값 -1), 복사한 `message`는 해제되지 않는다. 등록·취소 개념이 아직 없으므로 활성 수 복귀 판정은 해당 없음.

## 4. 미검증
- t1b의 간격(0.3초·0.6초)보다 빠른 도착, 연결을 닫기 전의 도착. 실제 restored가 종료 뒤에 보내는지 여부.
- t3의 누수는 LeakSanitizer 한 번의 보고이며 크기는 plist 구조에 따라 달라진다.
- Linux x86_64만. Android·실기기.

## 5. 재현
`CASES="t1b t3_inject" tools/fake-restore-test.sh <빌드 디렉터리>`. 누수 확인: ASan 빌드 디렉터리에서 `ASAN_OPTIONS=detect_leaks=1 FAKE_LEAKCHECK=1 CC="cc -fsanitize=address -g" CASES=t3_inject tools/fake-restore-test.sh <dir>` 후 `case_t3_inject.log`에서 `restore.c:5785` 검색.
