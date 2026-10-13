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


#                                                                           +----------+
#                                                                     veth2 |          |
#                                                     +---------------------+clientns1 |
#                                                     |                     +----------+
#                                                     |veth3
# +--------+             +-------+              +-----+--+
# |        |veth0   veth1|       |veth6    veth7|        |
# |serverns+-------------+expbr0 +--------------+expbr1  |
# +--------+             +-------+              +-----+--+
#                                                     |veth5                +----------+
#                                                     |               veth4 |          |
#                                                     +---------------------+clientns2 |
#                                                                           +----------+

usage="Usage: ./create_net.sh -m <mtu bytes (default: 4200)> -p <pid of caller> -s <server address> -n <number of interfaces per host (default: 1)>"

set -x
set -e

mtu=4200
server_addr="fc00::1"
pid_suffix=""
pid=0
num_interfaces=1

while getopts ":m:p:s:n:" opt; do
  case "$opt" in
    m) mtu="$OPTARG"
    ;;
    s) server_addr="$OPTARG"
    ;;
    p)
        pid_suffix="_$OPTARG"
        pid="$OPTARG"
    ;;
    n) num_interfaces="$OPTARG"
    ;;
    \?) printf "Error: Invalid option: -%s\n%s\n" "$OPTARG" "$usage"
    exit 1
    ;;
  esac

  case "$OPTARG" in
    -*) printf "Error: Option '%s' needs a valid argument!\n%s\n" "$opt" "$usage"
    exit 1
    ;;
  esac
done

if [ "$mtu" -lt 1500 ]; then
    printf "MTU must be at least 1500 bytes, but is: %d\n%s\n" "$mtu" "$usage"
    exit 1
fi

if [ "$num_interfaces" -lt 1 ] || [ "$num_interfaces" -gt 64 ]; then
    printf "Number of interfaces must be between 1 and 64, but is: %d\n%s\n" "$num_interfaces" "$usage"
    exit 1
fi

serverns="serverns$pid_suffix"
clientns1="clientns1$pid_suffix"
clientns2="clientns2$pid_suffix"
veth6="veth6$pid_suffix"
veth7="veth7$pid_suffix"
expbr0="expbr0$pid_suffix"
expbr1="expbr1$pid_suffix"

# Create all namespaces
ip netns add "$serverns"
ip netns add "$clientns1"
ip netns add "$clientns2"

# Disable IPv6 DAD (Duplicate Address Detection) to avoid tentative address delays.
# DAD puts addresses in a "tentative" state after assignment, during which bind()
# fails with EADDRNOTAVAIL. Disabling DAD makes addresses usable immediately.
for ns in "$serverns" "$clientns1" "$clientns2"; do
    ip netns exec "$ns" sysctl -w net.ipv6.conf.all.accept_dad=0 > /dev/null
    ip netns exec "$ns" sysctl -w net.ipv6.conf.default.accept_dad=0 > /dev/null
done

