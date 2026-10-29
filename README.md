# Logonomic Grok Build add-on

[![Grok add-on CI](https://github.com/Varinomics/vnm_logonomic_cli_grok/actions/workflows/provider-ci.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_logonomic_cli_grok/actions/workflows/provider-ci.yml)

Standalone C++20 package `logonomic.cli.grok`, using the public Logonomic CLI
add-on SDK. Windows/Linux x86-64 and macOS ARM64 artifacts are built separately.

The provider preserves the selected `GROK_HOME` profile. Account discovery uses
Grok's platform home convention and empty-override behavior. Authentication
inspection reads the native `grok models` status banner after successful process
completion. Quota inspection reads the selected profile's current public xAI
OIDC credential and calls Grok's fixed HTTPS billing and settings endpoints.
The bearer stays in the short-lived helper's memory and request headers; the
package does not refresh or write Grok credentials. Custom auth, team, and
proxy routes are unavailable for this quota probe. The usage window reports
Grok's own percentage, which may exceed 100, its actual period length and its
reset time. Monthly periods retain their actual number of days. Fractional
period boundaries retain their precision when measuring the window; the reset
is rounded down to the SDK's millisecond precision. Malformed or stale periods,
and durations that cannot be expressed exactly in whole minutes, fail the
refresh instead of reporting absent quotas. While the selected profile's
credential is expired, each quota refresh fails until Grok
itself refreshes the credential. On Linux and macOS the helper uses libcurl, so
building the package needs its development files.

Native file hooks report session and turn starts through authenticated SDK
helper ingress. Successful turn completion and subagent completion are not
advertised: Grok's Stop hooks are gates that may continue the turn, and the
file-hook channel has no matching confirmed subagent completion producer.
Managed configuration targets only `hooks/logonomic.json` within the selected
Grok home, under the host's bounded snapshot-and-replacement contract. The
persisted command is inactive without Logonomic's launch authority and resolves
the helper from that launch's environment. An existing different file is refused
without overwriting its contents. Title and coordination instructions are supplied
by the authenticated host at the first eligible root PreToolUse hook, whose
additional context reaches the model. A turn that never calls a tool does not
provide this delivery opportunity. Child hooks cannot consume the root title.

Exact resume uses the saved native session UUID and native `--resume`; conflicting
launcher identity or workspace options are refused. The host applies the managed
hook transform before the resumed launch and admits its actual SessionStart.

Optional `session-wake@1` prepares an isolated native leader together with the
original interactive TUI and a persistent ACP control client. A mailbox wake
checks that the exact native session and working directory remain resident, then
submits a native `session/prompt`. Its receipt means Grok acknowledged the prompt
in its queue; it does not mean the mailbox was read. The original composer and
approval UI remain with the TUI. The control client never answers permission,
question, or elicitation requests, and never loads or replaces a conversation.
The selected profile, launch environment and managed hooks are inherited by the
private leader. Existing launches without this preparation cannot be woken.

After a mailbox wake has subscribed that existing control connection, native
pending/resolved notifications and the matching interactive RPCs report root-session
permissions, questions, plan approvals and MCP elicitation. Each wait retains its
native RPC request ID, so successive waits for one tool call remain distinct. A
permission guard resolved by automatic policy does not establish attention. Optional
`attention-kind@1` distinguishes permission from interactive-question attention.
Observation issues no subscription or response requests. Original-TUI prompts
before the first wake, startup/trust prompts, child sessions and standalone
launches remain outside this coverage. A new session epoch requires another
wake before observation resumes. Lost notifications or a lost control connection
cannot be reconciled: Grok needs a non-driving observer role and a read-only
pending-interaction snapshot for complete startup and reconnect coverage. Loss
does not fabricate a resolution; admitted pending attention can remain until a
native resolution or host session transition clears it.

Wake requires the authenticated current-launch SessionStart identity and its
exact host transition nonce, followed by an unscoped native roster check. A
resume of the same native UUID must still match the new transition nonce.
Changing or ending the native conversation resets this identity. Busy, pause
and permission state remain independent. The control client does not answer
approval requests or issue resume commands. The host defers new notifications
after a confirmed interruption; notifications already accepted by Grok retain
its native queue behavior and may run after the current turn is cancelled.

Model, effort, approval, exact session-ID and exact resume options may use this
launch form. Other CLI options retain standalone execution and report wake
unavailable, including permission rules, disabled subagents, disabled web search,
and explicitly selected leader settings. On Windows, failure to start an
isolated leader falls back to the original standalone invocation after the
private job has been settled. On Linux and macOS, a failed startup after any native
process was admitted ends the launch so the terminal owner can settle its owned
processes; fallback is limited to failures before native admission. The live resident check
also rejects a TUI which chose its native standalone fallback. There is no
provider version ceiling.

Windows contains the leader, TUI and control client in a nested kill-on-close
job. Linux preserves the terminal process group and relies on Logonomic's
maintained PTY custody owner to settle orphaned descendants, including native
leader reconnects that change process group. macOS inherits the controlling
terminal and original process group, relying on the retained PTY owner to settle
its owned process groups in that original terminal session. Deliberately detached
descendants remain outside that Darwin cleanup scope. The helper is an indexed package
executable intended for that managed terminal launch. TUI exit or leader failure
ends the private runtime; wake requests never spawn another provider process.
Private control IPC is limited to the current OS user. A lost response after
submission is uncertain and is never automatically replayed. The live control
connection retains up to 4096 notification receipts without eviction; beyond
that bound further wake is unavailable until a new managed launch.

Structured implementation and delegated tasks use the native ACP stdio agent.
Delegation supplies the host's task-control MCP servers, verifies their native
session catalog reports ready and exposes every required action, then submits the
prompt. Native end-turn and an acknowledged session close are required for a
result. Model selection has no version ceiling.

A structured or delegated task keeps the launcher's permission posture. The
launcher's `--model`, `--reasoning-effort` and `--always-approve` options carry
over to `grok agent`; the interactive CLI's other options, among them
`--permission-mode`, `--allow` and `--deny`, have no agent-mode equivalent and
are refused with `delegated_launcher_options_unavailable`, naming the option. When
Grok asks for a tool's permission, the package answers with the request's own
`allow-once` option only for the task's own Task tools
(`logonomic-<task>-<server>__<action>`) and with `reject-once` for anything else,
which nobody can approve in a headless run; it never chooses `always-allow`.
Grok ends a turn at a rejected tool, and the task fails with
`tool_permission_denied`. A task that edits files or runs commands therefore
needs a launcher that approves them, such as one passing `--always-approve`.

Usage reads `sessions/<cwd>/<session>/usage.json` with its `summary.json`.
Persisted turns already contain deltas, including folded subagent spend. The
scanner counts each top-level turn once, preserves category values and retains
cached history when source files disappear. Reasoning and cache subsets are
not added again to the native input-plus-output total.

The independently negotiated `usage-temporal@1` capability supplies dated activity
on host-selected `usage-scan@3` pages. Each admitted top-level turn can carry its
exact native total at the original strict RFC3339 `endedAt` instant, retaining
offset and fractional precision. A timestamp without a timezone remains usable
for accounting and supplies no instant. Zero totals have no positive portion;
incomplete categories do not change the turn's proved total.
The established accounting dates, IDs and cumulative replay semantics remain
unchanged.

Dated evidence lives in separate, independently versioned optional files. A
stable native reparse must match the complete chosen accounting ledger; retained
proof must match the selected account, snapshot, full original accounting digest
and exact parent total before repository attribution is refreshed. Deleted native
files can retain that selected proof. Conflicting instants, unavailable evidence,
or unsupported optional files never invent dated amounts or alter accounting.
Accounting and its final sealed inventory receive space first. Optional scratch,
the evidence file, its index and the additional inventory entries share the
existing physical limits; capacity refusal removes those optional files and
preserves the complete accounting candidate.

Grok forks copy billed turns without durable original-row identities. An
ordinary fork carrying usage therefore fails the account's candidate scan with
`usage_fork_attribution_unavailable`; the last accepted history remains intact.
The scanner does not guess ownership from timestamps or equal token counts.
Native subagent session kinds are excluded because their spend is folded into
the parent ledger. This is exact accounting for supported unforked top-level
history, not universal fork attribution.

Each source JSON document is limited to 16 MiB. Candidates are limited to 1 GiB
and 1,048,576 records. Retained cache and scratch together are limited to 8 GiB
of logical bytes, 1,048,576 filesystem entries and 8,388,608 persisted lines.
Admission precedes file creation and writes; limits fail explicitly without
evicting retained history. Sorting uses bounded chunks and on-disk merge runs;
continuation pages read immutable snapshots. Each scan builds a private generation
of source artifacts, binding their fingerprints and parsed records together.
An exact manifest identifies retained artifacts by source key and content digest;
one durable pointer selects the accepted generation after the scan is complete.
Cancelled or rejected candidate scans cannot modify accepted retained history.
A filesystem sync failure after pointer replacement reports an indeterminate
commit outcome and preserves both generations.

The SDK source dependency tracks the owned Logonomic `master` branch. Set
`LOGONOMIC_CLI_ADDON_SDK_SOURCE` to a local `sdk/logonomic_cli_addon_sdk` directory
for coordinated development. `grok_package` produces the indexed package under
`package/logonomic.cli.grok` in a single-configuration build directory, or
`package/<config>/logonomic.cli.grok` with a multi-configuration generator.
Follow workstation build
queue and toolchain instructions for configuration and compilation. CTest
registers runtime, account/probe, usage, package-validation and conformance
fixtures. Building a package does not activate it or migrate user data.

The package recipe proves the Windows/Linux x86-64 or macOS ARM64 compiler target
during configuration. `cmake --install` exports a fresh artifact beneath its install-time
prefix and preserves executable permissions. An existing per-package destination,
including a dangling symlink, is refused. POSIX `DESTDIR` staging is supported;
nonempty `DESTDIR` with a drive-letter prefix is refused. Export is not atomic:
a failure may leave a partial artifact directory. Choose a fresh destination or
manually remove only that identified disposable export before retrying; never
remove an installed generation this way. The application's package ingestion
owns atomic installation and updates.

The upstream source revision and contract locations are recorded in
`package/provenance/upstream_contract.json`. The Grok icons derive from Lobe
Icons and retain the packaged MIT notice in `package/notices/lobe_icons_mit.txt`;
per-resource source and normalization details are in
`package/provenance/lobe_icons.json`. Grok names and marks identify the
independent upstream software; they do not imply endorsement.
