#!/usr/bin/env bash
# [functional.sh] 기능 테스트 — 실제 서버를 띄우고 curl로 응답 확인
# 사용: make && tests/functional.sh        (PORT=18080 기본. PORT ~ PORT+4 포트를 쓴다)
set -u

PORT=${PORT:-18080}
B1=$((PORT + 1))        # 백엔드 1
B2=$((PORT + 2))        # 백엔드 2
NOFWD=$((PORT + 3))     # Forward Proxy를 켜지 않은 서버
SLOW=$((PORT + 4))      # 응답하지 않는 백엔드 (504 확인용)

cd "$(dirname "$0")/.." || exit 1
TMP=$(mktemp -d)
PIDS=()
PASS=0
FAIL=0

cleanup() {
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

# check <이름> <기대값> <실제값>
check() {
    if [ "$2" == "$3" ]; then
        PASS=$((PASS + 1)); echo "  ok   $1"
    else
        FAIL=$((FAIL + 1)); echo "  FAIL $1 (기대: $2, 실제: $3)"
    fi
}
# check_contains <이름> <포함돼야 할 문자열> <실제값>
check_contains() {
    if [[ "$3" == *"$2"* ]]; then
        PASS=$((PASS + 1)); echo "  ok   $1"
    else
        FAIL=$((FAIL + 1)); echo "  FAIL $1 ('$2' 없음, 실제: $3)"
    fi
}
# 환경변수 http_proxy/NO_PROXY가 있어도 서버에 직접 붙도록 한다 (Forward 테스트만 -x로 프록시를 지정)
status() { curl -s -o /dev/null -w '%{http_code}' --max-time 15 "$@"; }
# 날것의 요청을 소켓으로 보내고 첫 줄(상태 라인)을 돌려준다
raw_status() {
    exec 3<>"/dev/tcp/127.0.0.1/$PORT"
    printf "$1" >&3
    read -r -t 5 line <&3
    exec 3<&- 3>&-
    echo "${line%$'\r'}"
}
wait_port() {
    for _ in $(seq 1 50); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0
        sleep 0.1
    done
    echo "서버가 $1 포트에서 시작하지 않음"; exit 1
}

# ── 준비: 백엔드 2개(서로 다른 내용), 라우팅 설정, 메인 서버 ──────────────
mkdir -p "$TMP/b1/api" "$TMP/b2/api"
echo "backend-1" > "$TMP/b1/api/index.html"
echo "backend-2" > "$TMP/b2/api/index.html"
printf '/api/ 127.0.0.1:%s 127.0.0.1:%s\n/slow/ 127.0.0.1:%s\n' "$B1" "$B2" "$SLOW" > "$TMP/routes.conf"

./bin/server -p "$B1" -r "$TMP/b1" & PIDS+=($!); B1_PID=$!
./bin/server -p "$B2" -r "$TMP/b2" & PIDS+=($!); B2_PID=$!
./bin/server -p "$PORT" -r ./www -c "$TMP/routes.conf" -f & PIDS+=($!)
./bin/server -p "$NOFWD" -r ./www & PIDS+=($!)
for p in "$B1" "$B2" "$PORT" "$NOFWD"; do wait_port "$p"; done > /dev/null

echo "== 정적 파일"
check "GET /index.html → 200" 200 "$(status "http://127.0.0.1:$PORT/index.html")"
check "GET / → index.html" 200 "$(status "http://127.0.0.1:$PORT/")"
check_contains "text/html" "text/html" "$(curl -sI "http://127.0.0.1:$PORT/index.html")"
check_contains "text/css" "text/css" "$(curl -sI "http://127.0.0.1:$PORT/style.css")"
check_contains "application/javascript" "application/javascript" "$(curl -sI "http://127.0.0.1:$PORT/app.js")"
check "GET /nope.html → 404" 404 "$(status "http://127.0.0.1:$PORT/nope.html")"
check "GET /../Makefile → 403" 403 "$(status --path-as-is "http://127.0.0.1:$PORT/../Makefile")"
check "GET /%2e%2e/Makefile → 404 (디코딩 안 함)" 404 "$(status --path-as-is "http://127.0.0.1:$PORT/%2e%2e/Makefile")"
check "HEAD → 200, 본문 없음" "0" "$(curl -sI "http://127.0.0.1:$PORT/index.html" | grep -ci '^<html')"
check "POST /index.html → 405" 405 "$(status -X POST -d x=1 "http://127.0.0.1:$PORT/index.html")"
check "DELETE → 501" 501 "$(status -X DELETE "http://127.0.0.1:$PORT/index.html")"

echo "== 잘못된 요청"
check "BAD 요청 → 400" "HTTP/1.1 400 Bad Request" "$(raw_status 'BAD\r\n\r\n')"
check "Host 없는 HTTP/1.1 → 400" "HTTP/1.1 400 Bad Request" "$(raw_status 'GET / HTTP/1.1\r\n\r\n')"
check "HTTP/2.0 → 505" "HTTP/1.1 505 HTTP Version Not Supported" "$(raw_status 'GET / HTTP/2.0\r\nHost: x\r\n\r\n')"
check "POST 길이 없음 → 411" "HTTP/1.1 411 Length Required" "$(raw_status 'POST / HTTP/1.1\r\nHost: x\r\n\r\n')"
check "chunked → 501" "HTTP/1.1 501 Not Implemented" "$(raw_status 'POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n')"
check "본문 1MB 초과 → 413" "HTTP/1.1 413 Payload Too Large" "$(raw_status 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n')"
LONG_URI=$(head -c 3000 /dev/zero | tr '\0' 'a')
check "URI 2KB 초과 → 414" "HTTP/1.1 414 URI Too Long" "$(raw_status "GET /$LONG_URI HTTP/1.1\r\nHost: x\r\n\r\n")"

echo "== keep-alive / HTTP/1.0"
REUSED=$(curl -sv "http://127.0.0.1:$PORT/index.html" "http://127.0.0.1:$PORT/style.css" -o /dev/null -o /dev/null 2>&1 | grep -c 'Re-using existing')
check "연결 재사용" 1 "$REUSED"
check "HTTP/1.0 → 200" 200 "$(status -0 "http://127.0.0.1:$PORT/index.html")"
exec 3<>"/dev/tcp/127.0.0.1/$PORT"
START=$SECONDS
read -r -t 10 line <&3      # 아무 요청도 안 보내면 유휴 타임아웃(5초)으로 서버가 연결을 닫는다
exec 3<&- 3>&-
ELAPSED=$((SECONDS - START))
check "유휴 연결은 5초 안팎에 닫힘" "yes" "$([ "$ELAPSED" -ge 4 ] && [ "$ELAPSED" -le 7 ] && echo yes || echo "no(${ELAPSED}s)")"

echo "== CGI"
check_contains "POST /cgi-bin/adder → 3" "1 + 2 = 3" "$(curl -s -d 'a=1&b=2' "http://127.0.0.1:$PORT/cgi-bin/adder")"
check_contains "GET /cgi-bin/adder?a=5&b=7 → 12" "5 + 7 = 12" "$(curl -s "http://127.0.0.1:$PORT/cgi-bin/adder?a=5&b=7")"
check "없는 CGI → 404" 404 "$(status "http://127.0.0.1:$PORT/cgi-bin/nope")"
check "CGI ../ → 403" 403 "$(status --path-as-is "http://127.0.0.1:$PORT/cgi-bin/../Makefile")"
check "CGI 폴더 자체 → 403 또는 404" "yes" "$(s=$(status "http://127.0.0.1:$PORT/cgi-bin/"); [ "$s" == 403 ] || [ "$s" == 404 ] && echo yes || echo "$s")"
check "좀비 프로세스 없음" 0 "$(ps -eo stat,comm | grep -c '^Z.*adder')"

echo "== Reverse Proxy (Round-Robin)"
R=""
for _ in 1 2 3 4; do R+="$(curl -s "http://127.0.0.1:$PORT/api/") "; done
check_contains "backend-1 응답" "backend-1" "$R"
check_contains "backend-2 응답" "backend-2" "$R"
FIRST=$(curl -s "http://127.0.0.1:$PORT/api/"); SECOND=$(curl -s "http://127.0.0.1:$PORT/api/")
check "연속 요청은 서로 다른 백엔드" "yes" "$([ "$FIRST" != "$SECOND" ] && echo yes || echo no)"

echo "== Forward Proxy"
check_contains "-f 켠 서버: 절대 URI 중계" "backend-1" "$(curl -s --noproxy "" -x "127.0.0.1:$PORT" "http://127.0.0.1:$B1/api/")"
check "원 서버 404 그대로 전달" 404 "$(status --noproxy "" -x "127.0.0.1:$PORT" "http://127.0.0.1:$B1/none.html")"
check "연결 안 되는 원 서버 → 502" 502 "$(status --noproxy "" -x "127.0.0.1:$PORT" "http://127.0.0.1:1/")"
check "-f 없는 서버 → 403" 403 "$(status --noproxy "" -x "127.0.0.1:$NOFWD" "http://127.0.0.1:$B1/api/")"
check "POST 중계" 200 "$(status --noproxy "" -x "127.0.0.1:$PORT" -d 'a=1&b=2' "http://127.0.0.1:$PORT/cgi-bin/adder")"

echo "== 동시 접속"
CURL_PIDS=()
for i in $(seq 1 30); do
    { status "http://127.0.0.1:$PORT/index.html"; echo; } > "$TMP/c$i" & CURL_PIDS+=($!)
done
wait "${CURL_PIDS[@]}"          # 서버가 아니라 curl들만 기다린다
OK=$(cat "$TMP"/c* 2>/dev/null | grep -c '^200$')
check "동시 30개 요청 모두 200" 30 "$OK"

echo "== 장애 대응"
if command -v python3 > /dev/null; then
    python3 -c "
import socket, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', $SLOW)); s.listen(5)
c, _ = s.accept(); time.sleep(12)
" & PIDS+=($!)
    sleep 0.5
    check "응답 없는 백엔드 → 504" 504 "$(status "http://127.0.0.1:$PORT/slow/")"
fi
kill "$B1_PID"; wait "$B1_PID" 2>/dev/null
OK=0; for _ in 1 2 3 4; do [ "$(curl -s "http://127.0.0.1:$PORT/api/")" == "backend-2" ] && OK=$((OK + 1)); done
check "백엔드 1개 꺼져도 나머지로 응답" 4 "$OK"
kill "$B2_PID"; wait "$B2_PID" 2>/dev/null
check "백엔드 모두 꺼짐 → 502" 502 "$(status "http://127.0.0.1:$PORT/api/")"
check "프록시 서버는 계속 살아 있음" 200 "$(status "http://127.0.0.1:$PORT/index.html")"

echo
echo "통과 $PASS / 실패 $FAIL"
[ "$FAIL" -eq 0 ]
