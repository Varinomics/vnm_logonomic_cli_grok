#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace grok {

struct Billing_http_response
{
    long        status = 0;
    std::string body;
};

// Sends only to Grok's fixed CLI proxy origin. Credentials remain in process
// memory and HTTPS request headers; neither the URL nor a child argv carries them.
std::optional<Billing_http_response> get_billing_resource(
    std::string_view resource, std::string_view bearer,
    std::string_view user_id, std::string_view client_version);

} // namespace grok
