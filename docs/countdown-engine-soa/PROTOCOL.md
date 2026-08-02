# Multiplayer Protocol

## Authority model

One peer is authoritative at any time.

The authoritative peer:

- validates commands
- emits events
- assigns sequence numbers
- publishes snapshots
- commits scores
- manages host transfer

## Message families

```text
HELLO
CAPABILITIES
SYNC_REQUEST
SYNC_SNAPSHOT
COMMAND
EVENT
HOST_TRANSFER_PREPARE
HOST_TRANSFER_ACK
HOST_TRANSFER_COMMIT
PING
PONG
ERROR
```

## Required envelope fields

```text
protocolVersion
engineVersion
matchId
roundId
senderPlayerId
hostPlayerId
hostTerm
sequenceNumber
messageType
payload
checksum
```

## Duplicate protection

Commands should include a client-generated command ID.

The host stores recently processed command IDs and returns the original result when a duplicate arrives.

## Recovery

A peer requests a snapshot when:

- it reconnects
- it detects a sequence gap
- it receives a newer host term
- its local checksum differs
