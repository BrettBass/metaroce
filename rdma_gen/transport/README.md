# Transport

This directory defines how rdma_gen accesses an RDMA backend:

- `ITransportProvider` defines devices, contexts, peers, and registered memory.
- `IRdmaVerbs` and `RdmaVerbs` provide the injectable verbs interface and its
  rdma-core implementation.
- `VerbsProvider` implements the transport objects using that interface.
- `GetTransportProvider` and `RdmaGenVerbsMode` select the backend.

Device-specific connection steps plug in through `VerbsPeerPolicy`, which the
provider runs around each peer's connection, rather than living in
`VerbsProvider`. Buffer allocation and memory implementations belong in
`memory/`; request generation and completion processing belong to the
execution code.

New sources here must also be added to the source list in the top-level
`CMakeLists.txt`.
