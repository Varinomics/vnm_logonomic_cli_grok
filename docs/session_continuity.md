# Native session continuity

The managed hook channel observes Grok's top-level `SessionStart` events. Grok's
`/new` command, including its `/clear` alias, creates a fresh native session UUID.
Its `/resume` command opens the native conversation picker. A cold load reports
`source: "load"`; the package classifies that as a resume transition and publishes
the exact UUID in the opaque resume record. The shared host owns conversation
history, Notes associations and dismissal state. The provider does not delete
native history when a conversation changes.

## Observation boundaries

Grok's source value describes the history supplied when the actor is installed:
an empty vector produces `new`, and a nonempty vector produces `load`. Restoring
a saved empty history can therefore produce `new`. That event alone does not
prove resume intent or the reactivation of a dismissed conversation.

Selecting an already open conversation can change only Grok's local UI state.
That native path emits neither a new `SessionStart` nor an activation RPC. A
resident attach can also reuse its actor without emitting another start hook.
These paths do not provide immediate conversation-selection evidence to the
package. Complete live `/resume` observation requires a native activation signal
for them. Terminal text, later prompt activity and roster membership are not
used to guess which conversation the user selected.

Session hooks observe native behavior; they cannot veto a native selection or
enforce exclusive ownership across managed terminals. Cross-worktree ownership
and adoption remain shared-host responsibilities. Compaction is a context event,
not a new conversation identity.

## Default execution directory

When `execution-context@1` is negotiated, a valid root `SessionStart.cwd` supplies
the actor's local default execution directory. Its exact spelling, including a
nested directory and spaces, is preserved. It is attributed to that start's
transition nonce with source sequence zero. Workspace-root metadata and a tool's
temporary working directory do not supply this fact.

The execution-context companion is independent of lifecycle classification.
Missing, invalid or unrequested cwd evidence does not discard a valid identity
transition. Later hooks do not renew this snapshot because they lack a
source-captured activation nonce. Selecting the same native UUID again requires
fresh activation evidence; a retained directory is not a current confirmation.

## Read-only history inspection

The package declares `interactive-resume@2`, `@3` and `@4`. Version 2 prepares an
exact native resume. Version 3 adds read-only availability inspection; version 4
also reports conversation content. A version 3 response omits the content member.
Every inspection acknowledgment binds the complete resolved request.

Inspection and explicit preparation share exact-record, launcher, selected-account
and effective-environment validation and the native availability reader.
Inspection reads only the selected native profile. It does not launch Grok,
install hooks, allocate a profile, edit a transcript or select another UUID.
The version 3 contract requires explicit preparation to read availability afresh
before its effects. Both available and unknown evidence permit the exact native
`--resume <UUID>` operation; an inconclusive local search does not create a new
refusal.

Positive inspection requires one matching native session directory, a readable
typed `summary.json` whose identity and cwd agree, readable native history files
and a verified native storage path.
The reader supports canonical URL-encoded cwd directories under
`GROK_HOME/sessions`. Long-cwd slug/hash directories, alternate native lookup
modes, ambiguous identities, unsupported formats, inaccessible storage and local
misses produce unknown evidence. A local miss does not establish absence from
Grok's broader lookup, including restored-child and nonlocal paths.

For conversation format 1, complete primary-system initialization and its
optional baseline skill reminder can establish empty content. A persisted title,
successful startup-observer status and an empty background-task snapshot are
metadata. Hook output, failures and incomplete task snapshots do not supply empty
evidence. Recognized human, assistant and tool records, retained message chunks in
the replay stream and `/btw` side conversations establish nonempty content.
Unrecognized content remains unknown. A missing or zero-byte chat cache is never
an initialization proof: Grok may rebuild it from `updates.jsonl`. A summary-only
session skeleton also supplies no empty-content evidence.

Empty-content evidence requires complete, stable reads and consistent companion
records. Side artifacts outside the understood initialization files, including
unsupported feedback or compaction history, prevent an empty result. Partial
lines, unsupported records, contradictory counts and uncertain compaction replay
remain unknown. Titles, usage, file size and an absence of
observed prompt hooks do not establish emptiness. Inspection is bounded to 4096
cwd entries, 1 MiB for a summary and 16 MiB/65,536 lines for each history stream;
exceeding a bound supplies unknown evidence. The host may withhold a row after
acknowledged empty evidence while retaining its native history and association.
A later inspection can make that row visible again.

The native source paths and evidence revision are recorded in
[`upstream_contract.json`](../package/provenance/upstream_contract.json).
That provenance is evidence for these contracts, not a provider-version ceiling.
