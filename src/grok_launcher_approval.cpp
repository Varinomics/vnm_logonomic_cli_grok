#include "grok_launcher_approval.h"

namespace grok {

const logonomic::cli_addon::Launcher_approval_rules& launcher_approval_rules()
{
    static const logonomic::cli_addon::Launcher_approval_rules rules{
        .program_names = {"grok", "grok.exe", "grok.cmd", "grok.bat", "grok.ps1"},
        .canonical_argument = "--always-approve",
        .bypass_arguments = {"--always-approve", "--yolo", "--dangerously-skip-permissions"},
        .approval_modes = {
            {"--permission-mode", {"bypassPermissions"}, {"default", "acceptEdits", "plan", "auto", "dontAsk"}},
        },
        .conflicting_arguments = {},
        .value_arguments = {
            "--cwd", "-s", "--session-id", "--ref", "--worktree-ref", "-m", "--model",
            "--effort", "--reasoning-effort", "--allow", "--allowedTools", "--deny", "--disallowedTools",
            "--sandbox", "--rules", "--append-system-prompt", "--system-prompt-override", "--system-prompt", "--tools",
            "--disallowed-tools", "--max-turns", "-p", "--single", "--output-format",
        },
        .optional_value_arguments = {"-r", "--resume", "-w", "--worktree"},
        .flag_arguments = {
            "-c", "--continue", "--fork-session", "--no-plan", "--no-subagents", "--no-memory",
            "--disable-web-search", "--experimental-memory", "--oauth", "--no-alt-screen",
            "--no-auto-update", "--no-leader",
        },
        .session_commands = {},
        .unsupported_commands = {
            "login", "logout", "inspect", "models", "mcp", "plugin", "sessions", "export",
            "import", "memory", "worktree", "dashboard", "agent", "wrap", "update", "version",
            "completions", "setup", "help",
        },
    };
    return rules;
}

} // namespace grok
