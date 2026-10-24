// Secrets kept on this computer only: sealed by Windows (DPAPI) to the user's account, so a copied hub_state.toml
// carries nothing usable to another computer or user.
#pragma once
#include <string>

namespace launcher {

std::string Seal(const std::string& plain);     // base64; empty when it cannot
std::string Unseal(const std::string& sealed);  // empty when it cannot

}  // namespace launcher
