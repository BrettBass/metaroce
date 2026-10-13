# Running rdma-unit-test against libsoftmetaroce

Runs the third-party verbs-conformance suite `rdma-unit-test` with
`libsoftmetaroce` (lsmr) as the RDMA backend, so the software MetaRoCE emulation
is exercised by the same API-level gtests that hardware and Soft-RoCE (`rxe`)
run. This complements the pcap/protocol verifiers by validating the
`MetaRoceVerbs` verbs surface and the RC datapath.

## The gap this bridges

`rdma-unit-test` (https://github.com/google/rdma-unit-test) is a generic verbs
framework: it calls the **global libibverbs C API** and expects a **real device**
enumerated by `ibv_get_device_list()` and opened by `ibv_open_device()`
(upstream `public/verbs_util.cc`). Suite `public/...` and `unit/...` paths below are
relative to that tree.

`libsoftmetaroce` is an in-process **C++ vtable** — `MetaRoceVerbs : RdmaVerbs :
IRdmaVerbs`, whose methods are *named* `ibv_*` but are virtual members on an
object, not the global `::ibv_*` C symbols. It is not an rdma-core provider and
exposes no kernel uverbs device; callers construct a
`MetaRoceVerbs` and route calls through an `IRdmaVerbs*`. So the real-device
model the suite expects does
not apply, and a bridge must map the suite's global `::ibv_*` calls onto a
`MetaRoceVerbs` instance.

## How the bridge works

A link-time `--wrap` interposition shim (`lsmr_ibv_shim.cpp`) linked into a
purpose-built gtest binary (`lsmr_run_unit_test`) maps the global libibverbs
symbols the suite calls onto a process-wide `MetaRoceVerbs` singleton.

The CMake build fetches `google/rdma-unit-test` at the revision specified in
`CMakeLists.txt`. `patches/series` adjusts completion waits and retry count in
upstream `unit/qp_test.cc`.

The interposition surface splits three ways (the libibverbs headers used
by the build):

1. **Real exported symbols — directly `--wrap`-able.** `ibv_open_device`,
   `ibv_close_device`, `ibv_query_device`, `ibv_query_gid`, `ibv_alloc_pd`,
   `ibv_dealloc_pd`, `ibv_create_cq`, `ibv_destroy_cq`,
   `ibv_create_comp_channel`, `ibv_destroy_comp_channel`, `ibv_create_qp`,
   `ibv_destroy_qp`, `ibv_modify_qp`, `ibv_query_qp`, `ibv_dereg_mr`,
   `ibv_get_device_list`, `ibv_free_device_list`. Each `__wrap_*` forwards to the
   matching `MetaRoceVerbs` method.

2. **Macro over real symbol(s) — wrap the underlying symbols.** `ibv_reg_mr` is a
   macro expanding (via `__builtin_constant_p`) to either `ibv_reg_mr` or
   `ibv_reg_mr_iova2`. Both (and `ibv_reg_mr_iova`) are wrapped and routed to
   `MetaRoceVerbs::ibv_reg_mr_internal`.

3. **`static inline` datapath — not `--wrap`-able.** `ibv_post_send`,
   `ibv_post_recv`, `ibv_poll_cq`, `ibv_req_notify_cq` dispatch through
   `qp->context->ops.*` / `cq->context->ops.*`; there is no symbol to wrap. The
   shim instead populates `context->ops.{post_send,post_recv,poll_cq,
   req_notify_cq}` with C trampolines into the singleton, and points every
   `qp->context` / `cq->context` at that ops-populated context.

`ibv_query_port` is a macro whose fast path dispatches through
`verbs_context::query_port` (unwrappable), falling back to the real
`ibv_query_port` symbol only when `context->abi_compat != __VERBS_ABI_IS_EXTENDED`.
The shim clears `abi_compat` on the context it returns from
`__wrap_ibv_open_device`, forcing the wrappable legacy path.

**Entry point.** `lsmr_gunit_main.cc` is the test binary's entry point (the
build uses it instead of the suite's `unit/gunit_main.cc`):
it registers `IntrospectionLsmr` (below), calls
`folly::SingletonVault::registrationComplete()` — the suite initializes absl, not
folly, and `MetaRoceVerbs` uses folly singletons — and `_exit`s with the gtest
result, because the emulation's folly thread pools deadlock in the folly
singleton atexit teardown.

**Capabilities.** The suite selects a `NicIntrospection` by device-name prefix to
learn a provider's capabilities and per-testcase deviations (so unsupported cases
skip instead of failing). `IntrospectionLsmr` (`introspection_lsmr.h`, keyed on
lsmr's own `lsmr` device prefix) reports the MetaRoCE profile lsmr emulates
plus lsmr's own deviation map (the MetaRoCE-protocol-level gaps). It is
registered directly, so no device renaming or dependence on a hardware
introspection is needed.

## Build requirements

Compile the suite sources and shim into the executable so `--wrap` can
intercept the suite's verbs calls. The suite, shim, and libsoftmetaroce must
use the same libibverbs headers and library.

To build from local checkouts instead of downloading, pass
`-DFETCHCONTENT_SOURCE_DIR_ABSL=<dir>`,
`-DFETCHCONTENT_SOURCE_DIR_MAGIC_ENUM=<dir>`, and
`-DFETCHCONTENT_SOURCE_DIR_RDMA_UNIT_TEST=<dir>`.

## Run model

rdma-unit-test is mostly **single-process loopback**: two QPs, often on one
`ibv_context`, connected to each other, posting on one and polling the other.
lsmr moves packets over a real UDP socket, so loopback means one `MetaRoceVerbs`
instance hosting both QPs and talking to itself over a local non-loopback IP address (GID discovery
requires one at startup; no network namespace required).

## Design rationale

- **Subclassing `VerbsExtension` / `VerbsHelperSuite`** (the suite's designed
  seam) is insufficient: `VerbsExtension` only indirects MR/AH/QP-create/
  init-to-RTR, while PD, CQ, SRQ, destroy, and the whole datapath bypass it and
  hit global libibverbs / `context->ops`. Redirecting everything requires
  interposition anyway. (`NicIntrospection` is a separate seam, and is used.)
- **Forking the suite** (patching its call sites to renamed `ibv_*` entry points)
  works but adds a permanent rebase burden against Google upstream.
- **A real rdma-core provider `.so` / kernel uverbs shim for lsmr** is the highest
  fidelity (unmodified tools "just work") but a large, ongoing-maintenance lift,
  unjustified for test tooling.

## Coverage

`IntrospectionLsmr` describes capabilities and known test deviations; see
`introspection_lsmr.h`. The test filter selects cases known to pass against
lsmr. Expand the filter when library fixes enable additional tests.

## Files

- `lsmr_ibv_shim.cpp` — the `--wrap` interposition shim (control-path wrappers
  and datapath ops-table trampolines).
- `introspection_lsmr.h` — `IntrospectionLsmr`: lsmr's MetaRoCE capabilities and
  deviation map.
- `lsmr_gunit_main.cc` — the suite entry point.
- `CMakeLists.txt` — the standalone OSS build: fetches Google upstream,
  applies the local compatibility patch, then builds the binary with the test filter.
- `check_green_names.py` — rejects filter entries with missing test definitions.
