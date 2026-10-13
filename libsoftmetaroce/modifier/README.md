# Libsoftmetaroce Modifier User Guide

## Introduction and Overview

Libsoftmetaroce (LSMR) is a user-space implementation of the MetaRoCE transport
protocol. It provides a software stack that can run without hardware NICs and is
mainly used for debugging, validation, and trying out protocol changes before
they go into real hardware.

The modifier framework is a built-in part of LSMR. Modifiers sit directly on the
packet path inside LSMR, so every TX/RX packet goes through them. This lets you
drop, reorder or do other packet-level changes without touching the core code,
which is useful for building test scenarios or reproducing corner cases.

This guide explains how to use modifiers when running LSMR, build test scenarios
or verify behavior while LSMR is talking to another MetaRoCE implementation.

The modifier framework runs in continuous testing, both between two LSMR
endpoints and between LSMR and hardware MetaRoCE implementations, so that the
same scenarios validate behavior across every environment.

## Currently Supported Modifiers

- **Drop**: Discards packets that match the filter
- **Reorder**: Delays packets to change their order
- **Duplicate**: Creates copies of packets with a delay
- **Corrupt**: Corrupts specific byte positions in packets
- **WriteEdit**: Modifies write/read operation headers (rkey, address)
- **HeaderEdit**: Modifies specific header fields (base, data, ack headers)
- **MarkEcn**: Marks packets with ECN (Explicit Congestion Notification)

LSMR also support multiple modifiers to run at the same time:

- one modifier at TX side and another on the RX side
- or multiple modifiers on a single side

## Using Modifier Config

When running LSMR, modifiers are enabled through a JSON config passed via the
`libsoftmetaroce_modifier_config` parameter. The former `modifier_config`
spelling remains available during migration.

Users can specify which modifiers to load, their filters, and their parameters
in this JSON file, and LSMR will apply them automatically on the TX/RX path
during runtime.

**Example:**

```bash
# Build rdma_gen binary
rdma_gen_bin=$(buck2 build //neteng/ai/rdma_gen:rdma_gen --show-output | awk '{print $2}')

# Server side
$rdma_gen_bin --verbs_mode=libsoftmetaroce --ib_dev= --gid_idx=0 --mtu=IBV_MTU_4096 --rc_servers=fc00::1 --message_size=4096 --num_messages=10 --op_type=send_recv --libsoftmetaroce_modifier_config=/home/ruizhex/modifier_config/ecnModifier.json

# Client side
$rdma_gen_bin --verbs_mode=libsoftmetaroce --ib_dev= --gid_idx=0 --mtu=IBV_MTU_4096  --message_size=4096 --op_type=send_recv
```

## Modifier Configuration in libsoftmetaroce

### Common JSON Fields

