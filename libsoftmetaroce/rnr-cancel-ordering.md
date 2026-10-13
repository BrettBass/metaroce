# How the initiator reads a BRNR against an RNR-Cancel

A responder with no receive posted refuses a message with BRNR, and sends an
RNR-Cancel once one is posted. The two can reach the initiator in either order,
so the initiator holds what a cancel reports in a state rather than acting on
its arrival.

## Why the cancel can arrive first

- **The refusal is recorded before it is sent.** The responder records the
  RNR-Cancel request at the receive miss and stages the BRNR acknowledgment
  afterward, so a receive posted in between sends a cancel for an
  acknowledgment not yet sent.
- **The two are separate packets**, so the network may deliver them in either
  order.

Were a cancel only to lift a pause already in place, as `BrnrBackoff::resume()`
does, one arriving first would find no pause, the BRNR behind it would pause the
tag, and no second cancel would follow. The initiator would wait out the whole
backoff for a receive the responder had already posted.

## What the cancel reports

`Spec 4.10.1` defines Posted CSN as the highest csn with a receive WQE posted on
that tag, "so the initiator learns how many receive-WQE-consuming messages it
may send before risking another RNR". It is a level: it stays true until the
receives behind it are taken.

A cancel moves the tag to `BRNR_CANCELED`, recording the csn it reports
(`canceledThroughCsn()`) and when it arrived (`canceledAtNs_`). It does so even
when the tag holds no transmission. `Spec 4.10.1` says an initiator drops such a
cancel; lsmr keeps it, so a BRNR that arrives after it is still covered.

## Reading a BRNR against it

`TxFlow::processAckPacket` reads each unresolved frame a BRNR reports against
that state; a frame of a failing or already-served work request is abandoned
first and never reaches the comparison:

| frame | effect |
| --- | --- |
| at or before the posted csn | covered: refused before that receive was posted, so it does not pause the tag |
| past the posted csn | pauses the tag at its msn and arms the backoff the BRNR carries |
| with no csn | pauses the tag; the posted csn says nothing about it |
| any, arriving more than one estimated QP round trip after the cancel | pauses the tag; the initiator takes that estimate (`getEstQpRttNs()`) as how long a covered refusal can still be in flight |

## Repeated and stale cancels

Cancels are unsequenced among themselves. While a cancel csn is held, one
reporting a csn behind it is stale and dropped, so a reordered cancel cannot
lift a pause raised since. `resume()` clears the held csn, after which a cancel
is not compared with the old one.

One repeating the level is not stale. A receive is not held for the tag resumed
at it, so a tag drawing from a shared receive queue can lose that receive to
another tag and be refused again at the same csn. The repeat reports the second
receive posted for it, so it takes the level and the time and lifts any pause.

## Invariants

1. A tag in `BRNR_CANCELED` has no blocked msn: `cancelThrough` clears
   `firstBlockedMsn()`, and nothing sets one without leaving that state.
   `Tag::findNextWqeInMap` calls `LOG(FATAL)` on a state it does not expect over
   a blocked msn.
2. A pause takes the state but leaves the csn. `blockAt()` keeps
   `canceledThroughCsn()`, so a covered refusal arriving after an uncovered one
   cannot drag the pause back over a message the responder can take. Only
   `resume()` clears it, and `cancelCovers` reads it against the round trip
   rather than the state.
3. An acknowledgment does not retire a canceled tag's csn: `TxFlow::isBlockedMsn`
   requires a `firstBlockedMsn()`, which a cancel clears.
4. A frame with no csn is never covered.
5. The cancel fields are read and written only under that tag's lock;
   `locking-discipline.md` has the order.
6. Csn and clock comparisons wrap. Use `before`/`beforeOrEqual` from
   `WrappingCompare.h`; the 32-bit clock wraps every few seconds, so compare
   instants against a bound rather than each other.

## Related

- `locking-discipline.md` -- the tag lock this state sits under.
- `tag-failure-lifecycle.md` -- the other cross-thread path through a Tag.
- `clock-domains.md` -- which clock reading `canceledAtNs_` holds.
