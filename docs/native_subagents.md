# Native subagent observation

Interactive Grok launches expose native child activity through `lifecycle-events@2`
continuous observation. The package uses the maintained framework filesystem
service and the launch's authenticated file hooks. Observation does not attach a
new ACP client or drive the native TUI.

## Native evidence and ownership

The host reserves `lifecycle-correlation-lifetime@1` before classifying the root's
`SessionStart`. That authenticated event binds the native root session ID, host
transition nonce and exact `transcriptPath`. The transcript must identify the
native `<session-id>/updates.jsonl` path. Disabling native recording leaves root
file-hook behavior available but cannot provide journal-backed child completion.

`SubagentStart` runs in the parent actor and supplies the parent session ID,
parent transcript and child ID/type. Grok awaits the hook before appending its
`subagent_spawned` notification. The observer records the journal's current byte
position during that hook. Only a later native spawn row matching the admission
can open an interval. Existing transcript history and unadmitted inventory rows
cannot create child activity for a new launch or resume.

Native storage uses a JSONL envelope with `method: "_x.ai/session/update"`, an
outer `params.sessionId` and `params.update`. The child fields remain snake_case:

| Native field | Meaning used by the observer |
| --- | --- |
| `subagent_id`, `child_session_id` | Matching native child session identity |
| `parent_session_id` | Immutable parent on a spawn record |
| `attempt_id` | One execution attempt within the child session |
| `subagent_type` | Matches the authenticated hook's bounded child label |
| `status` | `completed`, `failed` or `cancelled` on a finished record |

Each attempt receives a stable host agent ID scoped to the root transition nonce.
A resumed native child with a new `attempt_id` opens a separate interval. A
duplicate or stale finished row cannot stop a later attempt. Records without a
usable native attempt identity fail observation; the adapter does not guess a
replacement from timestamps, prompt text or the child's reusable session ID.

A nested parent's hook can precede the outer parent's journal append. Its exact
transcript and byte frontier are retained pending an ancestry join. The observer
reads that journal only after authenticated native spawn records establish its
chain to the root. Parent assignments never change, and every emitted descendant
uses the original root's `provider_session_id`. The current host projection is
flat beneath that root; the provider preserves ancestry for routing validation.

## Ordering, stops and retirement

`subagent_finished` supplies terminal evidence after the native gate completes.
`SubagentStop` can be vetoed and is never a stop observation. A child session end
cannot become a root session end. Child events carry no root phase, outcome,
attention or session transition, and attempt identifiers are not presented as a
session-wide provider sequence.

The persistent adapter queues paired child observations. `poll_observations`
returns at most sixteen facts per page with replayable event IDs and cursors.
An observe continuation does not acknowledge that queue. Root `SessionEnd`
follows already durable admitted child facts and appears on the final page with
`more: false`. A missing finish is never manufactured to make that page look
quiescent.

An acknowledged final cursor permits terminal retirement. Host abandonment
immediately retires the correlation, wakes an active observe window and releases
its filesystem subscription. Retired epochs cannot admit later child hooks.
Resuming the same native root first reserves its new epoch; the host then retires
the old one before starting observation. Concurrent live owners of the same
native journal are refused.

## Failure boundaries

The reader retains an incomplete trailing JSONL record until its newline arrives.
Malformed complete records, incompatible identities, changed ancestry, observed
journal truncation or changes at the retained byte frontier, unreadable admitted
journals and exhausted bounded state fail
the observation channel explicitly. None is interpreted as a child stop. Native
filesystem events trigger acquisition through the shared service; cancellation
waits do not poll the filesystem.

The adapter retains at most 256 host correlations, 128 active child attempts and
4096 child attempt identities/admissions per correlation. Its queue also retains
at most 4096 unacknowledged observations. A native JSONL record is bounded to
16 MiB, buffered partial records to 64 MiB per correlation, and one reconciliation
read to 4 MiB before yielding through the service. Limits fail explicitly instead
of evicting identity or terminal evidence.

These observations support the ordinary managed launch owner. They do not create
a universal lease across arbitrary physical-file aliases, profile copies or
unmanaged native processes. They also do not establish that a delegated task's
background descendants have drained, or that a native session close is atomic
with descendant completion. Those are separate native ACP completion contracts.

## Native source contract

The parser and ordering tests follow the public
[`xai-org/grok-build` source at `2bdd1d6`](https://github.com/xai-org/grok-build/tree/2bdd1d6a6369de0e8c68132ea4539e9abd9e14a8):

- [`extensions/notification.rs`](https://github.com/xai-org/grok-build/blob/2bdd1d6a6369de0e8c68132ea4539e9abd9e14a8/crates/codegen/xai-grok-shell/src/extensions/notification.rs)
  defines the persisted child fields and attempt identities.
- [`session/storage/mod.rs`](https://github.com/xai-org/grok-build/blob/2bdd1d6a6369de0e8c68132ea4539e9abd9e14a8/crates/codegen/xai-grok-shell/src/session/storage/mod.rs)
  defines `SessionUpdateEnvelope`.
- [`session/acp_session_impl/updates.rs`](https://github.com/xai-org/grok-build/blob/2bdd1d6a6369de0e8c68132ea4539e9abd9e14a8/crates/codegen/xai-grok-shell/src/session/acp_session_impl/updates.rs)
  awaits the parent start hook before persistence and drops stale native attempts.
- [`hooks/dispatcher.rs`](https://github.com/xai-org/grok-build/blob/2bdd1d6a6369de0e8c68132ea4539e9abd9e14a8/crates/codegen/xai-grok-hooks/src/dispatcher.rs)
  awaits observe-hook command completion.

The source revision records evidence rather than a provider version ceiling.
Compatibility is determined by the actual native hook and durable record shapes.
Focused local tests exercise those source-backed inputs with the real framework
filesystem service and SDK lifecycle/correlation contracts; they are not a claim
of authenticated native-model qualification on every supported OS.
