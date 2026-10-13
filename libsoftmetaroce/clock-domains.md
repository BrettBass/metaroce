# libsoftmetaroce clock domains

This file records which clock to read for which purpose. The library reads two
clocks with unrelated epochs, and offers one of them at two widths. Read this
before adding a timestamp, a deadline, or a duration.

## The accessors

All three are declared in `ClockSource.h`.

| Accessor | Clock | Width | Use it for |
|---|---|---|---|
| `getNowNs()` | protocol clock | 32-bit, wraps every ~4.29 s | a timestamp that goes on the wire, where the field is 32 bits |
| `getNowNs64()` | protocol clock, same read untruncated | 64-bit | a deadline or an absolute time the protocol acts on |
| `steadyNowNs()` | `std::chrono::steady_clock` | 64-bit | a duration accumulated for diagnostics |

`getNowNs()` is `getNowNs64()` truncated, so the two agree in their low 32
bits. `steadyNowNs()` shares an epoch with neither.

## The protocol clock

`getClockSourceInstance()` picks it from `--libsoftmetaroce_ptp_dev`: empty
gives `ClockSource`, reading `high_resolution_clock` and reporting
`CLOCK_REALTIME`; a device path gives `PTPClockSource`, reading that device.

**A wire timestamp is opaque to the peer.** The sender writes its own reading,
the receiver echoes it unchanged, and the sender subtracts it from a later
reading of its own. A round trip is measured entirely on one host. Both
readings must come from the configured source, which runs at its own rate under
a PTP device or an emulated environment.

**It steps.** `PTPClockSource` declares `is_steady = false`, and
`CLOCK_REALTIME` moves when the system clock is adjusted, and every absolute
`getNowNs64()` deadline moves with it.

## Rules

**Keep `steadyNowNs()` values apart from `getNowNs()` and `getNowNs64()`
values.** The epochs are unrelated and the types are both integers, so a
subtraction across them compiles and means nothing. `steadyNowNs()` exists to
accumulate over a run, which the protocol clock cannot do: its 32-bit reading
wraps every ~4.29 s, and its full reading can step.

**Read the time through `getNowNs64()`.** `getClockSourceInstance()`
constructs a new object per call; each holds a descriptor on the PTP device,
where one is configured, for as long as that object lives, which for a
function-local static is until process teardown. `TimerFD` owns an instance
because it needs the clock id for `timerfd_create` and the instance itself to
busy-poll a PTP clock.

**Measure spans past roughly four seconds at full width.** Unsigned subtraction
of two 32-bit readings recovers a forward interval shorter than about 4.29 s,
including one that crosses zero. Holding one as an absolute time does not
survive a wrap, and zero recurs as a real reading every wrap.

## Deadlines and wakes

A deadline the protocol acts on is an absolute `getNowNs64()` reading. The
watchdog is the exception: `WatchdogTimer` measures elapsed progress on
`std::chrono::steady_clock`, so a clock step leaves it in place. The timer that
wakes a thread to service a deadline is armed from a different basis.

Without a PTP device, `TimerFD` calls `timerfd_settime` with a flags argument
of zero, arming a relative interval that a clock step leaves in place while the
deadline moves. With one, `timerfd_create` cannot take the clock id, so
`TimerFD` computes an absolute target from the clock source and busy-polls
until it passes; that target moves with the deadline.

Re-read the clock on wake. A wake reports that work may be due.

## One assumption

`GetClockId()` returns `CLOCK_REALTIME` while `now()` reads
`high_resolution_clock`, so `high_resolution_clock` must be the `CLOCK_REALTIME`
clock. The C++ standard leaves that alias to the implementation; libstdc++
satisfies it by defining `high_resolution_clock` as the system clock. Two
callers rely on that: `timerfd_create` is armed on the id, and `isPtpClock()`
tests it to choose the timer mechanism. A source whose `now()` disagreed with
its id would arm on one clock and measure on another.
