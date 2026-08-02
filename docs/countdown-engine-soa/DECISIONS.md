# Architecture Decisions

## ADR-001: Platform-neutral core

The engine contains no display, touch, network, or storage implementation.

## ADR-002: Command-event model

Clients submit commands. The engine validates them and emits events.

## ADR-003: Round plug-ins

Numbers, Letters, and Conundrum are independent modules behind one shared interface.

## ADR-004: Host-authoritative multiplayer

Only the current host commits authoritative state changes.

## ADR-005: Leader-based host ownership

The score leader becomes host between rounds. Ties retain the current host.

## ADR-006: Injected time and randomness

Clock and random sources are interfaces to support deterministic tests.

## ADR-007: Event replay

Accepted events can reproduce match state and support debugging, recovery, and audit.

## ADR-008: Separate content licensing

Conundrum word-and-hint packs may use a license separate from the engine.