| Field                | Meaning                                                                                                                                                                            |
| -------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `name`               | Optional name for the modifier                                                                                                                                                     |
| `type`               | Modifier type (Drop, Reorder, Duplicate, etc.)                                                                                                                                     |
| `filter`             | Packet-matching criteria (see below for details)                                                                                                                                   |
| `direction`          | `"tx"` = outgoing, `"rx"` = incoming                                                                                                                                               |
| `packet_type`        | Packet type to match (e.g., `"data"`, `"ack"`)                                                                                                                                     |
| `random_percent`     | (Optional) Percentage of otherwise-matching packets that pass the filter. Defaults to 100. Supports fractional percentages greater than 0 and up to 100 |
| `fields`             | (Optional) Field-level match (e.g., fsn, flow_id, dst_qp, etc.). See [Filter Field Conditions](#filter-field-conditions) for supported formats |
| `modification_times` | How many matching packets to modify before stopping. Set to `"unlimited"`, `"always"`, or `"infinite"`, or omit this field entirely to allow the modifier to run every time it matches without stopping |

### Filter Field Conditions

Filter fields support several formats for matching packet header values.

#### Supported Fields

**Packet header fields** (integer-based, available for both data and ack packets):

| Field                       | Packet Type | Description                        |
| --------------------------- | ----------- | ---------------------------------- |
| `src_qp`                    | Data, Ack   | Source QP number                   |
| `dst_qp`                    | Data, Ack   | Destination QP number              |
| `flow_id`                   | Data, Ack   | Flow identifier. `path_id` is the former name and still matches |
| `fsn`                       | Data        | Frame Sequence Number              |
| `una`                       | Data        | Unacknowledged sequence number     |
| `posn`                      | Data        | Packet Offset Sequence Number      |
| `csn`                       | Data        | Completion Sequence Number         |
| `msn`                       | Data        | Message Sequence Number            |
| `cumulative_fsn`            | Ack         | Cumulative FSN                     |
| `max_rcvd_fsn`              | Ack         | Maximum received FSN               |
| `cumul_resp_delivered_msn`  | Ack         | Cumulative response delivered MSN  |
| `fsn_bit_vector`            | Ack         | FSN bit vector (array of 8 hex values) |

These fields support all comparison operators described below (equality,
`!=`, `>`, `<`, `>=`, `<=`, `In`).

**Address fields** (string-based):

| Field         | Description                                                        |
| ------------- | ------------------------------------------------------------------ |
| `remote_addr` | Remote IP address (IPv4 or IPv6). Compared against the IP address of the remote peer, ignoring the port number. The value must be a string. |

```json
"fields": {
  "remote_addr": "fc00::1"
}
```

#### Simple Equality (backward compatible)

The original format performs an exact-match comparison:

```json
"fields": {"fsn": 5, "path_id": 0}
```

#### Comparison Operators

You can use a comparison object with `op` and `value` to apply richer conditions.
Supported operators: `=`, `==`, `!=`, `>`, `<`, `>=`, `<=`, `In`

```json
"fields": {
  "fsn": {"op": ">=", "value": 5}
}
```

#### AND Logic (Multiple Conditions on One Field)

Pass an array of condition objects to combine them with AND logic.
For example, to match packets where `5 <= fsn < 10`:

```json
"fields": {
  "fsn": [
    {"op": ">=", "value": 5},
    {"op": "<", "value": 10}
  ]
}
```

#### In Operator (Match Against a Set)

The `In` operator checks whether a field value is contained in a given set.
Two syntaxes are supported:

**Explicit syntax:**

```json
"fields": {
  "fsn": {"op": "In", "value": [0, 1, 2, 4, 8]}
}
```

**Array shorthand:**

```json
"fields": {
  "fsn": [0, 1, 2, 4, 8]
}
```

Both forms are equivalent and match packets whose `fsn` is one of the listed values.

### Modifier-Specific Fields

| Modifier Type      | Extra Field(s)    | Meaning                                                                                                                        |
| ------------------ | ----------------- | ------------------------------------------------------------------------------------------------------------------------------ |
| reorder            | distance          | Defines reordering delay or distance<br>• unit: "packet" or "time_us"<br>• value: Packet distance or delay in microseconds |
| striped_message_wave | qp_count, tail_posn, max_pending_packets, release_interval_us | Emits complete messages as fragment stripes across QPs, followed by their aligned tails |
| duplicate          | delay_us          | Microsecond delay before sending the duplicated copy                                                                           |
| corrupt            | position_to_corrupt | Byte position to corrupt. If no position is provided, it defaults to corrupting the first byte of the packet.                |
|                    | value_to_set      | Specific value to set. If no value is provided, it defaults to XOR the packet with 0xff.                                       |
| write_edit         | target            | What to edit: "rkey" or "address"                                                                                              |
|                    | value_to_set      | Specific value to set. If no value is provided, it defaults to XOR with 0xff.                                                  |
| header_edit        | header_type       | Which header: Base, Data, ReadWrite, Ack, RnrCancel                                                                            |
|                    | field_name        | Name of the field to edit                                                                                                      |
|                    | value_to_set      | Replace the field with this value                                                                                              |
|                    | value_to_subtract | Subtract this from the field's current value. Numeric fields only, and mutually exclusive with `value_to_set`.                 |

## Examples

### Drop Modifier

Drops packets that match the specified filter:

```json
{
  "name": "drop_modifier_tx",
  "type": "drop",
  "filter": {
    "direction": "tx",
    "packet_type": "data",
    "fields": {"fsn": 1, "dst_qp": 100, "path_id": 0}
  },
  "modification_times": 1
}
```

### Modifier with Percentage

Applies the modifier to about 10% of packets that match the filter:

```json
{
  "name": "drop_modifier_tx_10_percent",
  "type": "drop",
  "filter": {
    "direction": "tx",
    "packet_type": "data",
    "random_percent": 10
  }
}
```

### Drop Modifier with Address Filter

Drops packets only when they are destined for a specific remote IP address.
The `remote_addr` field matches against the remote peer's IP address (ignoring the
port number):

