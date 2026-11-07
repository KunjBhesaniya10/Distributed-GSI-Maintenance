#!/usr/bin/env bash
# Add network delay between the containers (D5.2), so a round trip costs
# what it would between machines.  Every container delays its outgoing
# packets by RTT/2, so coordinator <-> shard round trips take RTT.
#
#   bash bench/net_delay.sh 1      # 1 ms round trip (same region, different zones)
#   bash bench/net_delay.sh 0      # remove the delay
#
# Needs cap_add NET_ADMIN and iproute2 in the image (docker-compose.yml, Dockerfile).
set -e
RTT=${1:?usage: net_delay.sh <round trip ms>}
HALF=$(awk "BEGIN { print $RTT / 2 }")
cd "$(dirname "$0")/../docker"
for c in coordinator shard1 shard2 shard3 shard4 shard5; do
    if [ "$RTT" = 0 ]; then
        docker compose exec -T -u root "$c" tc qdisc del dev eth0 root 2>/dev/null || true
    else
        docker compose exec -T -u root "$c" tc qdisc replace dev eth0 root netem delay "${HALF}ms"
    fi
done
echo "round trip delay between containers: ${RTT} ms"
