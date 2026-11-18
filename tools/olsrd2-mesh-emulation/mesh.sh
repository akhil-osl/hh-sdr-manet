#!/usr/bin/env bash
# Emulated OLSRd2 mesh in Docker for F01-AK-2. See README.md.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
DOCKER=${DOCKER:-docker}
IMAGE=${IMAGE:-f01ak2-olsrd2}
NET=${NET:-f01ak2}
SUBNET=10.99.0.0/24
PREFIX=f01-

die() { echo "mesh: $*" >&2; exit 1; }
ts()  { date -u +%H:%M:%S.%3N; }
log() { echo "$(ts) mesh $*"; }

node_ip()  { local i; i=$(printf '%d' "'$1"); echo "10.99.0.$((i - 64 + 10))"; }
nodes()    { $DOCKER ps "$@" --filter "name=^${PREFIX}" --format '{{.Names}}' | sed "s/^${PREFIX}//" | sort; }
dx()       { local n=$1; shift; $DOCKER exec "${PREFIX}${n}" "$@"; }
dxi()      { local n=$1; shift; $DOCKER exec -i "${PREFIX}${n}" "$@"; }
node_mac() { dx "$1" cat /sys/class/net/eth0/address; }

apply_profile() {
    local file=$1; shift
    [ -f "$file" ] || file="$HERE/profiles/$file.env"
    [ -f "$file" ] || die "no profile $1"
    # shellcheck disable=SC1090
    ( . "$file"
      for n in "$@"; do
          dx "$n" ip link set eth0 mtu "$MTU"
          dx "$n" tc qdisc replace dev eth0 root netem \
              rate "$RATE" delay "$DELAY" "$JITTER" loss "$LOSS" limit 10000
      done
      log "profile $(basename "$file") rate=$RATE delay=$DELAY jitter=$JITTER loss=$LOSS mtu=$MTU nodes=$*" )
}

# Drop frames from the peer on both ends, so the link is cut in both directions.
link_set() {
    local op=$1 a=$2 b=$3 ma mb
    ma=$(node_mac "$a"); mb=$(node_mac "$b")
    dx "$a" nft "$op" element netdev f01 blocked "{ $mb }"
    dx "$b" nft "$op" element netdev f01 blocked "{ $ma }"
    if [ "$op" = add ]; then log "link_down $a-$b"; else log "link_up $a-$b"; fi
}

cmd_build() {
    $DOCKER build -t "$IMAGE" "$HERE"
}

cmd_up() {
    local count=${1:-3} topo=${2:-chain} profile=${3:-default} names=() i n
    [ "$count" -ge 2 ] && [ "$count" -le 26 ] || die "node count must be 2..26"
    [ -f "$topo" ] || topo="$HERE/topologies/$topo.txt"
    [ -f "$topo" ] || die "no topology $2"
    [ -z "$(nodes -a)" ] || die "mesh already up or left stopped; run '$0 down' first"

    $DOCKER network inspect "$NET" >/dev/null 2>&1 || \
        $DOCKER network create --internal --subnet "$SUBNET" "$NET" >/dev/null
    for ((i = 0; i < count; i++)); do
        n=$(printf "\\$(printf '%03o' $((65 + i)))")
        names+=("$n")
        $DOCKER run -d --name "${PREFIX}${n}" --hostname "$n" \
            --network "name=$NET,ip=$(node_ip "$n"),\"driver-opt=com.docker.network.endpoint.sysctls=net.ipv4.conf.IFNAME.send_redirects=0,net.ipv4.conf.IFNAME.rp_filter=0\"" \
            --cap-add NET_ADMIN --cap-add NET_RAW \
            --sysctl net.ipv4.ip_forward=1 \
            --sysctl net.ipv4.conf.all.send_redirects=0 \
            --sysctl net.ipv4.conf.all.rp_filter=0 \
            --sysctl net.ipv6.conf.all.disable_ipv6=1 \
            "$IMAGE" >/dev/null
        dxi "$n" nft -f - <<'NFT'
table netdev f01 {
    set blocked { type ether_addr; }
    chain in {
        type filter hook ingress device "eth0" priority 0;
        ether saddr @blocked drop
    }
}
NFT
        # No route out of the mesh: forwarding must come from OLSRd2 routes.
        dx "$n" ip route del default 2>/dev/null || true
    done
    log "up nodes=${names[*]} topology=$(basename "$topo") image=$IMAGE"

    apply_profile "$profile" "${names[@]}"
    while read -r a b; do
        [ -n "${a:-}" ] && [ "${a:0:1}" != "#" ] && link_set add "$a" "$b"
    done < "$topo"
    for n in "${names[@]}"; do
        dx "$n" sh -c 'olsrd2 -l /etc/olsrd2/olsrd2.conf >/var/log/olsrd2.stdout 2>&1 &'
    done
    log "olsrd2 started on ${names[*]}"
}

cmd_down() {
    local n
    for n in $(nodes -a); do $DOCKER rm -f "${PREFIX}${n}" >/dev/null; done
    $DOCKER network rm "$NET" >/dev/null 2>&1 || true
    log "down"
}

cmd_info() {
    local n=$1; shift
    dx "$n" sh -c "printf '%s\nquit\n' '$*' | nc -N -w2 127.0.0.1 2009"
}

cmd_status() {
    local n
    for n in $(nodes); do
        echo "=== $n $(node_ip "$n") $(node_mac "$n")"
        dx "$n" ip route
        dx "$n" tc qdisc show dev eth0 | grep netem || true
        dx "$n" nft list set netdev f01 blocked | grep -o 'elements = {[^}]*}' || echo "blocked: none"
    done
}

usage() {
    cat <<EOF
usage: $0 <command> [args]
  build                              build the node image
  up [count] [topology] [profile]    start nodes A, B, ... (default: 3 chain default)
  down                               remove all nodes and the network
  link-down X Y | link-up X Y        cut or restore the link between X and Y
  profile NAME|FILE [nodes...]       apply waveform profile (default: all nodes)
  info NODE "CMD"                    OLSRd2 telnet command, e.g. "nhdpinfo link"
  exec NODE cmd...                   run a command in a node
  status                             routes, netem and blocked links per node
EOF
}

case "${1:-}" in
    build)     cmd_build ;;
    up)        shift; cmd_up "$@" ;;
    down)      cmd_down ;;
    link-down) link_set add "$2" "$3" ;;
    link-up)   link_set delete "$2" "$3" ;;
    profile)   shift; p=$1; shift; [ $# -gt 0 ] || set -- $(nodes); apply_profile "$p" "$@" ;;
    info)      shift; cmd_info "$@" ;;
    exec)      shift; n=$1; shift; dx "$n" "$@" ;;
    status)    cmd_status ;;
    nodes)     nodes ;;
    ip)        node_ip "$2" ;;
    *)         usage; exit 1 ;;
esac