```json
{
  "name": "drop_modifier_tx_addr",
  "type": "drop",
  "filter": {
    "direction": "tx",
    "packet_type": "data",
    "fields": {"remote_addr": "fc00::1", "dst_qp": 100}
  },
  "modification_times": 1
}
```

This is useful when multiple peers are connected and you only want to apply a
modifier to traffic going to or coming from a specific address.

### Reorder Modifier

Delays packets to change their order. Reordering can be based on packet count or
time.

**Packet-based reordering:**

```json
{
  "name": "reorder_modifier_rx_packet",
  "type": "reorder",
  "filter": {
    "direction": "rx",
    "packet_type": "ack"
  },
  "modification_times": 5,
  "distance": {
    "unit": "packet",
    "value": 3
  }
}
```

**Time-based reordering:**

```json
{
  "name": "reorder_modifier_tx_time",
  "type": "reorder",
  "filter": {
    "direction": "tx",
    "packet_type": "data"
  },
  "modification_times": 5,
  "distance": {
    "unit": "time_us",
    "value": 3000
  }
}
```

### Striped Message-Wave Modifier

```json
{
  "name": "striped_message_wave",
  "type": "striped_message_wave",
  "filter": {
    "direction": "tx",
    "packet_type": "data"
  },
  "qp_count": 4,
  "tail_posn": 7,
  "max_pending_packets": 160,
  "release_interval_us": 190
}
```

The modifier aligns the next complete message from each configured QP, emits
complete POSN stripes from zero through `tail_posn - 1`, then emits the aligned
tail. `qp_count` must equal the number of QPs that will send matching traffic.
If the configured wave cannot complete, the first held-fragment retransmit
releases the pending packets and switches the modifier to pass-through.
Packets are released asynchronously at `release_interval_us` intervals;
each packet arms only the next deadline from its actual send-start time, so
scheduler stalls cannot trigger a catch-up burst and callback overhead does not
reduce the configured rate.
Choose this wall-clock interval from the full frame-on-wire size (including
preamble/SFD, FCS, and IFG), target wire rate, and any runtime clock scaling.
Allow a small receiver margin for start-to-start callback overhead when flow
control is disabled.
Duplicate fragments for scheduled messages are suppressed. After a message
finishes, retransmission-marked fragments use a separate paced recovery queue.
Non-retransmitted duplicates and exact retransmits already in that queue are
suppressed; a retransmit becomes eligible again after its recovery copy emits.
The modifier also switches permanently to pass-through after reaching the
pending-packet cap.

### Duplicate Modifier

Create duplicate packets with a specified delay.

```json
{
  "name": "duplicate_modifier_tx",
  "type": "duplicate",
  "delay_us": 3000,
  "filter": {
    "direction": "tx",
    "packet_type": "data",
    "fields": {"fsn": 10, "path_id": 0, "dst_qp": 2}
  },
  "modification_times": 1
}
```

### Corrupt Modifier

Modifies specific byte positions in packets.

**Basic corruption:**

If no byte position is specified, the modifier corrupts the first byte by
default (XOR with 0xFF). Note that this breaks the byte that contains the
opcode, so attempts to process the packet this way should lead to immediate
failure.

```json
{
  "name": "corrupt_modifier_tx",
  "type": "corrupt",
  "filter": {
    "direction": "tx",
    "packet_type": "data",
    "fields": {"fsn": 1, "path_id": 0, "dst_qp": 2}
  },
  "modification_times": 1
}
```

**Corruption with specific value:**

```json
{
  "name": "corrupt_modifier_tx_specific",
  "type": "corrupt",
  "filter": {
    "direction": "tx",
    "packet_type": "data",
    "fields": {"fsn": 1, "path_id": 0, "dst_qp": 2}
  },
  "modification_times": 1,
  "position_to_corrupt": 42,
  "value_to_set": 255
}
```

### Write Edit Modifier

