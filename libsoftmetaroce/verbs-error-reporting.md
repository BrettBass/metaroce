# libsoftmetaroce verbs error reporting

This file records what an `ibv_*` verb returns when it refuses a call, and when
lsmr also logs the refusal. Read this before adding or changing a verb's
failure path.

## Return values

The `ibv_*` functions follow libibverbs conventions:
- Functions that return pointers should return `nullptr` on error
- Null-returning entry points must set `errno` before returning (`EINVAL` for
  invalid arguments, `ENOSYS` for unsupported verbs)
- A verb that checks membership returns `ENOENT` for an object lsmr does not
  hold that the verb acts on, and `EINVAL` for one that is only the destination
  of its work.
- Memory the process does not hold produces `EFAULT`, a failed allocation
  `ENOMEM`, and a spent range of handles `ENOSPC`.
- A verb that returns `int` returns on error what libibverbs specifies for it:
  most return a positive errno value, while `ibv_poll_cq`,
  `ibv_get_async_event`, `ibv_query_gid` and `ibv_close_device` return -1 with
  `errno` set. `ibv_poll_cq` returns the number of completions on success, and
  the others return 0.
- `ibv_poll_cq` checks for a null queue or array and a negative count; it does
  not check that a non-null queue is live, so polling requires one.

## Logging

- **Do not log** a caller's own mistake; the return code reports it. Including:
  a null or fabricated handle, an object lsmr does not hold, a value past an
  advertised limit, an attribute set in a state that forbids it.
- **Do log** a verbs operation libibverbs permits but lsmr does not implement,
  such as a transition to the SQD state, an unsupported opcode, or an empty
  scatter list where the opcode needs data. Exceeding a limit lsmr advertises is
  not such an operation: `max_sge` says what it takes, where nothing advertises
  that lsmr lacks the SQD state.

## Quick reference

| Situation                        | Mechanism                 |
|----------------------------------|---------------------------|
| Caller's own mistake             | Error code, no log        |
| Verb's own object not held       | `ENOENT`, no log          |
| Destination object not held      | `EINVAL`, no log          |
| Memory the process does not hold | `EFAULT`, no log          |
| Allocation failed                | `ENOMEM`, no log          |
| Handle range exhausted           | `ENOSPC` + log            |
| libibverbs permits, lsmr doesn't | Error code + `LOG(ERROR)` |
