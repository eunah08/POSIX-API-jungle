#!/usr/bin/env bash
# [load.sh] 부하 테스트 — 동시 연결 수에 따른 처리량(RPS)과 지연 시간 측정
# 사용: 서버 실행 후 tests/load.sh      (URL=... 로 대상 변경, wrk > ab > curl 순으로 있는 도구를 사용)
# 결과를 README의 "테스트 결과 → 부하" 표에 옮겨 적는다
set -u

URL=${URL:-http://127.0.0.1:8080/index.html}
DURATION=${DURATION:-10}
LEVELS=${LEVELS:-"10 100"}      # 동시 연결 수

if ! curl -s -o /dev/null --max-time 3 "$URL"; then
    echo "서버가 응답하지 않습니다: $URL  (먼저 ./bin/server -p 8080 -r ./www 를 실행하세요)"
    exit 1
fi

echo "대상: $URL, 시간: ${DURATION}초"
for c in $LEVELS; do
    echo
    echo "== 동시 연결 $c"
    if command -v wrk > /dev/null; then
        wrk -t4 -c"$c" -d"${DURATION}s" --latency "$URL"
    elif command -v ab > /dev/null; then
        ab -k -n $((c * 100)) -c "$c" "$URL" 2>&1 | grep -E "Requests per second|Time per request|Failed requests|Non-2xx"
    else
        # wrk, ab가 없을 때: curl을 동시에 c개씩 돌려 대략적인 처리량만 잰다
        TOTAL=$((c * 20))
        START=$(date +%s.%N)
        seq 1 "$TOTAL" | xargs -P "$c" -I{} curl -s -o /dev/null "$URL"
        END=$(date +%s.%N)
        awk -v n="$TOTAL" -v s="$START" -v e="$END" 'BEGIN { printf "요청 %d개, %.2f초, 약 %.0f req/s (curl 기반 대략치)\n", n, e - s, n / (e - s) }'
    fi
done
