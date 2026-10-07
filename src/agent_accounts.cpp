// Extra cloud accounts: one CloudSession per account in obn.accounts.json,
// next to the primary session the slicer logged in. See extra_accounts.hpp.
//
// The slicer still sees one account. get_user_print_info (abi_http.cpp)
// appends the extra accounts' printers to the primary's list and records
// which account each came from in extra_dev_owner_; publish, subscribe,
// cloud print, camera and rename/unbind then pick the owner's connection
// and token through cloud_session_for_locked_() / session_for_device().

#include "obn/agent.hpp"

#include "obn/abi_export.hpp"
#include "obn/bambu_networking.hpp"
#include "obn/cloud_ca_bundle.hpp"
#include "obn/cloud_session.hpp"
#include "obn/config.hpp"
#include "obn/json_lite.hpp"
#include "obn/log.hpp"

namespace obn {

std::vector<std::shared_ptr<CloudSession>> Agent::load_extra_accounts_()
{
    const std::string path = obn::config::path_in_dir("obn.accounts.json");
    std::string err;
    auto list = obn::accounts::load(path, &err);
    if (!err.empty()) {
        OBN_WARN("accounts: %s is not valid JSON (%s); keeping the current set",
                 path.c_str(), err.c_str());
        return {};
    }
    const std::string primary = cloud_user_id();

    // Every reload starts over: the old sessions go back to the caller to
    // stop, start_extra_sessions_ opens fresh ones. Reloads are rare (the
    // plugin adds, removes or refreshes an account), so a reconnect is fine.
    std::vector<std::shared_ptr<CloudSession>> old;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [uid, ec] : extra_clouds_)
        if (ec.session) old.push_back(std::move(ec.session));
    extra_clouds_.clear();
    for (auto& a : list) {
        if (a.session.user_id == primary) continue;
        const std::string uid = a.session.user_id;   // read before the move
        extra_clouds_[uid] = {std::move(a), nullptr};
    }
    // Fresh sessions must bootstrap their printers again.
    for (const auto& [dev, uid] : extra_dev_owner_) cloud_kickstarted_devs_.erase(dev);
    OBN_INFO("accounts: %zu extra account(s) in %s", extra_clouds_.size(), path.c_str());
    return old;
}

void Agent::start_extra_sessions_()
{
    if (obn::config::current().block_cloud) return;
    const std::string region = cloud_region();
    std::string ca;
#if defined(_WIN32)
    ca = obn::tls::ensure_cloud_ca_bundle_file(config_dir());
#endif

    std::vector<std::pair<std::string, std::shared_ptr<CloudSession>>> fresh;
    std::map<std::string, std::vector<std::string>> devs_by_uid;
    std::function<void(std::string, std::string)> on_msg;
    std::function<void(std::string)>              on_sub_fail;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // Extra accounts only run alongside the primary connection, so they
        // come and go with the slicer's own login.
        if (!cloud_session_ || !cloud_session_->is_started() || !cloud_msg_cb_) return;
        on_msg      = cloud_msg_cb_;
        on_sub_fail = cloud_sub_fail_cb_;
        for (auto& [uid, ec] : extra_clouds_) {
            if (!ec.account.enabled || ec.session) continue;
            ec.session = std::make_shared<CloudSession>();
            ec.session->configure(region, uid, ec.account.session.access_token, ca);
            fresh.emplace_back(uid, ec.session);
        }
        for (const auto& [dev, uid] : extra_dev_owner_) devs_by_uid[uid].push_back(dev);
    }

    for (auto& [uid, sess] : fresh) {
        auto on_connected = [this, uid = uid](int status, int reason, std::string /*msg*/) {
            OBN_INFO("cloud: extra account uid=%s status=%d reason=%d", uid.c_str(), status, reason);
            // The slicer's "server connected" state tracks the primary only.
            if (status == 0) { kickstart_cloud_status(); return; }
            std::lock_guard<std::mutex> lk(mu_);
            for (const auto& [dev, owner] : extra_dev_owner_)
                if (owner == uid) cloud_kickstarted_devs_.erase(dev);
        };
        if (int rc = sess->start(on_connected, on_msg, on_sub_fail); rc != BAMBU_NETWORK_SUCCESS) {
            OBN_WARN("cloud: extra account uid=%s failed to start rc=%d", uid.c_str(), rc);
            std::lock_guard<std::mutex> lk(mu_);
            if (auto it = extra_clouds_.find(uid); it != extra_clouds_.end() && it->second.session == sess)
                it->second.session.reset();
            continue;
        }
        OBN_INFO("cloud: extra account uid=%s connecting", uid.c_str());
        if (auto it = devs_by_uid.find(uid); it != devs_by_uid.end()) sess->add_subscribe(it->second);
    }
}

