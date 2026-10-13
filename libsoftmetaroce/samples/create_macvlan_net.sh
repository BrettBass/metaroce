#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Creates a macvlan-based multiport test topology for libsoftmetaroce.
#
# Each namespace gets a single veth connected to a bridge for cross-namespace
# connectivity. Inside the namespace, macvlan children in bridge mode are
# stacked on the veth to simulate per-port PF addresses. The parent veth
# carries the VF address used for GID exchange.
#
#                                   macvlan p0 (fc00::<pid>:10)
# +--------+  veth0 (fc00::<pid>:1) macvlan p1 (fc00::<pid>:11)  veth1
# |serverns|.....................................................+-------+
# +--------+                                                      |expbr0 |
#                                                                  +--+----+
#                                                                     |
#                                                                  +--+----+
#                                    macvlan p0 (fc00::<pid>:110)  |expbr1 |
# +----------+  veth2 (fc00::<pid>:101) macvlan p1 ...  veth3     +-------+
# |clientns1 |..................................................
# +----------+
#
# Ports can be selectively brought down with -d <port_index> to test
# the Active Port Set reporting with some ports in a failed state.

usage="Usage: ./create_macvlan_net.sh -m <mtu> -p <pid> -s <server addr> -n <num_ports> [-d <down_port_index>]"

set -x
set -e

mtu=4200
server_addr="fc00::1"
pid_suffix=""
pid=0
num_ports=2
down_ports=()

while getopts ":m:p:s:n:d:" opt; do
  case "$opt" in
    m) mtu="$OPTARG" ;;
    s) server_addr="$OPTARG" ;;
    p) pid_suffix="_$OPTARG"; pid="$OPTARG" ;;
    n) num_ports="$OPTARG" ;;
    d) down_ports+=("$OPTARG") ;;
    \?) printf "Error: Invalid option: -%s\n%s\n" "$OPTARG" "$usage"; exit 1 ;;
  esac
  case "$OPTARG" in
    -*) printf "Error: Option '%s' needs a valid argument!\n%s\n" "$opt" "$usage"; exit 1 ;;
  esac
done

if [ "$num_ports" -lt 1 ] || [ "$num_ports" -gt 16 ]; then
    printf "Number of ports must be between 1 and 16, but is: %d\n%s\n" "$num_ports" "$usage"
    exit 1
fi

serverns="serverns$pid_suffix"
clientns1="clientns1$pid_suffix"
clientns2="clientns2$pid_suffix"
veth6="veth6$pid_suffix"
veth7="veth7$pid_suffix"
expbr0="expbr0$pid_suffix"
expbr1="expbr1$pid_suffix"

server_veth_ns="veth0_1$pid_suffix"
server_veth_br="veth1_1$pid_suffix"
client1_veth_ns="veth2_1$pid_suffix"
client1_veth_br="veth3_1$pid_suffix"
client2_veth_ns="veth4_1$pid_suffix"
client2_veth_br="veth5_1$pid_suffix"

ip netns add "$serverns"
ip netns add "$clientns1"
ip netns add "$clientns2"

for ns in "$serverns" "$clientns1" "$clientns2"; do
    ip netns exec "$ns" sysctl -w net.ipv6.conf.all.accept_dad=0 > /dev/null
    ip netns exec "$ns" sysctl -w net.ipv6.conf.default.accept_dad=0 > /dev/null
    ip netns exec "$ns" sysctl -w net.ipv6.conf.all.keep_addr_on_down=1 > /dev/null
done

ip link add "$server_veth_ns" type veth peer "$server_veth_br"
ip link add "$client1_veth_ns" type veth peer "$client1_veth_br"
ip link add "$client2_veth_ns" type veth peer "$client2_veth_br"

ip link set "$server_veth_ns" netns "$serverns"
ip link set "$client1_veth_ns" netns "$clientns1"
ip link set "$client2_veth_ns" netns "$clientns2"

ip link add "$veth6" type veth peer "$veth7"
ip link add name "$expbr0" type bridge
ip link add name "$expbr1" type bridge

ip link set "$server_veth_br" master "$expbr0"
ip link set "$client1_veth_br" master "$expbr1"
ip link set "$client2_veth_br" master "$expbr1"
ip link set "$veth6" master "$expbr0"
ip link set "$veth7" master "$expbr1"

for iface in "$server_veth_br" "$client1_veth_br" "$client2_veth_br" "$veth6" "$veth7" "$expbr0" "$expbr1"; do
    ip link set "$iface" mtu "$mtu" up
done

# Helper: set up macvlan ports inside a namespace.
# Args: namespace, veth_name, vf_address, port_addr_base_hex
setup_macvlan_ports() {
    local ns="$1"
    local veth="$2"
    local vf_addr="$3"
    local port_base="$4"

    ip netns exec "$ns" ip link set "$veth" mtu "$mtu" up
    ip netns exec "$ns" ip addr add "$vf_addr/64" dev "$veth"

    for i in $(seq 0 $((num_ports - 1))); do
        local port_name="${veth}p${i}"
        local port_addr
        port_addr=$(printf "fc00::%x:%x" "$pid" "$((port_base + i))")

        ip netns exec "$ns" ip link add link "$veth" name "$port_name" type macvlan mode bridge
        ip netns exec "$ns" ip addr add "$port_addr/64" dev "$port_name"
        ip netns exec "$ns" ip link set "$port_name" mtu "$mtu" up
    done

    ip netns exec "$ns" ip link set lo up
}

# Server: VF addr = test_server_address(), port addrs at 0x10+i
setup_macvlan_ports "$serverns" "$server_veth_ns" "$server_addr" "0x10"

# Client1: VF addr at 0x101, port addrs at 0x110+i
client1_vf=$(printf "fc00::%x:%x" "$pid" "0x101")
setup_macvlan_ports "$clientns1" "$client1_veth_ns" "$client1_vf" "0x110"

# Client2: VF addr at 0x201, port addrs at 0x210+i
client2_vf=$(printf "fc00::%x:%x" "$pid" "0x201")
setup_macvlan_ports "$clientns2" "$client2_veth_ns" "$client2_vf" "0x210"

# Bring down specified ports (on all namespaces).
for dp in "${down_ports[@]}"; do
    if [ "$dp" -ge 0 ] && [ "$dp" -lt "$num_ports" ]; then
        ip netns exec "$serverns" ip link set "${server_veth_ns}p${dp}" down
        ip netns exec "$clientns1" ip link set "${client1_veth_ns}p${dp}" down
        ip netns exec "$clientns2" ip link set "${client2_veth_ns}p${dp}" down
    fi
done
