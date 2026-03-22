# SafeChain

LoRa-based emergency signaling network with three device roles:

* **Node** — battery-powered emergency device that originates alerts
* **Repeater** — relays alerts and ACKs across multiple hops
* **Gateway** — receives alerts, acknowledges delivery, and forwards events to a local computer for visualization and analysis

This project is being upgraded from a working prototype into an **industry-grade state-machine network infrastructure** with stronger delivery guarantees, clearer protocol semantics, reboot recovery, and safer firmware behavior.

---

## Current Architecture

### Device Roles

* **Node**

  * Creates emergency events
  * Uses a stable `event_id` across retries
  * Stores pending emergency state locally for reboot recovery
  * Waits for a matching ACK before confirming success

* **Repeater**

  * Relays both emergency `EVENT` frames and return `ACK` frames
  * Performs duplicate suppression
  * Enforces hop-count limits
  * Supports both legacy packets and V1 protocol frames during migration

* **Gateway**

  * Accepts V1 emergency frames
  * Deduplicates by logical event identity
  * Sends matching V1 ACKs back to the origin node
  * Journals accepted events in the gateway event manager during runtime
  * Keeps legacy receive support during migration

---

## Protocol Upgrade Summary

The original prototype overloaded transport meaning and event meaning into the same message field and changed message identity on retry. The current refactor introduces a cleaner V1 protocol.

### V1 protocol concepts

* `frame_type` — transport meaning (`EVENT`, `ACK`, etc.)
* `event_type` — emergency meaning (`FIRE`, `FLOOD`, `CRIME`, `SAFE`)
* `event_id` — stable emergency identifier across retries and relays
* `attempt` — retry counter for the originating node
* `origin_id` — node that created the event
* `sender_id` — device that transmitted the current hop
* `hop_count` / `max_hops` — multi-hop relay control

### Why this matters

* Retries are no longer mistaken for new emergencies
* ACK matching is deterministic
* Duplicate filtering works on logical identity instead of local packet sequence alone
* Future expansion is safer because transport and business semantics are separated

---

## Implementation Status Tracker

### Implemented

#### Shared protocol

* [x] Added `safechain_protocol.h/.cpp` to define the V1 frame schema
* [x] Added protocol-level CRC validation for V1 frames
* [x] Separated `frame_type` from `event_type`
* [x] Introduced stable `event_id` for emergency lifecycle tracking
* [x] Added relay-safe `sender_id` updates per hop

#### Node

* [x] Replaced retry identity model so retries keep the same `event_id`
* [x] Added persistent event counter using storage/NVS
* [x] Added pending-event journal (`NodeEventRecord`)
* [x] Journal pending event before first transmit
* [x] Resume pending event after reboot
* [x] Accept only matching V1 ACK for current event
* [x] Moved node emergency flow into a cleaner controller/state model

#### Repeater

* [x] Replaced router subsystem with clean dual-stack relay logic
* [x] Added V1 duplicate suppression using `(origin_id, event_id, frame_type)`
* [x] Relay V1 `EVENT` frames
* [x] Relay V1 `ACK` frames
* [x] Retained legacy packet relay during migration
* [x] Added delayed relay scheduling and relay statistics

#### Gateway

* [x] Added dual-stack receive path for legacy + V1 frames
* [x] Added V1 event processing and matching V1 ACK generation
* [x] Added V1 duplicate handling based on logical event identity
* [x] Replaced gateway event manager with cleaner subsystem split
* [x] Added gateway journal manager for accepted V1 events during runtime
* [x] Retained legacy packet processing during migration

#### End-to-end network flow

* [x] Node -> Gateway direct V1 path
* [x] Node -> Repeater -> Gateway V1 multi-hop path
* [x] Gateway -> Repeater -> Node V1 ACK return path
* [x] Stable event identity preserved across retries and relays

---

### In Progress / Partially Implemented

* [ ] Gateway journal is structured, but not yet truly **persistent across reboot**
* [ ] Host commit state exists conceptually, but end-to-end host acknowledgment is not yet complete
* [ ] Node controller is cleaner, but not yet a full RTOS/task-based FSM implementation
* [ ] Legacy compatibility remains enabled during migration and still needs eventual removal
* [ ] Observability counters exist, but not all corruption/fault metrics are fully wired everywhere

---

### Still Needed for Industry-Grade State-Machine Infrastructure

#### Reliability and persistence

* [ ] Make gateway journal persistent across reboot/power loss
* [ ] Add replay of uncommitted gateway events after reboot
* [ ] Add explicit host acknowledgment so gateway can distinguish:

  * `received_by_gateway`
  * `queued_for_host`
  * `committed_to_host`
* [ ] Add retained dead-letter handling for failed host delivery

#### Firmware resilience

* [ ] Add watchdog integration (task watchdog / interrupt watchdog where supported)
* [ ] Add reboot reason capture and boot counters
* [ ] Add brownout-safe operating mode
* [ ] Add radio fault counters and radio reinitialization strategy
* [ ] Add low-battery emergency transmission policy