std::shared_ptr<CloudSession> Agent::cloud_session_for_locked_(const std::string& dev_id) const
{
    auto owner = extra_dev_owner_.find(dev_id);
    if (owner == extra_dev_owner_.end()) return cloud_session_;
    auto ec = extra_clouds_.find(owner->second);
    // Never fall back to the primary: the broker refuses another account's
    // device topics, and a refused subscribe drops the whole connection.
    return ec != extra_clouds_.end() ? ec->second.session : nullptr;
}

int Agent::reload_extra_accounts()
{
    if (config_dir().empty()) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    for (auto& old : load_extra_accounts_()) old->stop();
    start_extra_sessions_();
    request_device_list_refresh_();
    return static_cast<int>(extra_accounts().size());
}

std::vector<obn::accounts::ExtraAccount> Agent::extra_accounts() const
{
    std::vector<obn::accounts::ExtraAccount> out;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [uid, ec] : extra_clouds_)
        if (ec.account.enabled) out.push_back(ec.account);
    return out;
}

void Agent::set_extra_device_owners(std::map<std::string, std::string> owners)
{
    std::lock_guard<std::mutex> lk(mu_);
    extra_dev_owner_.swap(owners);
}

obn::auth::Session Agent::session_for_device(const std::string& dev_id) const
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (auto owner = extra_dev_owner_.find(dev_id); owner != extra_dev_owner_.end())
            if (auto ec = extra_clouds_.find(owner->second); ec != extra_clouds_.end())
                return ec->second.account.session;
    }
    return user_session_snapshot();
}

std::map<std::string, std::string>
Agent::cloud_api_http_headers_for(const std::string& dev_id) const
{
    auto h = cloud_api_http_headers();
    const auto s = session_for_device(dev_id);
    if (!s.access_token.empty()) h["Authorization"] = "Bearer " + s.access_token;
    return h;
}

std::string Agent::extra_accounts_status_json() const
{
    std::string out = "{\"api\":1,\"accounts\":[";
    std::lock_guard<std::mutex> lk(mu_);
    bool first = true;
    for (const auto& [uid, ec] : extra_clouds_) {
        if (!first) out += ',';
        first = false;
        out += "{\"user_id\":" + obn::json::escape(uid);
        out += std::string(",\"started\":") + (ec.session && ec.session->is_started() ? "true" : "false");
        out += std::string(",\"connected\":") + (ec.session && ec.session->is_connected() ? "true" : "false");
        out += '}';
    }
    return out + "]}";
}

void Agent::request_device_list_refresh_()
{
    // Stock slicers have no "device list changed" callback. The closest is
    // the user-topic bind notice: UserManager::parse_json answers
    // {"bind":{"command":"bind","result":"success","dev_id":X}} with
    // update_user_machine_list_info() and then set_selected_machine(X).
    // Naming the printer that is already selected makes the second step a
    // no-op. With no selection we stay quiet; the slicer picks the new list
    // up on its next fetch (print dialog, Device tab, restart).
    BBL::OnMessageFn fn;
    std::string      selected;
    {
        std::lock_guard<std::mutex> lk(mu_);
        fn       = on_user_message_;
        selected = !user_selected_machine_.empty() ? user_selected_machine_ : remembered_machine_;
    }
    const std::string uid = cloud_user_id();
    if (!fn || uid.empty() || selected.empty()) return;
    OBN_INFO("accounts: asking the slicer to refetch its device list");
    fn(uid, "{\"bind\":{\"command\":\"bind\",\"result\":\"success\",\"dev_id\":" +
                obn::json::escape(selected) + "}}");
}

} // namespace obn

// Private exports for the companion account plugin, which resolves them from
// the already-loaded library (ctypes). Not part of Studio's ABI. The plugin
// checks obn_extra_accounts_api() first so it can fall back to restart-based
// switching on libraries without this feature.

OBN_ABI int obn_extra_accounts_api(void)
{
    return 1;
}

// Re-reads obn.accounts.json and applies it. Returns the number of enabled
// extra accounts, or a negative BAMBU_NETWORK_ERR_* when no agent is up yet.
// Blocks while dropped connections shut down; call it off the UI thread.
OBN_ABI int obn_extra_accounts_reload(void)
{
    auto* a = obn::Agent::active_instance();
    return a ? a->reload_extra_accounts() : BAMBU_NETWORK_ERR_INVALID_HANDLE;
}

// JSON snapshot (see Agent::extra_accounts_status_json) in a thread_local
// buffer valid until the next call on the same thread; nullptr with no agent.
OBN_ABI const char* obn_extra_accounts_status(void)
{
    static thread_local std::string s_status;
    auto* a = obn::Agent::active_instance();
    if (!a) return nullptr;
    s_status = a->extra_accounts_status_json();
    return s_status.c_str();
}
