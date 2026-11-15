#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/helper_ingress.h>
#include <vnm_filesystem_observation.h>

#include <memory>
#include <optional>

namespace grok {

// A native hook admits a parent journal frontier; only subsequent native
// attempt records can change the child timeline of its authenticated root.
class Lifecycle_observer
{
public:
    Lifecycle_observer();
    Lifecycle_observer(
        std::shared_ptr<vnm::filesystem_observation::Service> service,
        vnm::filesystem_observation::Configuration configuration);
    ~Lifecycle_observer();

    Lifecycle_observer(const Lifecycle_observer&)            = delete;
    Lifecycle_observer& operator=(const Lifecycle_observer&) = delete;

    void handle(logonomic::cli_addon::Invocation_context& context);
    std::optional<logonomic::cli_addon::Classify_event_result> classify(
        const logonomic::cli_addon::Classify_event_request& request,
        logonomic::cli_addon::Contract_failure* failure = nullptr);

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace grok
