#include <logonomic_cli_addon/json.h>
#include <cstdio>
#include <string>
int main(int argc, char** argv)
{
    namespace addon = logonomic::cli_addon;
    // Lifecycle is deliberately unavailable in this fixture. A title-capable
    // native gate must not depend on this unrelated lane completing.
    if (argc != 3 || std::string(argv[1]) != "request-title-instruction" ||
        std::string(argv[2]) != "--args-stdin") return 4;
    std::string input;
    for (int ch; (ch = std::fgetc(stdin)) != EOF;) {
        if (input.size() > 16384) return 2;
        input += static_cast<char>(ch);
    }
    const auto value = addon::parse_json(input, {}, nullptr);
    if (!value || !value->find("source") || *value->find("source") != addon::Json_value("grok") ||
        !value->find("hook_event_name") || *value->find("hook_event_name") != addon::Json_value("PreToolUse") ||
        !value->find("delivery") || !value->find("delivery")->find("kind") ||
        *value->find("delivery")->find("kind") != addon::Json_value("model-context")) return 2;
    std::fputs("{\"accepted\":true,\"instruction_required\":true,\"instruction\":\"Set the title using the authenticated host instruction.\"}\n", stdout);
    return std::fflush(stdout) == 0 ? 0 : 1;
}
