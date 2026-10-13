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

pid_suffix=""
num_interfaces=1

usage="Usage: ./cleanup.sh -p <pid of caller> -n <number of interfaces per host (default: 1)>"

while getopts ":p:n:" opt; do
  case "$opt" in
    p)
        pid_suffix="_$OPTARG"
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

if [ "$num_interfaces" -lt 1 ] || [ "$num_interfaces" -gt 64 ]; then
    printf "Number of interfaces must be between 1 and 64, but is: %d\n%s\n" "$num_interfaces" "$usage"
    exit 1
fi

serverns="serverns$pid_suffix"
clientns1="clientns1$pid_suffix"
clientns2="clientns2$pid_suffix"
veth6="veth6$pid_suffix"
expbr0="expbr0$pid_suffix"
expbr1="expbr1$pid_suffix"

# Delete bridges
ip link set "$expbr0" down
ip link set "$expbr1" down
ip link del "$expbr0" type bridge
ip link del "$expbr1" type bridge


# Delete dynamically created interfaces based on num_interfaces
for i in $(seq 1 "$num_interfaces"); do
    # Server bridge interfaces (veth1_i)
    server_veth_br="veth1_${i}$pid_suffix"
    # Client1 bridge interfaces (veth3_i)
    client1_veth_br="veth3_${i}$pid_suffix"
    # Client2 bridge interfaces (veth5_i)
    client2_veth_br="veth5_${i}$pid_suffix"
    # Delete bridge-side interfaces
    ip link del "$server_veth_br" 2>/dev/null || true
    ip link del "$client1_veth_br" 2>/dev/null || true
    ip link del "$client2_veth_br" 2>/dev/null || true
done


# Delete bridge interconnection interface (veth6 <-> veth7 pair)
ip link del "$veth6" 2>/dev/null || true

# Delete network namespaces
ip netns delete "$clientns1" 2>/dev/null || true
ip netns delete "$clientns2" 2>/dev/null || true
ip netns delete "$serverns" 2>/dev/null || true
