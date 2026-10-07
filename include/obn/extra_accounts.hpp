#pragma once

// Extra Bambu Cloud accounts that run next to the logged-in one.
//
// A companion Orca plugin lists them in `<config_dir>/obn.accounts.json`
// and owns that file (login, token refresh); the library only reads it and
// re-reads it on obn_extra_accounts_reload(). Entries use obn.auth.json's
// keys plus `enabled` and `label`:
//
//   {"version":1,"accounts":[{"enabled":true,"label":"Work",
//     "account":"me@work.example","access_token":"...","refresh_token":"...",
//     "expires_at":"2027-01-01T00:00:00Z","user_id":"123","user_name":"..."}]}
//
// All accounts share the slicer's cloud region. See agent_accounts.cpp for
// how the agent uses them.

#include "obn/auth.hpp"

#include <string>
#include <vector>

namespace obn::accounts {

struct ExtraAccount {
    obn::auth::Session session;
    std::string        label;          // put in front of printer names; may be empty
    bool               enabled = true;
};

// Parses the file body. Entries without user_id or access_token are skipped,
// as are repeats of a user_id. On a parse error returns an empty list and
// fills `err`.
std::vector<ExtraAccount> parse(const std::string& text, std::string* err = nullptr);

// Reads and parses `path`. A missing file is not an error: empty list.
std::vector<ExtraAccount> load(const std::string& path, std::string* err = nullptr);

// "[label] name", or `name` unchanged when the label is empty.
std::string labelled_name(const std::string& label, const std::string& name);

} // namespace obn::accounts
