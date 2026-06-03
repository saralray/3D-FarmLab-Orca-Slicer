#pragma once
#include "IUiBackend.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace Slic3r { namespace GUI { namespace Automation {

// JSON-RPC 2.0 standard error codes.
constexpr int kParseError      = -32700;
constexpr int kInvalidRequest  = -32600;
constexpr int kMethodNotFound  = -32601;
constexpr int kInvalidParams   = -32602;
// Application error codes (design spec §5).
constexpr int kErrNotFound        = 1001; // widget/target not found (or ambiguous)
constexpr int kErrNotActionable   = 1002; // disabled / hidden
constexpr int kErrWaitTimeout     = 1003;
constexpr int kErrGuiBusy         = 1004; // GUI thread timeout
constexpr int kErrScreenshotFail  = 1005;
constexpr int kErrDisabled        = 1006;
constexpr int kErrLoadFailed      = 1007; // file.open: load_files returned empty / threw

constexpr const char* kProtocolVersion = "2.0";
constexpr const char* kAutomationVersion = "1.0.0";

class JsonRpcDispatcher {
public:
    explicit JsonRpcDispatcher(IUiBackend& backend);

    // Parse a JSON-RPC request body, dispatch, and return the response body.
    // Never throws; transport-level/parse errors become JSON-RPC error responses.
    std::string handle_request(const std::string& body);

    // For tests: dispatch an already-parsed request object and return the response.
    nlohmann::json dispatch(const nlohmann::json& request);

private:
    nlohmann::json make_result(const nlohmann::json& id, nlohmann::json result);
    nlohmann::json make_error(const nlohmann::json& id, int code, const std::string& msg);

    // Method handlers (each returns the `result` object or throws AutomationError).
    nlohmann::json m_version(const nlohmann::json& params);
    nlohmann::json m_tree_dump(const nlohmann::json& params);
    nlohmann::json m_tree_find(const nlohmann::json& params);
    nlohmann::json m_widget_get(const nlohmann::json& params);
    nlohmann::json m_input_click(const nlohmann::json& params);
    nlohmann::json m_input_type(const nlohmann::json& params);
    nlohmann::json m_input_key(const nlohmann::json& params);
    nlohmann::json m_sync_wait_for(const nlohmann::json& params);
    nlohmann::json m_app_state(const nlohmann::json& params);
    nlohmann::json m_screenshot_window(const nlohmann::json& params);
    nlohmann::json m_file_open(const nlohmann::json& params);

    // Resolve a unique, actionable (enabled+visible) node from params["target"].
    // Throws kErrNotFound (missing/ambiguous) or kErrNotActionable (disabled/hidden).
    // `tree_out` keeps the snapshot alive; the returned node is a stable copy.
    const UiNode resolve_actionable(const nlohmann::json& params, UiNode& tree_out);

    IUiBackend& m_backend;
};

}}} // namespace
