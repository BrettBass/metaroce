# lsmr_preload: run unmodified verbs binaries on libsoftmetaroce

`liblsmr_preload.so` presents libsoftmetaroce as the verbs implementation
to binaries that were built against ordinary libibverbs. The loader binds
the `ibv_*` symbols below to this library instead of libibverbs; each
forwards to a process-wide `MetaRoceVerbs` instance. Datapath verbs are
static inline in `verbs.h` and dispatch via `context->ops`, so `open_device`
installs trampolines in the ops table. The technique matches the rdma-unit
link-time shim, in shared-library form.

## Usage

```sh
LSMR_PRELOAD_GIDS=fc00::1 LD_PRELOAD=/path/to/liblsmr_preload.so ib_write_bw ...
```

`LSMR_PRELOAD_GIDS` is optional: a comma-separated list of local IP
addresses to use as GIDs (the `--libsoftmetaroce_gid` value). When unset,
addresses are auto-discovered from local interfaces. Diagnostics go to
stderr. The emulated device is named `lsmr0`, so `-d lsmr0` selects it
where a binary lists more than one device.

## Coverage

The interposed set is the `ib_write_bw` RC flow: device list, open/close,
device/port/gid queries, PD, CQ, MR (all three `reg_mr` spellings), QP
create/modify/query/destroy, and the datapath ops. Completion channels are unsupported; use polling.
Datapath dispatch runs through trampolines that open_device installs, and
the extended query, null-MR, and device-memory verbs resolve through those
trampolines; a verb with no backend operation fails cleanly at its header
inline.
