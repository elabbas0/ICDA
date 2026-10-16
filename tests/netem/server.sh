#!/bin/sh
# inside icda-nettest: bridge + DHCP + HTTP server; $NETEM = netem arguments for server->guest
gcc -O2 -o /tmp/bridge /work/bridge.c || exit 1
/tmp/bridge & 
sleep 1
ip addr add 10.9.0.1/24 dev tap0 && ip link set tap0 up
[ -n "$NETEM" ] && tc qdisc add dev tap0 root netem $NETEM && tc qdisc show dev tap0
dnsmasq --no-daemon --port=0 --interface=tap0 --dhcp-range=10.9.0.50,10.9.0.60,1h --dhcp-option=3,10.9.0.1 --dhcp-option=6,10.9.0.1 --log-dhcp > /tmp/dhcp.log 2>&1 &
sysctl net.ipv4.tcp_sack net.ipv4.tcp_congestion_control
( while :; do date +%s >> /tmp/ss.log; ss -tin "( sport = :8099 )" >> /work/ss.log; sleep 1; done ) &
exec python3 -u /work/serve.py