**Edit rkey:**

```json
{
  "name": "write_edit_modifier_tx_rkey",
  "type": "write_edit",
  "filter": {
    "direction": "tx",
    "packet_type": "data"
  },
  "modification_times": 10,
  "target": "rkey"
}
```

**Edit address:**

```json
{
  "name": "write_edit_modifier_tx_addr",
  "type": "write_edit",
  "filter": {
    "direction": "tx",
    "packet_type": "data"
  },
  "modification_times": 10,
  "target": "address"
}
```

**With specific value:**

```json
{
  "name": "write_edit_modifier_with_value",
  "type": "write_edit",
  "filter": {
    "direction": "tx",
    "packet_type": "data"
  },
  "modification_times": 10,
  "target": "rkey",
  "value_to_set": 12345678
}
```

### Header Edit Modifier

Modifies specific header fields. Supports multiple header types.

**Base header edit:**

```json
{
  "name": "header_edit_base",
  "type": "header_edit",
  "filter": {
    "direction": "tx",
    "packet_type": "data"
  },
  "modification_times": 1,
  "header_type": "base",
  "field_name": "dst_qp",
  "value_to_set": 12345
}
```

**Data header edit:**

```json
{
  "name": "header_edit_data",
  "type": "header_edit",
  "filter": {
    "direction": "tx",
    "packet_type": "data"
  },
  "modification_times": 1,
  "header_type": "data",
  "field_name": "posn"
}
```

**Relative edit (subtract):**

`value_to_set` pins a field to one constant, which is not what you want for a
field that advances from packet to packet. `value_to_subtract` holds it a fixed
distance behind whatever the sender chose, so the offset survives the field
advancing. The edit wraps within the field's own width, which is the arithmetic
the sequence-number fields (`fsn`, `una`, `msn`, `csn`) are already compared
under.

```json
{
  "name": "slow_una_tx_data",
  "type": "header_edit",
  "filter": {
    "direction": "tx",
    "packet_type": "data"
  },
  "header_type": "data",
  "field_name": "una",
  "value_to_subtract": 200
}
```

**Other header types:**

```json
{
  "name": "header_edit_ack",
  "type": "header_edit",
  "filter": {
    "direction": "rx",
    "packet_type": "ack"
  },
  "modification_times": 1,
  "header_type": "ack",
  "field_name": "cumulative_fsn",
  "value_to_set": 100
}
```

### Mark Ecn Modifier

Marks packets with Explicit Congestion Notification:

```json
{
  "name": "mark_ecn_modifier",
  "type": "markecn",
  "filter": {
    "direction": "tx",
    "packet_type": "data",
    "fields": {
      "fsn": 1
    }
   },
  "modification_times": 1
}
```

## Multiple Modifiers

Multiple modifiers that will be applied to outgoing/incoming packets

The example below drops with FSN = 1 packet and reorder packets with FSN = 2
packet by delaying it until 3 subsequent packets have been sent, repeating this
process 100 times.

```json
[
  {
    "name": "drop_modifier_1",
    "type": "drop",
    "filter": {
      "direction": "tx",
      "packet_type": "data",
      "fields": {"fsn": 1}
    },
    "modification_times": 100
  },
  {
    "name": "reorder_modifier_1",
    "type": "reorder",
    "filter": {
      "direction": "tx",
      "packet_type": "data",
      "fields": {"fsn": 2}
    },
    "modification_times": 100,
    "distance": {
      "unit": "packet",
      "value": 3
    }
  }
]
```

If you want to apply different modifiers on outgoing and incoming packets, you
can attach them separately on the client and server sides.

The example below drops one outgoing TX data packet with FSN=1 (simulating
packet loss on send) and marks one incoming RX data packet with FSN=2 with ECN
flag (simulating congestion notification from the network)

```json
[
  {
    "name": "drop_tx_data",
    "type": "drop",
    "filter": {
      "direction": "tx",
      "packet_type": "data",
      "fields": {"fsn": 1}
    },
    "modification_times": 1
  },
  {
    "name": "mark_ecn_rx_data",
    "type": "markecn",
    "filter": {
      "direction": "rx",
      "packet_type": "data",
      "fields": {"fsn": 2}
    },
    "modification_times": 1
  }
]
```