#### State-machine hardening

* [ ] Move node controller fully to non-blocking service/state architecture
* [ ] Separate UI effects (LED/buzzer/BLE) from emergency transport logic
* [ ] Add explicit repeater queue prioritization (`ACK` before `EVENT` before status traffic)
* [ ] Add TTL/expiry handling for relay queue entries
* [ ] Add gateway ingress pipeline stages for validate -> journal -> host dispatch -> ACK

#### Security

* [ ] Add message authenticity tag / MAC verification in production mode
* [ ] Store configuration and keys in encrypted storage
* [ ] Add secure provisioning mode vs production mode
* [ ] Add secure boot / flash encryption where manufacturing allows

#### Testing and validation

* [ ] Add protocol serializer/parser golden tests
* [ ] Add node/repeater/gateway state transition unit tests
* [ ] Add duplicate injection tests
* [ ] Add corrupted packet injection tests
* [ ] Add brownout/reboot recovery tests
* [ ] Add 72-hour soak test
* [ ] Add multi-hop congestion tests
* [ ] Add host disconnect / replay recovery tests

#### Migration cleanup

* [ ] Remove legacy packet path once all devices are on V1
* [ ] Remove legacy ACK handling assumptions
* [ ] Normalize config/constants across node, repeater, and gateway projects
* [ ] Consolidate shared code into a real shared library layout

---

## Project Status

### Current maturity

* **Prototype maturity:** working and network-functional
* **Protocol maturity:** significantly improved with V1 event identity and ACK flow
* **Industry-grade readiness:** not complete yet

### Plain assessment

SafeChain now has a working V1 transport foundation and a more reliable end-to-end emergency path, but it is still in the **hardening phase** rather than final production readiness.

The biggest improvements already completed are:

* stable emergency identity
* deterministic ACK matching
* repeater ACK relay support
* pending-event recovery on node reboot
* cleaner subsystem boundaries for gateway and repeater

The biggest remaining gaps are:

* true persistent gateway journaling
* watchdog and brownout recovery
* security/authenticity controls
* full non-blocking production-grade state machine runtime
* stronger testing and validation coverage

---

## Reference Specification

This repository is being aligned with the design document:

**SPEC-1-SafeChain-State-Machine-Hardening**

The specification defines the target architecture for:

* versioned wire protocol
* explicit state machines for node, repeater, and gateway
* persistent event journaling
* deterministic ACK and retry behavior
* health monitoring and recovery
* security baseline
* milestone-based implementation plan

### Spec sections

* Background
* Requirements
* Method
* Implementation
* Milestones
* Gathering Results

This README reflects the current implementation status against that specification.

---

## Recommended Repository Structure

```text
Safechain_Node/
  Safechain_Node.ino
  emergency.h
  emergency.cpp
  storage.h
  storage.cpp
  safechain_protocol.h
  safechain_protocol.cpp

Safechain_Repeater/
  Safechain_Repeater.ino
  router.h
  router.cpp
  safechain_protocol.h
  safechain_protocol.cpp

Safechain_Gateway/
  Safechain_Gateway.ino
  event_manager.h
  event_manager.cpp
  gateway_journal.h
  gateway_journal.cpp
  safechain_protocol.h
  safechain_protocol.cpp
```

Long term, these shared protocol files should be moved into a common library/module instead of being copied across projects.

---

## Recommended Next Milestones

### Milestone 1 — Gateway durability

* Make gateway journal persistent across reboot
* Add host acknowledgment and replay support
* Improve gateway corruption and delivery audit metrics

### Milestone 2 — Node hardening

* Finish non-blocking node controller runtime
* Add watchdog, reboot reason, and brownout policy
* Separate UI services from transport logic

### Milestone 3 — Security baseline

* Add MAC/authenticity checks
* Add encrypted storage for device settings and keys
* Add production security profile

### Milestone 4 — Migration cleanup

* Remove legacy packet path
* Normalize configs and shared constants
* Convert shared protocol into a reusable library

### Milestone 5 — Validation

* Add automated tests and fault injection
* Perform long-duration soak and multi-hop reliability testing
* Measure delivery latency and duplicate suppression accuracy

---

## Suggested GitHub Update Notes

### Suggested commit title

`feat: migrate SafeChain to V1 event protocol with repeater ACK relay and node reboot recovery`

### Suggested commit summary

* add V1 protocol with stable event identity
* replace node retry model to keep same event across retries
* add node pending-event journal and reboot resume
* replace repeater router with V1 duplicate-aware relay subsystem
* replace gateway event manager with V1 journaled ACK flow
* retain legacy protocol compatibility during migration

### Suggested project board columns

* Done
* Hardening in progress
* Validation
* Production readiness blockers

---

## Notes

This project currently uses a migration-friendly dual-stack design so legacy and V1 packets can coexist temporarily. The final industry-grade version should remove legacy protocol handling once the full fleet is upgraded and validated.
