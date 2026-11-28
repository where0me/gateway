#!/bin/sh
# Isolated service tests: no real daemon, driver, serial device or init file is used.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d /tmp/rk3506-startup.XXXXXX)
cleanup()
{
    resolved_test_root=$(CDPATH= cd -- "$test_root" && pwd -P) || return
    case "$resolved_test_root" in
      /tmp/rk3506-startup.*) rm -rf -- "$resolved_test_root" ;;
    esac
}
trap cleanup EXIT

mkdir -p "$test_root/bin" "$test_root/work/drivers" "$test_root/sys/module" "$test_root/sys/usb"
export STARTUP_TEST_LOG="$test_root/calls"
export STARTUP_TEST_SYS="$test_root/sys"

cat > "$test_root/bin/start-stop-daemon" <<'MOCK'
#!/bin/sh
echo "daemon $*" >> "$STARTUP_TEST_LOG"
case "$1" in
  -K)
    case "${STARTUP_TEST_STATE:-stopped}" in
      managed) exit 0 ;;
      unmanaged) case " $* " in *" -p "*) exit 1 ;; *) exit 0 ;; esac ;;
      *) exit 1 ;;
    esac ;;
  -S) exit 0 ;;
esac
exit 2
MOCK

cat > "$test_root/bin/insmod" <<'MOCK'
#!/bin/sh
echo "insmod $*" >> "$STARTUP_TEST_LOG"
[ "${STARTUP_TEST_LOAD_FAIL:-0}" = 0 ] || exit 1
mkdir -p "$STARTUP_TEST_SYS/module/cdc_acm"
MOCK

cat > "$test_root/bin/sleep" <<'MOCK'
#!/bin/sh
echo "sleep $*" >> "$STARTUP_TEST_LOG"
MOCK
chmod +x "$test_root/bin/"*

sed -e "s#^DAEMON=.*#DAEMON=$test_root/work/gateway-server#" \
    -e "s#^WORKDIR=.*#WORKDIR=$test_root/work#" \
    -e "s#^PIDFILE=.*#PIDFILE=$test_root/gateway.pid#" \
    -e "s#/sys/module/cdc_acm#$test_root/sys/module/cdc_acm#g" \
    -e "s#/sys/bus/usb/drivers/cdc_acm#$test_root/sys/usb/cdc_acm#g" \
    "$repo_root/deploy/rk3506/S90zigbee-gateway" > "$test_root/service"

printf '%s\n' '{"serial_path":"/dev/ttyACM-startup-test-absent","baud_rate":921600}' > "$test_root/work/gateway_config.json"
touch "$test_root/work/drivers/cdc-acm.ko"

run_service()
{
    env PATH="$test_root/bin:$PATH" sh "$test_root/service" start > "$test_root/output" 2>&1
}

# Cold start loads the driver; missing USB waits only ten simulated seconds.
: > "$STARTUP_TEST_LOG"
run_service
[ "$(grep -c '^insmod ' "$STARTUP_TEST_LOG")" -eq 1 ]
[ "$(grep -c '^sleep ' "$STARTUP_TEST_LOG")" -eq 10 ]
[ "$(grep -c '^daemon -S ' "$STARTUP_TEST_LOG")" -eq 1 ]
grep -q 'starting backend for configuration and diagnostics' "$test_root/output"

# Already-loaded drivers must not be inserted again.
: > "$STARTUP_TEST_LOG"
run_service
! grep -q '^insmod ' "$STARTUP_TEST_LOG"
grep -q '^daemon -S ' "$STARTUP_TEST_LOG"

# An existing managed service is idempotent; a foreground process is refused.
export STARTUP_TEST_STATE=managed
: > "$STARTUP_TEST_LOG"
run_service
! grep -q '^daemon -S ' "$STARTUP_TEST_LOG"
grep -q 'already running' "$test_root/output"

export STARTUP_TEST_STATE=unmanaged
: > "$STARTUP_TEST_LOG"
if run_service; then echo 'FAIL: foreground duplicate was accepted'; exit 1; fi
! grep -q '^daemon -S ' "$STARTUP_TEST_LOG"

# Driver load failures must not start the daemon.
export STARTUP_TEST_STATE=stopped STARTUP_TEST_LOAD_FAIL=1
rmdir "$test_root/sys/module/cdc_acm"
: > "$STARTUP_TEST_LOG"
if run_service; then echo 'FAIL: driver load failure was ignored'; exit 1; fi
! grep -q '^daemon -S ' "$STARTUP_TEST_LOG"

# Existing non-USB deployments work without the optional driver file.
rm "$test_root/work/drivers/cdc-acm.ko"
printf '%s\n' '{"serial_path":"/dev/ttyS2","baud_rate":921600}' > "$test_root/work/gateway_config.json"
: > "$STARTUP_TEST_LOG"
run_service
! grep -q '^insmod ' "$STARTUP_TEST_LOG"
! grep -q '^sleep ' "$STARTUP_TEST_LOG"
grep -q '^daemon -S ' "$STARTUP_TEST_LOG"

echo 'RK3506 startup tests passed (6 scenarios).'
