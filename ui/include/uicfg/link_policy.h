#pragma once
#include <string>
#include <string_view>
namespace uicfg {
// Design doc 14.3: OSC 8 links never auto-open; on click show the real URL;
// only http/https allowed; file, shell, mailto and custom schemes rejected (no override).
enum class LinkDecision { Allow, RejectScheme, RejectMalformed };
struct LinkCheck { LinkDecision decision; std::string scheme; std::string displayUrl; };
LinkCheck checkLink(std::string_view url);
}
