#!/bin/bash
# Нагрузочное сравнение HTTP-серверов: coroed_server (корутины + epoll)
# и threaded_server (поток ОС на соединение).
#
# Запуск из корня репозитория:
#   ./bench/bench.sh [длительность_с] [повторов]
#
# Параметры через переменные окружения:
#   CONNECTIONS  -- список чисел соединений (по умолчанию "10 50 100 250 500 1000 2000")
#   SERVER_CPUS  -- ядра для сервера (taskset), по умолчанию 0-7
#   CLIENT_CPUS  -- ядра для wrk, по умолчанию 8-15
#   WRK_THREADS  -- потоки wrk, по умолчанию 4
#
# Результат: bench/results/results.csv, графики строит bench/plot.py.

set -u
cd "$(dirname "$0")/.." || exit 1

DURATION=${1:-10}
RUNS=${2:-3}
CONNECTIONS=${CONNECTIONS:-"10 50 100 250 500 1000 2000"}
SERVER_CPUS=${SERVER_CPUS:-0-7}
CLIENT_CPUS=${CLIENT_CPUS:-8-15}
WRK_THREADS=${WRK_THREADS:-4}
OUT=bench/results
CSV=$OUT/results.csv

mkdir -p "$OUT"
ulimit -n 65535 || exit 1

# Coroed по умолчанию держит до 512 задач; для 2000 соединений
# нужно больше слотов, поэтому лимит поднимается при сборке.
echo "Сборка серверов (-O3, COROED_MAX_TASKS=4096)..."
make clean > /dev/null
if ! make server CFLAGS_EXTRA=-DCOROED_MAX_TASKS=4096 > "$OUT/build.log" 2>&1; then
  cat "$OUT/build.log"
  exit 1
fi

# Перевести значение задержки wrk (us/ms/s) в миллисекунды
to_ms() {
  awk -v v="$1" 'BEGIN {
    if (v ~ /us$/) { sub(/us$/, "", v); printf "%.3f", v / 1000 }
    else if (v ~ /ms$/) { sub(/ms$/, "", v); printf "%.3f", v }
    else if (v ~ /s$/) { sub(/s$/, "", v); printf "%.3f", v * 1000 }
    else { printf "%s", v }
  }'
}

wait_port() {
  for _ in $(seq 1 50); do
    curl -s -o /dev/null "http://127.0.0.1:$1/hello/ping" && return 0
    sleep 0.1
  done
  return 1
}

echo "server,connections,run,rps,p50_ms,p90_ms,p99_ms,errors,rss_mb,threads" > "$CSV"

run_server() {
  local name=$1 binary=$2 port=$3

  taskset -c "$SERVER_CPUS" "./build/bin/$binary" "$port" > "$OUT/$name.log" 2>&1 &
  local pid=$!
  if ! wait_port "$port"; then
    echo "$name не запустился"
    kill "$pid" 2> /dev/null
    return 1
  fi

  # Прогрев
  taskset -c "$CLIENT_CPUS" wrk -t"$WRK_THREADS" -c100 -d3s "http://127.0.0.1:$port/hello/bench" > /dev/null

  for c in $CONNECTIONS; do
    local threads=$WRK_THREADS
    [ "$c" -lt "$threads" ] && threads=$c
    for run in $(seq 1 "$RUNS"); do
      local tmp
      tmp=$(mktemp)
      taskset -c "$CLIENT_CPUS" wrk -t"$threads" -c"$c" -d"${DURATION}s" --timeout 5s --latency \
        "http://127.0.0.1:$port/hello/bench" > "$tmp" 2>&1 &
      local wrk_pid=$!

      # Память и число потоков сервера в середине прогона
      sleep $((DURATION / 2))
      local rss_kb threads_count
      rss_kb=$(ps -o rss= -p "$pid" | tr -d ' ')
      threads_count=$(ls "/proc/$pid/task" | wc -l)
      wait "$wrk_pid"

      local rps p50 p90 p99 socket_errors non2xx errors
      rps=$(awk '/Requests\/sec/ {print $2}' "$tmp")
      p50=$(to_ms "$(awk '$1 == "50%" {print $2}' "$tmp")")
      p90=$(to_ms "$(awk '$1 == "90%" {print $2}' "$tmp")")
      p99=$(to_ms "$(awk '$1 == "99%" {print $2}' "$tmp")")
      socket_errors=$(awk -F'[ ,]+' '/Socket errors/ {print $4 + $6 + $8 + $10}' "$tmp")
      non2xx=$(awk '/Non-2xx/ {print $NF}' "$tmp")
      errors=$(( ${socket_errors:-0} + ${non2xx:-0} ))

      echo "$name,$c,$run,$rps,$p50,$p90,$p99,$errors,$((rss_kb / 1024)),$threads_count" | tee -a "$CSV"
      rm -f "$tmp"
      sleep 1
    done
  done

  kill "$pid"
  wait "$pid" 2> /dev/null
}

run_server coroed coroed_server 18080
run_server threads threaded_server 18081

echo "Готово: $CSV"
