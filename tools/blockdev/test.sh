#!/usr/bin/env bash
# Run against a fresh loop image and a private openHalo cluster.
set -euo pipefail
if [[ $# != 3 || $EUID != 0 ]]; then
  echo "usage (as root): $0 INSTALL_PREFIX database_os_user scratch_directory" >&2
  exit 2
fi
bin=$(realpath "$1")/bin
user=$2
[[ $(id -u "$user") != 0 && -x "$bin/pg_ctl" ]]
scratch=$(realpath "$3")
# Keep the device node and backing image outside the database user's writable tree.
[[ $(stat -c %u "$scratch") == 0 ]]
mode=$(stat -c %a "$scratch")
(( (8#$mode & 0022) == 0 || (8#$mode & 01000) != 0 ))
work=$(mktemp -d "$scratch/blockdev-test.XXXXXXXX")
run=$work/run
data=$run/data
sock=$run/socket
image=$work/volume.img
port=55439
loop=''
asdb() { runuser -u "$user" -- "$@"; }
sql() { asdb "$bin/psql" -X -v ON_ERROR_STOP=1 -h "$sock" -p "$port" -d halo0root "$@"; }
stop() { asdb "$bin/pg_ctl" -w -D "$data" -m fast stop; }
start() { asdb "$bin/pg_ctl" -w -D "$data" -l "$run/server.log" start; }
cleanup() {
  if [[ -e "$data/postmaster.pid" ]]; then stop || return 1; fi
  if [[ -n "$loop" ]]; then
    rm -f "$work/device"
    losetup -d "$loop"
  fi
}
trap cleanup EXIT
echo "Test artifacts: $work"
chmod 0711 "$work"
install -d -m 0700 -o "$user" "$run"
asdb mkdir -m 700 "$sock" "$run/tablespace"
truncate -s 16M "$image"
printf OHBLK8K1 | dd of="$image" conv=notrunc status=none
loop=$(losetup --find --show "$image")
# Give the test user a private device node, with no access to other disks.
major=$(stat -c %t "$loop")
minor=$(stat -c %T "$loop")
mknod -m 0600 "$work/device" b "$((16#$major))" "$((16#$minor))"
chown "$user" "$work/device"
asdb "$bin/initdb" -D "$data" --no-locale --encoding=UTF8 --auth-local=peer --auth-host=reject > "$work/init.log"
asdb tee -a "$data/postgresql.conf" >/dev/null <<CONFIG
listen_addresses = ''
unix_socket_directories = '$sock'
port = $port
shared_buffers = '16MB'
max_connections = 10
CONFIG
start
sql -c "CREATE TABLESPACE labblock LOCATION '$run/tablespace'"
oid=$(sql -Atc "SELECT oid FROM pg_tablespace WHERE spcname='labblock'")
stop
printf '%s\n' "$oid" | asdb tee "$data/openhalo_block.oid" >/dev/null
asdb ln -s "$work/device" "$data/openhalo.block"
start
sql <<'SQL'
CREATE TABLE device_rows(id int PRIMARY KEY USING INDEX TABLESPACE labblock,
                         payload text NOT NULL) TABLESPACE labblock;
INSERT INTO device_rows SELECT i, 'oh-block-row-' || i FROM generate_series(1,1000) i;
UPDATE device_rows SET payload='oh-block-updated-500' WHERE id=500;
DELETE FROM device_rows WHERE id % 10 = 0 AND id <> 500;
CREATE TABLE other_rows(id int, payload text) TABLESPACE labblock;
INSERT INTO other_rows VALUES (7,'kept apart');
CREATE TABLE ordinary_rows(x int);
INSERT INTO ordinary_rows VALUES (42);
BEGIN;
CREATE TABLE rolled_back(a int) TABLESPACE labblock;
INSERT INTO rolled_back VALUES(1);
ROLLBACK;
CHECKPOINT;
SQL
verify() {
  sql -Atc "SELECT count(*), sum(id), max(payload) FILTER (WHERE id=500),
    md5(string_agg(id::text || ':' || payload, ',' ORDER BY id)) FROM device_rows"
}
expected=$(sql -Atc "SELECT count(*), sum(i), max(s) FILTER (WHERE i=500),
    md5(string_agg(i::text || ':' || s, ',' ORDER BY i))
    FROM (SELECT i, CASE WHEN i=500 THEN 'oh-block-updated-500' ELSE 'oh-block-row-'||i END s
      FROM generate_series(1,1000) i WHERE i%10<>0 OR i=500) t")
before=$(verify)
echo "Before restart: $before"
[[ "$before" == "$expected" ]]
path=$(sql -Atc "SELECT pg_relation_filepath('device_rows')")
[[ ! -e "$data/$path" ]]
ordinary=$(sql -Atc "SELECT pg_relation_filepath('ordinary_rows')")
[[ -s "$data/$ordinary" ]]
stop
# Read the actual device. The row marker occurs in heap data, not in the directory.
LC_ALL=C grep -aFq 'oh-block-updated-500' "$work/device"
start
after=$(verify)
echo "After restart: $after"
[[ "$after" == "$expected" ]]
[[ $(sql -Atc "SELECT payload FROM other_rows WHERE id=7") == 'kept apart' ]]
[[ $(sql -Atc 'SELECT x FROM ordinary_rows') == 42 ]]
[[ $(sql -Atc "SELECT pg_relation_size('device_rows') >= 8192") == t ]]
sql -c "SET enable_seqscan=off; EXPLAIN (COSTS OFF) SELECT payload FROM device_rows WHERE id=500"
[[ $(sql -qAtc "SET enable_seqscan=off; SELECT payload FROM device_rows WHERE id=500") == 'oh-block-updated-500' ]]
sql -c 'TRUNCATE other_rows'
sql -c "INSERT INTO other_rows VALUES (9,'reused')"
[[ $(sql -Atc 'SELECT payload FROM other_rows') == reused ]]
sql -c 'DROP TABLE other_rows, device_rows'
sql -c 'CHECKPOINT'
# Dropping the tables and their forks must free all metadata slots.
python3 - "$work/device" <<'PY'
import sys
with open(sys.argv[1], 'rb') as dev:
    directory = dev.read(8192)
assert directory[:8] == b'OHBLK8K1'
assert directory[512:] == bytes(8192-512), 'leaked device slot'
PY
echo 'PASS: device I/O, two tables, index, rollback, restart, truncate, drop and ordinary tablespace'
