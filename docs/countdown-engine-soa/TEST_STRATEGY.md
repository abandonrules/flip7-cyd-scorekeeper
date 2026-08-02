# Test Strategy

## Core principle

All game rules must be testable without a display, network, or real clock.

## Required test doubles

- FakeClock
- SeededRandomSource
- InMemoryMatchStore
- LoopbackTransport
- ManualDictionary
- RecordingEventSink

## Critical regression cases

- stale host event rejected
- host transfer on lead change
- host retained on tie
- reconnect restores exact state
- replay reproduces final score
- invalid Number step does not consume operands
- Letters choice buttons never allow illegal final composition
- Conundrum wrong answer preserves solution and forces only local reshuffle
- duplicate command does not apply twice
