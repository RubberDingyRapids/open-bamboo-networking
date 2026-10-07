#include "obn/extra_accounts.hpp"

#include "obn/json_lite.hpp"

#include <fstream>
#include <iterator>
#include <set>

namespace obn::accounts {

std::vector<ExtraAccount> parse(const std::string& text, std::string* err)
{
    std::vector<ExtraAccount> out;
    if (text.empty()) return out;
    std::string perr;
    auto root = obn::json::parse(text, &perr);
    if (!root) {
        if (err) *err = perr;
        return out;
    }
    std::set<std::string> seen;
    auto list_v = root->find("accounts");
    for (const auto& entry : list_v.as_array()) {
        if (!entry.is_object()) continue;
        ExtraAccount a;
        a.session = obn::auth::session_from_json(entry);
        if (!a.session.logged_in()) continue;
        if (!seen.insert(a.session.user_id).second) continue;
        a.label   = entry.find("label").as_string();
        auto en   = entry.find("enabled");
        a.enabled = en.is_bool() ? en.as_bool() : true;
        out.push_back(std::move(a));
    }
    return out;
}

std::vector<ExtraAccount> load(const std::string& path, std::string* err)
{
    if (path.empty()) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) return {};
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    return parse(text, err);
}

std::string labelled_name(const std::string& label, const std::string& name)
{
    if (label.empty()) return name;
    return "[" + label + "] " + name;
}

} // namespace obn::accounts