# Multi-interface mode
# Arrays to store interface names
declare -a server_veths_ns
declare -a server_veths_br
declare -a client1_veths_ns
declare -a client1_veths_br
declare -a client2_veths_ns
declare -a client2_veths_br
declare -a all_bridge_ifaces
# Create veth pairs dynamically based on num_interfaces
for i in $(seq 1 "$num_interfaces"); do
    # Server interfaces (veth0_i <-> veth1_i)
    server_veth_ns="veth0_${i}$pid_suffix"
    server_veth_br="veth1_${i}$pid_suffix"
    server_veths_ns+=("$server_veth_ns")
    server_veths_br+=("$server_veth_br")
    # Client1 interfaces (veth2_i <-> veth3_i)
    client1_veth_ns="veth2_${i}$pid_suffix"
    client1_veth_br="veth3_${i}$pid_suffix"
    client1_veths_ns+=("$client1_veth_ns")
    client1_veths_br+=("$client1_veth_br")
    # Client2 interfaces (veth4_i <-> veth5_i)
    client2_veth_ns="veth4_${i}$pid_suffix"
    client2_veth_br="veth5_${i}$pid_suffix"
    client2_veths_ns+=("$client2_veth_ns")
    client2_veths_br+=("$client2_veth_br")
    # Create the veth pairs
    ip link add "$server_veth_ns" type veth peer "$server_veth_br"
    ip link add "$client1_veth_ns" type veth peer "$client1_veth_br"
    ip link add "$client2_veth_ns" type veth peer "$client2_veth_br"
    # Move namespace-side interfaces to their respective namespaces
    ip link set "$server_veth_ns" netns "$serverns"
    ip link set "$client1_veth_ns" netns "$clientns1"
    ip link set "$client2_veth_ns" netns "$clientns2"
    # Add bridge-side interfaces to arrays for later processing
    all_bridge_ifaces+=("$server_veth_br" "$client1_veth_br" "$client2_veth_br")
done
# Create bridge interconnection veth pair
ip link add "$veth6" type veth peer "$veth7"
all_bridge_ifaces+=("$veth6" "$veth7")
# Create topology bridges
ip link add name "$expbr0" type bridge
ip link add name "$expbr1" type bridge
# Add server interfaces to expbr0
for server_br_iface in "${server_veths_br[@]}"; do
    ip link set "$server_br_iface" master "$expbr0"
done
# Add client interfaces to expbr1
for client1_br_iface in "${client1_veths_br[@]}"; do
    ip link set "$client1_br_iface" master "$expbr1"
done
for client2_br_iface in "${client2_veths_br[@]}"; do
    ip link set "$client2_br_iface" master "$expbr1"
done
# Connect the bridges
ip link set "$veth6" master "$expbr0"
ip link set "$veth7" master "$expbr1"
# Bring up all bridge interfaces and bridges
for iface in "${all_bridge_ifaces[@]}" "$expbr0" "$expbr1"; do
    ip link set "$iface" mtu "$mtu" up
done
# Configure server namespace interfaces
for i in $(seq 1 "$num_interfaces"); do
    server_veth_ns="${server_veths_ns[$((i-1))]}"
    if [ "$i" -eq 1 ]; then
        # Use the provided server_addr for the first interface
        server_addr_current="$server_addr"
    else
        # Generate subsequent addresses based on pid and interface index
        server_addr_current=$(printf "fc00::%x:%x" "$pid" "$i")
    fi
    ip netns exec "$serverns" ip addr add "$server_addr_current/64" dev "$server_veth_ns"
    ip netns exec "$serverns" ip link set "$server_veth_ns" mtu "$mtu" up
done
ip netns exec "$serverns" ip link set lo up
# Configure client1 namespace interfaces
for i in $(seq 1 "$num_interfaces"); do
    client1_veth_ns="${client1_veths_ns[$((i-1))]}"
    client1_addr_hex=$(printf "fc00::%x:%x" "$pid" "$((0x100 + i))")
    ip netns exec "$clientns1" ip addr add "$client1_addr_hex/64" dev "$client1_veth_ns"
    ip netns exec "$clientns1" ip link set "$client1_veth_ns" mtu "$mtu" up
done
ip netns exec "$clientns1" ip link set lo up
# Configure client2 namespace interfaces
for i in $(seq 1 "$num_interfaces"); do
    client2_veth_ns="${client2_veths_ns[$((i-1))]}"
    client2_addr_hex=$(printf "fc00::%x:%x" "$pid" "$((0x200 + i))")
    ip netns exec "$clientns2" ip addr add "$client2_addr_hex/64" dev "$client2_veth_ns"
    ip netns exec "$clientns2" ip link set "$client2_veth_ns" mtu "$mtu" up
done
ip netns exec "$clientns2" ip link set lo up

# Give time for interfaces to come up.
sleep 2
