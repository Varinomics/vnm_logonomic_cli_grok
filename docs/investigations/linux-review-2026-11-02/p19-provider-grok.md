# P19 — Grok launcher and wake bridge investigation

Date: 2026-11-02. Scope: findings 45, 66, 67, 135, 136 and 137 in package B51.

## Outcome and integration status

The Linux investigation implemented and independently reviewed four fixes: runner-aware launcher projection (#45), duplicate standalone-option insertion (#66), approval-option recognition (#135) and catchable POSIX signal cleanup (#137). It also reproduced the native-record parser failure (#67) with a small, source-supported input and measured the existing wake bridge (#136) without changing its polling strategy.

The provider changes are published in [draft PR #8][provider-pr], branch `work/linux-review-20261102-p19-provider-grok`. The shared SDK prerequisite is published in [draft PR #173][sdk-pr], branch `work/linux-review-20261102-p19-launcher-parser`. **Neither PR is merged.** Independent review found no outstanding correctness blocker in the tested Linux scope. Native PowerShell execution and the platform limits below remain follow-up before marking the changes ready.

| Finding | Evidence established here | Disposition | Remaining boundary |
|---|---|---|---|
| 45 | Three exact argument-byte mismatches reproduced against the baseline; direct-host quote/Unicode contract checked; seven final provider CTests and both focused SDK launcher checks passed | **Fixed and independently reviewed in the tested Linux scope** through shared SDK invocation projection and Grok consumption | The aggregate SDK test target remains compile-blocked in unchanged quota test code; native PowerShell execution is unavailable |
| 66 | Duplicate insertion reproduced against baseline production libraries; actual Grok parser configuration and its locked clap version inspected | **Fixed and independently reviewed**: insert `--no-leader` only when absent as an option | No real Grok executable was launched |
| 67 | A 259-byte native-shaped `session/update` closes an initialized production protocol and a later valid response does not reopen it; native source supports the payload route | **Reproduced; production parser unchanged**; follow-up design remains open | The record was synthesized from source, not captured from a live Grok/model session |
| 135 | Approval editing rejected verified native options in an executed baseline regression | **Fixed and independently reviewed**: accept the omitted options/aliases while preserving approval semantics | Capability evidence is tied to the inspected native snapshot |
| 136 | Real bridge idle CPU/context switches and full transport submission-to-receipt latency measured with scripted native peers | **Measured; polling retained unchanged** | No battery, many-terminal, real-model or user-interface measurement |
| 137 | HUP/INT/TERM in both startup and resident phases reproduced retained runtime directories; all six corresponding candidate probes removed them | **Fixed and independently reviewed**: cleanup before restoring/re-raising the original signal | SIGKILL, crashes, native macOS/Windows and full terminal-owner integration are outside the executed coverage |

### Published changes and dependency order

| Owner/change | Published commit |
|---|---|
| Shared SDK literal invocation API, PR #173 | [0e9aa901be11629a8febc179745d268d85595fb0][sdk-launcher-commit] |
| Provider standalone and approval options, PR #8 | [808ba80308361e439c89a5d3c5dfcd7b32452263][option-commit] |
| Provider POSIX signal cleanup, PR #8 | [644eb32d2ef7193eb48f858a15fa848b3c4246bf][signal-commit] |
| Provider runner-aware invocation consumer, PR #8 | [d80d2d07cd8a6537ad42f00de8452279ecef4928][provider-launcher-commit] |

**Merge the SDK prerequisite before the provider consumer.** Build the consumer with that SDK source/API; the original SDK master does not contain the new helper. All five SDK changes are within `sdk/logonomic_cli_addon_sdk`, outside the owning paired application paths, so the paired-application-path gate does not apply. The recorded source hashes identify investigated inputs and are not a runtime version ceiling or a promise about later upstream behavior.

## Source and build provenance

The unchanged provider baseline was [c4d9145bd8fd19b7c522b22bc4f83675e2463f53][provider-base]. The SDK source was refreshed from Logonomic master at [b9e44f6fe131624d48b0338c23295dae67d526bd][sdk-base]. All 536 files in the SDK subtree were verified against Git blob identities before and after the baseline gates. Candidate option and signal gates used isolated, recorded source snapshots, so concurrent #45 edits were not silently included. The signal snapshot combined the baseline with the published option correction and the five signal-related files; its 97 provider files and the 536 SDK files remained unchanged through those gates. For the final launcher gates, all 97 provider files and 537 SDK files matched between the frozen inputs and live sources. Both published code trees matched their exact local trees before this report was added.

At publication, SDK master had advanced one commit to [a87018b7d67dc7ecda8983049d19b6edfae199a8][sdk-reconciled-master]. Its nine changes affected release workflows, tools and documentation; no SDK, CMake or AGENTS input changed. The reviewed candidate retained the captured SDK base, and PR #173 was verified mergeable with a clean merge state. Provider master remained at the captured baseline. This reconciliation did not require replacing or retesting unaffected inputs.

Native CLI and broadcast contracts were read at [xai-org/grok-build 2bdd1d6a6369de0e8c68132ea4539e9abd9e14a8][native-base]. Direct launcher semantics were checked at [vnm_framework d9334145f489a5650fb79a5a7086d8b6618f0c6f][direct-host] and the [Qt v6.12.0 splitter][qt-splitter]. These are primary source observations, distinct from executing a native Grok or PowerShell binary.

The external toolchain was `logonomic-linux-development-sdk-2026.10.23.1`, archive SHA-256 `56100ede7b00bd7faf06a7b64c57b94106f63e3ba1b77c0bbdf5d61d7ca220c2`. The archive matched the required checksum. A task-owned writable copy passed `sdk-preflight`. The environment used GCC 13.3.0, C++20 Release builds, Ubuntu 24.04.3/glibc 2.39 and system libcurl 8.5.0. Configure operations used one queued compiler slot; focused builds used at most four slots with `--parallel 4`. An explicit persistent `CMAKE_CXX_COMPILER_LAUNCHER` enabled ccache: all 88 baseline compiler calls were cacheable; the later signal build recorded 59 hits and 28 misses across 87 cacheable calls.

## Launcher findings: 45, 66 and 135

### 45 — preserve the authored runner's argument bytes

The baseline used one quote-toggle tokenizer for direct, Bash and PowerShell launchers. Exact comparisons against the baseline library produced:

| Runner/input | Baseline argument value | Required value and evidence |
|---|---|---|
| Direct: `grok --rules """quoted"""` | `quoted` | `"quoted"`, including the literal quotes; direct host/SDK splitter contract |
| PowerShell: `grok --append-system-prompt 'Don''t guess'` | `Dont guess` | `Don't guess`; PowerShell source-contract expectation, not a native PowerShell run |
| Bash: `grok --rules "one\\two"` | Two backslashes between `one` and `two` | One backslash; native Bash comparison emitted hex `6f6e655c74776f` |

The correction belongs at the shared launcher grammar boundary. The [published SDK change][sdk-launcher-commit] exposes the reusable `literal_launcher_invocation` API, and the [provider consumer][provider-launcher-commit] routes Grok launch preparation, exact resume, delegated recognition and related account/probe recognition through it. Runner identity and available host `launcher_argv` evidence participate in admission. Environment assignment values come from the host's effective environment rather than reconstructed quote-stripped text. A launcher that cannot be represented faithfully keeps its existing unavailable/unmanaged outcome; supported literal shell forms remain accepted.

The direct host first validates quoting, then uses `QProcess::splitCommand`, whose whitespace classification includes Unicode. The SDK factors the existing splitter into strict host mode and the legacy public mode, retaining existing `literal_argv` behavior while enforcing the actual host contract in the new helper. Regressions cover host-rejected forms (`"unfinished`, `""`, and `part""value`) and valid commands separated by U+00A0/U+2003, as well as inconsistent host evidence and runner-specific literal values. All seven final provider CTests passed. The full maintained SDK launcher-approval group passed through the focused harness, and the existing launcher utilities CTest passed; the aggregate SDK test limitation is documented separately below. [Host validation and splitting][direct-host]; [Qt whitespace splitting][qt-splitter].

### 66 — avoid adding another standalone selector

The native [PagerArgs definition and parse entry point][native-cli] use a boolean `no_leader` with a conflict against `leader`, without a self-override or global `args_override_self` setting. [Cargo.lock][native-lock] selects clap/clap_builder 4.5.53; that version's [boolean/SetTrue contract][clap-settrue] rejects repetition unless overriding is enabled. This closes the historical source-contract gap; a real Grok invocation was still not performed.

The [published correction][option-commit] records `--no-leader` during the existing option traversal and adds it only when absent for ordinary preparation, initial-prompt preparation and exact resume. Traversal distinguishes an option from an option value and preserves authored argument bytes/order. It leaves authored duplicate flags for the native parser to judge; it does not normalize unrelated user settings. The test-only regression linked against unchanged baseline libraries exited with `Ordinary preparation must add --no-leader only when absent as an option`.

### 135 — align approval editing with verified options

The same commit adds `--reasoning-effort`, `--no-leader` and verified aliases `--worktree-ref`, `--allowedTools`, `--disallowedTools`, `--append-system-prompt` and `--system-prompt` to the appropriate approval tables. The [native declaration][native-cli] supports these spellings. The baseline regression failed with `Yolofy must recognize native options and preserve their values`. Candidate tests cover preservation of values, idempotent approval toggling and comparison keys that remain independent of approval state. The immutable option candidate passed both `grok_runtime_tests` and `grok_initial_prompt_tests`; independent review found no blocker in that scope.

## 137 — clean owned POSIX runtimes on catchable termination

On the baseline, SIGHUP, SIGINT and SIGTERM in both leader startup and resident operation bypassed the runtime destructor and left the per-launch directory. All six probes used the actual bridge with an inert executable, retained the expected terminating signal, and kept a separate live peer intact.

The [published signal change][signal-commit] installs scoped handlers only in the POSIX bridge. The handler records the first signal in `sig_atomic_t`; normal control flow observes it during startup, the bridge loop and bounded helper cancellation. Existing runtime cleanup settles direct children and removes owned endpoints/locks, after which original dispositions are restored and the original signal is re-raised. Inherited ignored dispositions remain ignored. Standalone fallback restores handlers before `exec`.

All six candidate probes removed their runtime directory and preserved the original terminating signal and simultaneous live peer. The committed [POSIX transport fixtures][posix-tests] also exercise direct-child settlement and inherited ignored SIGINT. The combined option/signal candidate passed runtime, wake-protocol, wake-bridge, attention-bridge and POSIX-transport CTests. Independent review found no blocker in that tested scope. No polling interval or Windows wait behavior changed.

This is catchable-signal cleanup, not a terminal-close integration claim. SIGKILL/crashes still bypass normal cleanup; the outer terminal/PTY owner retains responsibility for descendants beyond the bridge's direct children. No recursive stale-directory sweep or cross-session cleanup was introduced.

## 67 — a small valid native-shaped record closes the protocol

The reproduced input is 259 ASCII/UTF-8 bytes, excluding a trailing newline:

```json
{"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"original-session","update":{"sessionUpdate":"tool_call","toolCallId":"call-unsigned","title":"mcp__catalog__lookup","kind":"other","status":"pending","rawInput":{"value":9223372036854775808}}}}
```

This avoids relying on speculative large/deep payloads. The native [tool-call producer][native-raw-input] parses model arguments as `serde_json::Value` and puts that value into `rawInput` before validation/coercion. The [update forwarding code][native-updates] wraps the update in a session notification; the [leader broadcast path][native-broadcast] sends ordinary session notifications to subscribers. The native lockfile uses serde_json 1.0.149, whose [number representation admits positive `u64`][serde-number]. In contrast, the SDK's [integer parser][sdk-json] parses integral lexemes into `int64_t` and rejects this overflow. The record is small and shallow, so frame/depth/member limits do not explain this failure.

### Minimal executed procedure and result

Construct `grok::Wake_protocol`, call `initialize()`, and feed this matching successful response through `receive()`:

```json
{"jsonrpc":"2.0","id":"logonomic-wake-initialize","result":{"protocolVersion":1,"_meta":{"agentInstanceId":"original-leader"}}}
```

Check `initialized() == true`, feed the 259-byte record, record `initialized()` and `failure_reason()`, then feed the same valid initialization response again. A small executable linked to the unchanged provider and SDK production libraries returned:

```json
{"fixture_bytes":259,"initialized_before":true,"closed_after":true,"failure_reason":"native_record_invalid","reinitialized_after_valid_record":false}
```

The [receive failure branch][protocol-reject] calls `fail("native_record_invalid")`; [fail][protocol-close] marks the protocol closed and subsequent records cannot reopen it. The executed result therefore establishes an exact component defect and the inspected native source establishes a plausible producer/broadcast route. It does **not** establish an observed real Grok emission, its frequency, or an integrated user-interface symptom. No real Grok/model was run and no prompt was pending in this minimal reproduction.

### Scoped follow-up; parser unchanged in this work

Design and qualify a bounded native JSON representation or envelope projection at the appropriate owner boundary, capable of handling valid native data outside the stricter internal SDK value domain, including fields this protocol does not consume. No parser design or owning module has been adopted. Preserve the existing 1 MiB native record bound, envelope and RPC-ID validation, exact session/transition/receipt correlation, and truthful failure of malformed correlated initialize/prompt replies. Use the small unsigned-integer broadcast as a regression, alongside malformed correlated replies, invalid envelopes, unknown notifications and already-written prompt uncertainty cases.

The follow-up must not globally relax SDK host contracts, ignore every parse failure, or reconnect/replay an uncertain prompt merely to hide a failed connection. Source proof and this reproduction justify targeted work; they do not validate a particular replacement parser. Production JSON/protocol behavior is unchanged in this PR's investigation result.

## 136 — bounded idle CPU and complete transport latency measurement

Both the unchanged baseline bridge and the independently reviewed signal candidate were measured with their existing [10 ms bridge wait][bridge-wait] and [client transport wait][client-wait]. The measurement used the real bridge and `grok::exchange_wake_request`, with inert Python peers implementing initialize, resident session-list and prompt queue acknowledgment. There was no real Grok/model, artificial load, or compiler activity during the intervals.

### Method and timing boundary

For each binary, run two trials, alternating baseline/candidate. Each trial owns a fresh 32-hex runtime token, session UUID, transition nonce, working directory and child processes. Send a `SessionStart` observation, verify resident availability through `inspect`, perform one warm-up notification and settle for 100 ms. Record a 10-second idle interval with no native control messages, followed by 40 distinct notifications, staggered by a deterministic random delay from 0–17 ms. Use seeds 1000 and 1001 for the respective trials.

The client timestamps entry to and return from the production `exchange_wake_request` call with `steady_clock`; its cancellation/wait callback sleeps for the requested interval and returns false. The native fixture timestamps immediately before emitting `_x.ai/queue/changed` with the same `sessionId` and `runningPromptId`, then returns `stopReason: end_turn`. Each accepted receipt must equal the submitted notification ID. This includes the bridge's resident checks, prompt dispatch, queue acknowledgment and complete returned receipt. It excludes host adapter dispatch, real model processing and rendering. Native fixture logging and Python scheduling remain inside the measured path.

For the bridge process alone, sample `/proc/<pid>/stat` user/system ticks, `/proc/<pid>/status` voluntary/involuntary context switches, the process CPU clock and scheduler runtime before/after idle. The system exposed 100 clock ticks/s, so user/system deltas are quantized to 10 ms; the separate process CPU clock provides the finer total used for CPU percentages. The monotonic clock advertised 1 ns resolution, which is not a guarantee of timing accuracy. Host 1-minute load ranged from about 0.126–0.299; affinity allowed nine logical CPUs and the cgroup quota was eight CPU equivalents.

### Idle measurements

CPU percentage is relative to one fully occupied core. Totals combine two 10-second intervals per binary; percentage ranges show the two individual trials.

| Measurement | Baseline | Signal candidate |
|---|---:|---:|
| Elapsed idle time | 20.0020 s | 20.0020 s |
| `/proc` user / system CPU | 0.010 / 0.070 s | 0.020 / 0.030 s |
| Fine process CPU total | 0.088183 s | 0.072239 s |
| Weighted CPU / one core | 0.441% | 0.361% |
| Individual trial CPU range | 0.416–0.465% | 0.353–0.369% |
| Voluntary context switches | 1,975; 98.74/s | 1,978; 98.89/s |
| Involuntary context switches | 0 | 0 |

The coarse user/system counters and fine CPU clock need not sum to the same decimal total. These observations establish nonzero idle work under the measured conditions. Approximately 99 voluntary context switches/s does not establish exactly 100 OS wakeups/s.

### Wake latency distribution

Each row contains 80 samples. Values are milliseconds; p95 uses nearest rank. “Native ack” denotes the fixture's queue-notification emission timestamp.

| Binary / interval | Minimum | Median | Mean | p95 | Maximum |
|---|---:|---:|---:|---:|---:|
| Baseline: submit → native ack | 20.796 | 25.908 | 25.994 | 30.243 | 31.325 |
| Baseline: native ack → receipt | 8.339 | 14.626 | 14.624 | 19.434 | 19.866 |
| Baseline: **submit → receipt** | **31.576** | **40.434** | **40.618** | **40.792** | **50.519** |
| Signal candidate: submit → native ack | 20.601 | 25.875 | 25.979 | 30.471 | 31.075 |
| Signal candidate: native ack → receipt | 9.378 | 15.071 | 15.124 | 19.723 | 20.037 |
| Signal candidate: **submit → receipt** | **40.366** | **40.466** | **41.103** | **45.597** | **50.697** |

All 160 sampled notification IDs returned their exact accepted receipts. Four replay checks returned the prior receipt without another native prompt. Session/transition identity remained intact, and all four runtime directories disappeared on owned TUI exit. Only fixture-owned processes and paths were cleaned.

The binary identities were SHA-256 `0e42db422922f5005131be969dea65b17ea1c7c5990495320e6e448cddadf7a8` (baseline) and `f35c130ba4d2a15325df923a8ae427d3932ee820937dfaa40fb82ee1c926009f` (signal candidate). These short trials provide no confidence interval, battery result or many-terminal extrapolation. Their difference does not establish a performance improvement from signal handling. Polling remains unchanged; an event-driven redesign would need its own deadline, cancellation, process-exit and teardown validation, including the relevant native platforms.

## Validation and reproducibility

The credited gates were focused, serial/headless tests. No broad suite, private hosted CI or live UI run is claimed.

| Check | Actual result |
|---|---|
| Toolchain checksum and writable-copy preflight | Checksum matched; preflight passed |
| Baseline `grok_runtime_tests`, `grok_wake_protocol_tests`, `grok_wake_bridge_tests`, `grok_attention_bridge_tests` | Four executed passes in the default sandbox |
| Baseline `grok_initial_prompt_tests` | Initial read-only HOME failure; targeted rerun with disposable writable HOME passed |
| Baseline `grok_wake_transport_posix_tests` | Initial local socket permission failure; targeted permitted-AF_UNIX rerun passed |
| #66 and #135 new tests against unchanged baseline libraries | Both failed on the intended assertions before the correction |
| Immutable option candidate | Runtime and initial-prompt CTests: 2 executed, 0 skipped, 0 failed; independently reviewed |
| Immutable option + signal candidate | Runtime, wake-protocol, wake-bridge, attention-bridge and POSIX-transport CTests: 5 executed, 0 skipped, 0 failed; independently reviewed |
| Signal reproduction | Six baseline leaks reproduced; six candidate cleanup probes passed |
| #45 baseline diagnostics | Three exact argument-byte differences reproduced against unchanged production libraries |
| #45 final provider gates | `grok_runtime_tests`, `grok_initial_prompt_tests`, `grok_probe_tests`, `grok_wake_protocol_tests`, `grok_wake_bridge_tests`, `grok_attention_bridge_tests` and `grok_wake_transport_posix_tests`: all 7 passed |
| #45 maintained combined SDK test target | `logonomic_cli_addon_sdk_tests` build failed at unchanged `tests/quota_failure_compatibility_tests.cpp:77` under GCC 13 with `-Werror=missing-field-initializers`; SDK library and launcher test translation unit compiled; no aggregate-target pass credited |
| #45 SDK `launcher_approval_contracts` group | **PASS**: full checked-in group executed through a standalone harness compiling exact `tests/launcher_approval_tests.cpp` and `tests/sdk_test_support.cpp` with unchanged `-Wall -Wextra -Werror` |
| #45 normal SDK launcher utilities CTest | `logonomic_cli_addon_launcher_utilities_tests`: **PASS, 3.07 s**, one executed registered CTest; distinct from the standalone contracts harness |
| #67 reproduction | Production component result reproduced exactly as printed above; no parser fix credited |
| #136 measurement | Four bounded trials, 160 sampled wakes, four receipt replays, all owned runtimes removed |

The original SDK copy belonged to another session and was read-only; copying it into the task's writable tooling directory resolved preflight without modifying the original. Optional SQL-driver and QtQuick3D SpatialAudio dependencies were absent; the focused provider/SDK code is non-Qt C++ and did not require them. There was no missing required dependency after setup.

The initial prompt fixture tried to create `/root/grok-strict-<nonce>/home`, which was outside the writable sandbox. The POSIX fixture's `socket(AF_UNIX)` returned `EPERM`; a small socket probe confirmed the environmental cause. Only the affected tests were rerun with the necessary writable HOME/local-socket permission. These original failures are recorded as failures, not counted as passes. The signal harness also initially inherited ignored SIGINT: positive signal probes were corrected to set default dispositions and unblock HUP/INT/TERM, while a separate product fixture verifies inherited ignore preservation.

The SDK validation has a separate build limitation: the maintained combined test executable failed in [quota-compatibility test code][quota-test] under the required warning policy. The failing source and [public quota schema][quota-schema] are byte-identical to the captured baseline; **no whole baseline aggregate SDK build was run or is claimed**. The test source, schema and warning policy were left unchanged. The focused alternative compiled and executed the entire checked-in `launcher_approval_contracts` group using its original test/support sources and warning flags and passed. The existing launcher utilities CTest also passed. This is one standalone group execution and one registered CTest, not a passing aggregate SDK target or two SDK CTests.

To reproduce the focused build, activate the verified external SDK and choose writable `HOME` and persistent `CCACHE_DIR`. Set `PROVIDER_SOURCE` to the selected provider checkout, `SDK_SOURCE` to its matching `sdk/logonomic_cli_addon_sdk` subtree, and `BUILD_DIR` to an isolated build directory:

```bash
queued-build --slots 1 -- cmake -S "$PROVIDER_SOURCE" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER_LAUNCHER="$SDK_ROOT/bin/ccache" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DLOGONOMIC_CLI_ADDON_SDK_SOURCE="$SDK_SOURCE"

queued-build --slots 4 -- cmake --build "$BUILD_DIR" --parallel 4 --target \
  grok_runtime_tests grok_initial_prompt_tests grok_probe_tests grok_wake_protocol_tests \
  grok_wake_bridge_tests grok_attention_bridge_tests grok_wake_transport_posix_tests

sdk-run-headless ctest --test-dir "$BUILD_DIR" --output-on-failure \
  --no-tests=error --parallel 1 \
  -R '^grok_(runtime|initial_prompt|probe|wake_protocol|wake_bridge|attention_bridge|wake_transport_posix)_tests$'
```

The POSIX tests require a runner that permits local Unix sockets and child processes. The commands list the seven final provider gates; the original six-test baseline omitted `grok_probe_tests`. For that baseline, use the recorded baseline provider/SDK revisions; for the final #45 consumer, use the finalized prerequisite SDK revision. Do not substitute an evolving working tree for the source snapshot whose result is being reported.

### Reproducing the focused SDK launcher checks

Use the published SDK commit above and isolated absolute paths for `SDK_BUILD_DIR`, `HARNESS_SOURCE` and `HARNESS_BUILD_DIR`. `SDK_SOURCE` denotes the `sdk/logonomic_cli_addon_sdk` subtree. Put the following two files in `HARNESS_SOURCE`; they are the complete harness used for the passing group execution. It calls the maintained group without copying, modifying or omitting its cases.

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.25)
project(p19_launcher_sdk_group LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
set(P19_SDK_SOURCE "" CACHE PATH "Frozen SDK source")
set(P19_SDK_LIBRARY "" CACHE FILEPATH "Built SDK library")
if(NOT P19_SDK_SOURCE OR NOT P19_SDK_LIBRARY)
    message(FATAL_ERROR "Frozen SDK source and built SDK library are required")
endif()
add_executable(p19_launcher_approval_group
    main.cpp
    "${P19_SDK_SOURCE}/tests/launcher_approval_tests.cpp"
    "${P19_SDK_SOURCE}/tests/sdk_test_support.cpp")
target_compile_options(p19_launcher_approval_group PRIVATE -Wall -Wextra -Werror)
target_include_directories(p19_launcher_approval_group PRIVATE
    "${P19_SDK_SOURCE}/include" "${P19_SDK_SOURCE}/src" "${P19_SDK_SOURCE}/tests")
target_link_libraries(p19_launcher_approval_group PRIVATE "${P19_SDK_LIBRARY}" Threads::Threads)
```

`main.cpp`:

```cpp
#include "sdk_test_support.h"
#include <iostream>

int main()
{
    const bool passed = sdk_tests::test_launcher_approval_contracts();
    std::cout << "launcher_approval_contracts: " << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? 0 : 1;
}
```

With the same SDK activation, writable HOME and persistent ccache as the provider build:

```bash
queued-build --slots 1 -- cmake -S "$SDK_SOURCE" -B "$SDK_BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER_LAUNCHER="$SDK_ROOT/bin/ccache" \
  -DLOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS=ON

queued-build --slots 4 -- cmake --build "$SDK_BUILD_DIR" --parallel 4 --target \
  logonomic_cli_addon_sdk logonomic_cli_addon_launcher_utilities_tests

sdk-run-headless ctest --test-dir "$SDK_BUILD_DIR" --output-on-failure \
  --no-tests=error --parallel 1 \
  -R '^logonomic_cli_addon_launcher_utilities_tests$'

queued-build --slots 1 -- cmake -S "$HARNESS_SOURCE" -B "$HARNESS_BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER_LAUNCHER="$SDK_ROOT/bin/ccache" \
  -DP19_SDK_SOURCE="$SDK_SOURCE" \
  -DP19_SDK_LIBRARY="$SDK_BUILD_DIR/liblogonomic_cli_addon_sdk.a"

queued-build --slots 4 -- cmake --build "$HARNESS_BUILD_DIR" --parallel 4 \
  --target p19_launcher_approval_group

"$HARNESS_BUILD_DIR/p19_launcher_approval_group"
```

The recorded group result was `launcher_approval_contracts: PASS`; the registered utilities CTest passed in 3.07 s. These commands reproduce those focused checks. They do not establish a successful build of the separate aggregate target `logonomic_cli_addon_sdk_tests`, whose observed compilation failure remains recorded above.

## Native and unavailable follow-up

| Gate | What remains to establish |
|---|---|
| Native PowerShell authored launchers | Execute quoting/escaping cases on supported PowerShell; Linux source-contract fixtures are not native execution |
| Native Windows transport | Confirm platform deadline/exit/cancellation behavior if a future change touches Windows waits; those waits were unchanged here |
| Native macOS POSIX signal behavior | Run catchable-signal and child-settlement cases on Darwin before claiming native coverage |
| Real Grok broadcast and wake integration | Exercise actual native emission/acknowledgment, especially #67; the current evidence is production components plus source-supported inert peers |
| Full terminal/PTY owner integration | Confirm terminal-close descendant custody across the bridge boundary; the inert tests cover bridge-owned direct children |
| Power or many-terminal impact | Measure on representative hardware/workload before claiming material battery or aggregate performance harm |

The next action is native PowerShell follow-up and review of the remaining platform coverage before marking either draft ready. Independent review found no outstanding correctness blocker in the tested Linux scope, with the aggregate SDK build limitation retained. Integration must land the SDK prerequisite before the provider consumer; neither PR is merged. The #67 follow-up remains a separately scoped correctness task with its parser/representation design and owner boundary still open. The measured #136 result does not justify a speculative polling change.

[provider-pr]: https://github.com/Varinomics/vnm_logonomic_cli_grok/pull/8
[sdk-pr]: https://github.com/Varinomics/logonomic/pull/173
[sdk-launcher-commit]: https://github.com/Varinomics/logonomic/commit/0e9aa901be11629a8febc179745d268d85595fb0
[provider-launcher-commit]: https://github.com/Varinomics/vnm_logonomic_cli_grok/commit/d80d2d07cd8a6537ad42f00de8452279ecef4928
[sdk-reconciled-master]: https://github.com/Varinomics/logonomic/commit/a87018b7d67dc7ecda8983049d19b6edfae199a8
[quota-test]: https://github.com/Varinomics/logonomic/blob/0e9aa901be11629a8febc179745d268d85595fb0/sdk/logonomic_cli_addon_sdk/tests/quota_failure_compatibility_tests.cpp#L77
[quota-schema]: https://github.com/Varinomics/logonomic/blob/0e9aa901be11629a8febc179745d268d85595fb0/sdk/logonomic_cli_addon_sdk/include/logonomic_cli_addon/quota.h
[provider-base]: https://github.com/Varinomics/vnm_logonomic_cli_grok/tree/c4d9145bd8fd19b7c522b22bc4f83675e2463f53
[option-commit]: https://github.com/Varinomics/vnm_logonomic_cli_grok/commit/808ba80308361e439c89a5d3c5dfcd7b32452263
[signal-commit]: https://github.com/Varinomics/vnm_logonomic_cli_grok/commit/644eb32d2ef7193eb48f858a15fa848b3c4246bf
[sdk-base]: https://github.com/Varinomics/logonomic/tree/b9e44f6fe131624d48b0338c23295dae67d526bd/sdk/logonomic_cli_addon_sdk
[native-base]: https://github.com/xai-org/grok-build/tree/2bdd1d6a6369de0e8c68132ea4539e9abd9e14a8
[native-cli]: https://github.com/xai-org/grok-build/blob/2bdd1d6a6369de0e8c68132ea4539e9abd9e14a8/crates/codegen/xai-grok-pager/src/app/cli.rs
[native-lock]: https://github.com/xai-org/grok-build/blob/2bdd1d6a6369de0e8c68132ea4539e9abd9e14a8/Cargo.lock
[clap-settrue]: https://docs.rs/clap_builder/4.5.53/clap_builder/builder/enum.ArgAction.html#variant.SetTrue
[direct-host]: https://github.com/Varinomics/vnm_framework/blob/d9334145f489a5650fb79a5a7086d8b6618f0c6f/cpp/core/vnm_launcher_command.cpp#L10-L94
[qt-splitter]: https://github.com/qt/qtbase/blob/v6.12.0/src/corelib/io/qprocess.cpp#L2510-L2547
[posix-tests]: https://github.com/Varinomics/vnm_logonomic_cli_grok/blob/644eb32d2ef7193eb48f858a15fa848b3c4246bf/tests/grok_wake_transport_posix_tests.cpp
[native-raw-input]: https://github.com/xai-org/grok-build/blob/2bdd1d6a6369de0e8c68132ea4539e9abd9e14a8/crates/codegen/xai-grok-shell/src/session/acp_session_impl/tool_calls.rs#L1468-L1495
[native-updates]: https://github.com/xai-org/grok-build/blob/2bdd1d6a6369de0e8c68132ea4539e9abd9e14a8/crates/codegen/xai-grok-shell/src/session/acp_session_impl/updates.rs#L249-L324
[native-broadcast]: https://github.com/xai-org/grok-build/blob/2bdd1d6a6369de0e8c68132ea4539e9abd9e14a8/crates/codegen/xai-grok-shell/src/leader/server.rs#L2196-L2224
[serde-number]: https://github.com/serde-rs/json/blob/v1.0.149/src/number.rs#L26-L33
[sdk-json]: https://github.com/Varinomics/logonomic/blob/b9e44f6fe131624d48b0338c23295dae67d526bd/sdk/logonomic_cli_addon_sdk/src/json.cpp#L579-L588
[protocol-reject]: https://github.com/Varinomics/vnm_logonomic_cli_grok/blob/c4d9145bd8fd19b7c522b22bc4f83675e2463f53/src/grok_wake_protocol.cpp#L310-L320
[protocol-close]: https://github.com/Varinomics/vnm_logonomic_cli_grok/blob/c4d9145bd8fd19b7c522b22bc4f83675e2463f53/src/grok_wake_protocol.cpp#L192-L209
[bridge-wait]: https://github.com/Varinomics/vnm_logonomic_cli_grok/blob/c4d9145bd8fd19b7c522b22bc4f83675e2463f53/src/grok_session_bridge.cpp#L181
[client-wait]: https://github.com/Varinomics/vnm_logonomic_cli_grok/blob/c4d9145bd8fd19b7c522b22bc4f83675e2463f53/src/grok_wake_transport_posix.cpp#L547
