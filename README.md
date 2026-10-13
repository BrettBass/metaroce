# MetaRoCE

MetaRoCE is an RDMA transport protocol designed for hardware-offloaded
operation over best-effort Ethernet networks with shallow-buffer
switches. It targets large-scale AI training clusters, where thousands
of accelerators exchange data across networks spanning racks, buildings,
and sometimes geographic locations, scaling to hundreds of thousands of
nodes without compromising tail latency. The authoritative description
is the specification in [spec/](spec/).

## Key ideas

- **Lossy fabric, no PFC required.** The transport treats the network as
  lossy and recovers through selective acknowledgment and
  retransmission. The primary expectation from the fabric is ECN
  marking; switch spraying, trimming, telemetry, and link-level credits
  are all optional or unused.
- **Tags.** A single connection (QP) carries multiple independent
  message streams, each with its own sequence space and completion
  ordering, so a stall on one stream does not block the others.
- **Explicit multipath.** The sender maintains multiple flows per
  connection, each with its own window and SACK state, steering packets
  across ECMP routes via per-packet entropy values. A silent flow is
  distinguishable from a silent connection, so failed flows shed load
  onto survivors.
- **Relaxed-order data delivery.** Packets carry their message offset
  (writes also carry address, length, and rkey), so data lands in its
  final location as it arrives and completions are delivered in order —
  no large on-NIC reorder buffers.
- **Congestion control with receiver-driven rate limiting.** Baseline
  AIMD over per-flow windows, plus receiver-communicated target rates
  for fast incast convergence, with programmable parameters.
- **Operations.** Send/Recv (with and without immediate, with and
  without tags), RDMA Write and Read (with and without immediate), and
  atomics (compare-and-swap, fetch-and-add).

## Repository layout

| Path | Contents |
|---|---|
| [spec/](spec/) | The MetaRoCE protocol specification |
| [libsoftmetaroce/](libsoftmetaroce/) | Software MetaRoCE transport with a verbs provider |
| [rdma_gen/](rdma_gen/) | RDMA traffic generator; sample driver for the transport |
| [wireshark/](wireshark/) | Lua dissector for MetaRoCE packets |
| [build/](build/) | `getdeps`-based build support |
| `CMakeLists.txt`, `build.sh` | Top-level CMake build |

## Build

The main build script is [build/fbcode_builder/getdeps.py](build/fbcode_builder/getdeps.py),
and the exact build command is in [build.sh](build.sh).

> [NOTE] Make sure that your default compiler is gcc, not clang,
> otherwise folly/coro (required) will not be built.

## License

MetaRoCE is Apache-2.0 licensed; see [LICENSE](LICENSE). Contributions
are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).
