# ACK source ports

`Spec 7.1.1` recommends copying the acknowledged data packet's UDP source port.
It also permits an alternative: the ACK source port must change with high
probability when the data source port changes.

libsoftmetaroce sends through bound UDP sockets. A peer-selected port may
already be used by another process, so copying that port cannot be guaranteed.
Instead, an ingress source-port change makes the sender select another ACK
socket on the same local address.

Changes before ACK emission coalesce into one socket change. The first observed
source port establishes the baseline. If no alternative exists and the socket
limit prevents creating one, the ACK retains its source port. Raw transports
have no UDP source port.
