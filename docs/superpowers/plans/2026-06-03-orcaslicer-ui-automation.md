# OrcaSlicer UI Automation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in localhost JSON-RPC server to a running OrcaSlicer GUI that lets an external script introspect, drive (via simulated input), and screenshot the real UI (wx widgets, the 3D viewport, and ImGui controls).

**Architecture:** A pure, GUI-free protocol core (data model + serializer + locator + JSON-RPC dispatcher behind an `IUiBackend` interface) is fully unit-tested in CI against a `MockUiBackend`. A boost::beast listener (`AutomationServer`) receives `POST /jsonrpc` on `127.0.0.1` and feeds bodies to the dispatcher. The real `WxUiBackend` marshals every call onto the GUI thread, walks the `wxWindow` tree, reads a per-frame ImGui item table, drives `wxUIActionSimulator`, and captures screenshots via window DCs and `GLCanvas3D::render_thumbnail()`. Everything is gated by `is_automation_enabled()` so a disabled build has zero new threads and zero behavior change.

**Tech Stack:** C++17, wxWidgets, boost::asio/beast (already linked), `nlohmann/json` (vendored, header-only), Catch2 v3.11.0, Dear ImGui (with `imgui_internal.h` in-tree), Python 3 (reference client).

---

## Architecture note — refinement of the spec's `IUiBackend` (read before starting)

The design spec (`docs/superpowers/specs/2026-06-03-orcaslicer-ui-automation-design.md`) lists `IUiBackend` methods as `dump_tree, find, get_widget, click, type, key, wait_for, app_state, screenshot_window, screenshot_viewport3d`. The spec **also** requires (§11) that *locator resolution* and *the dispatcher* be unit-testable with no GUI, and that the dispatcher have **no** wx/ImGui/GL includes.

If `find`/`get_widget`/`click(target)`/`wait_for` resolved targets *inside* the backend, that resolution logic would live in both `WxUiBackend` (real) and `MockUiBackend` (tests) — a DRY violation — and could not be unit-tested independently. So this plan keeps the **external JSON-RPC protocol exactly as the spec defines it** (method names, params, results, error codes, node shape — see §5 of the spec) but refines the **internal C++ layering**:

- **Pure / CI-tested (no wx):** `UiNode` + structs, `WidgetSerializer` (`UiNode` ↔ JSON), `Locator` (resolve target spec + evaluate wait-state over `UiNode` trees), `JsonRpcDispatcher` (parse envelope → validate params → `dump_tree` → resolve via `Locator` → call a backend *primitive*).
- **`IUiBackend`** exposes a snapshot (`dump_tree`), an `app_state`, **input primitives that act on an already-resolved `UiNode`** (carrying its screen rect + an opaque `handle`), screenshots, and a `refresh_ui` hook. The dispatcher orchestrates; the backend only collects nodes and executes primitives.
- **`WxUiBackend`** (GUI thread, manual verification) produces `UiNode`s from the wx tree + ImGui item table and executes the primitives; it marshals every public call onto the GUI thread.
- **`MockUiBackend`** (tests) returns canned trees and records primitive calls.

This satisfies every spec goal while keeping the heart of the system testable. The opaque `UiNode::handle` (a `wxWindow*` cast to `uintptr_t` for wx, or an item index for ImGui) lets the real backend recover concrete objects without re-running resolution; it is never serialized.

---

## File Structure

**New — pure core (compiled into both the test target and `libslic3r_gui`):**
- `src/slic3r/GUI/Automation/IUiBackend.hpp` — plain structs (`UiNode`, `Rect`, `DumpOptions`, `AppState`, `KeyChord`, `PngImage`, `AutomationError`) + the abstract `IUiBackend`. No wx/ImGui/GL.
- `src/slic3r/GUI/Automation/WidgetSerializer.{hpp,cpp}` — `UiNode`/`AppState` → `nlohmann::json`.
- `src/slic3r/GUI/Automation/Locator.{hpp,cpp}` — flatten tree, resolve `Target` (id → path → predicate), `evaluate_state` for `sync.wait_for`.
- `src/slic3r/GUI/Automation/JsonRpcDispatcher.{hpp,cpp}` — JSON-RPC 2.0 parse/route/build; one handler per v1 method; depends only on `IUiBackend` + `Locator` + `WidgetSerializer`.

**New — server (GUI side, no widget code):**
- `src/slic3r/GUI/Automation/AutomationServer.{hpp,cpp}` — boost::beast listener on `127.0.0.1`, `POST /jsonrpc` (reads body) + `GET /` health page, own thread, delegates body string to a `std::function<std::string(const std::string&)>`.

**New — real GUI backend & ImGui recording (GUI thread):**
- `src/slic3r/GUI/Automation/AutomationRegistry.{hpp,cpp}` — `wxWindow* ↔ automation_id` side map + `set_automation_id()` helper.
- `src/slic3r/GUI/Automation/ImGuiItemTable.{hpp,cpp}` — double-buffered per-frame recorder of ImGui items + window enumeration.
- `src/slic3r/GUI/Automation/WxUiBackend.{hpp,cpp}` — real `IUiBackend`; GUI-thread marshaller; wx tree walk; ImGui item read; `wxUIActionSimulator`; screenshots.

**New — tests, client, docs:**
- `tests/automation/CMakeLists.txt`
- `tests/automation/automation_tests.cpp` — Catch2 entry (links `Catch2::Catch2WithMain`).
- `tests/automation/test_serializer.cpp`, `test_locator.cpp`, `test_dispatcher.cpp`
- `tests/automation/MockUiBackend.{hpp,cpp}`
- `tools/automation/orca_automation.py` — reference Python client.
- `tools/automation/example_slice.py` — runnable e2e smoke test.
- `doc/automation.md` — protocol reference.

**Changed:**
- `src/slic3r/GUI/ImGuiWrapper.cpp` — guarded recording hooks (`button`, `bbl_button`, `checkbox`, `bbl_checkbox`, `combo`, `slider_float`, `input_double`, `radio_button`, `menu_item_with_icon`, `begin`/`end`, frame swap in `render()`).
- `src/slic3r/GUI/ImGuiWrapper.hpp` — declare the (compiled-out-when-disabled) recording hook.
- `src/slic3r/GUI/GUI_App.{hpp,cpp}` — own `AutomationServer`/`WxUiBackend`/`JsonRpcDispatcher`; `is_automation_enabled()`; start in `post_init()`, stop in `OnExit()`.
- `src/slic3r/GUI/GUI_Init.hpp` — add `int automation_port` to `GUI_InitParams`.
- `src/OrcaSlicer.cpp` — read the new CLI options, populate `GUI_InitParams`.
- `src/libslic3r/PrintConfig.cpp` — register `automation_server` (bool) + `automation_server_port` (int) in `CLIMiscConfigDef`.
- `src/slic3r/CMakeLists.txt` — add `Automation/` sources to `SLIC3R_GUI_SOURCES`.
- `tests/CMakeLists.txt` — `add_subdirectory(automation)`.
- ~15-20 widget-construction sites — `set_automation_id(...)` calls.

### CLI flag mapping (deliberate, documented)

The spec asks for `--automation-server[=PORT]`. OrcaSlicer's CLI is a `DynamicConfig` (`read_cli`) that does not cleanly support an optional-value bool. To work *with* the framework rather than around it, this plan implements two options:
- `--automation-server` (`coBool`) — enable the server.
- `--automation-server-port` (`coInt`, default `13619`) — port override.

Same capability; the docs and example use these flag names. `automation_port` in `GUI_InitParams` is `0` when disabled, else the chosen port.

---

## PHASE 1 — Pure protocol core (CI-tested, no GUI)

### Task 1: Test target scaffold + core types + first serializer test

**Files:**
- Create: `src/slic3r/GUI/Automation/IUiBackend.hpp`
- Create: `src/slic3r/GUI/Automation/WidgetSerializer.hpp`
- Create: `src/slic3r/GUI/Automation/WidgetSerializer.cpp`
- Create: `tests/automation/CMakeLists.txt`
- Create: `tests/automation/automation_tests.cpp`
- Create: `tests/automation/test_serializer.cpp`
- Modify: `tests/CMakeLists.txt:50-54` (add `add_subdirectory(automation)`)

- [ ] **Step 1: Create the core types header**

`src/slic3r/GUI/Automation/IUiBackend.hpp`:

```cpp
#pragma once
// PURE header: no wx / ImGui / GL includes. Safe to compile in the display-free
// unit-test target. Shared by the dispatcher, serializer, locator, and backends.
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Automation {

enum class BackendKind { Wx, ImGui };

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

// One node of the unified UI tree. `handle` is opaque (wxWindow* cast to uintptr_t
// for wx, item index for ImGui); it is used by WxUiBackend to recover concrete
// objects and is NEVER serialized.
struct UiNode {
    BackendKind         backend = BackendKind::Wx;
    std::string         id;       // automation id if set, else derived path id
    std::string         path;     // positional path, e.g. "MainFrame/Panel[2]/Button[0]"
    std::string         klass;    // wx class name or imgui item type
    std::string         label;
    Rect                rect;     // screen coordinates
    bool                enabled = true;
    bool                visible = true;
    bool                has_value = false;
    std::string         value;    // when applicable (text/choice/check/slider)
    std::uint64_t       handle = 0;
    std::vector<UiNode> children; // wx only; imgui items are flat under their window
};

struct DumpOptions {
    std::optional<std::string> root;          // id/path to root the dump at
    int                        max_depth = -1; // -1 = unlimited
    bool                       visible_only = false;
    bool                       include_imgui = true;
};

enum class MouseButton { Left, Right, Middle };
enum class KeyModifier { Ctrl, Shift, Alt, Cmd };

struct KeyChord {
    std::vector<KeyModifier> modifiers;
    std::string              key; // normalized lowercase: "s", "enter", "f5", "tab", ...
};

struct AppState {
    std::string                active_tab;
    bool                       project_loaded = false;
    bool                       slicing = false;
    int                        slice_progress = -1; // -1 = unknown
    std::optional<std::string> modal_dialog;
    bool                       foreground = false;
};

struct PngImage {
    std::vector<unsigned char> png; // encoded PNG bytes
    int                        width = 0;
    int                        height = 0;
};

// Thrown by backends/dispatcher; carries a JSON-RPC application error code.
struct AutomationError : std::runtime_error {
    int code;
    AutomationError(int code, std::string msg)
        : std::runtime_error(std::move(msg)), code(code) {}
};

// Backend abstraction. The dispatcher orchestrates; the backend only snapshots
// and executes primitives on already-resolved nodes.
class IUiBackend {
public:
    virtual ~IUiBackend() = default;

    // Force a fresh frame so transient ImGui items are recorded before a read or
    // action. No-op for non-GUI backends.
    virtual void refresh_ui() = 0;

    // Snapshot the UI tree (wx hierarchy + flat imgui items under their windows).
    virtual UiNode dump_tree(const DumpOptions& opts) = 0;

    // Application-level state snapshot.
    virtual AppState app_state() = 0;

    // Click a resolved node (uses its rect/handle). Raises/focuses first.
    virtual bool click(const UiNode& node, MouseButton button, bool dbl,
                       const std::vector<KeyModifier>& modifiers) = 0;
    // Type into the currently-focused control.
    virtual bool type_text(const std::string& text) = 0;
    // Send key chords (e.g. ctrl+s) to the focused window.
    virtual bool send_keys(const std::vector<KeyChord>& chords) = 0;

    // Screenshots. target == nullptr => main frame.
    virtual PngImage screenshot_window(const UiNode* target) = 0;
    virtual PngImage screenshot_viewport3d(std::optional<int> plate,
                                           std::optional<int> width,
                                           std::optional<int> height) = 0;
};

}}} // namespace Slic3r::GUI::Automation
```

- [ ] **Step 2: Create the serializer header**

`src/slic3r/GUI/Automation/WidgetSerializer.hpp`:

```cpp
#pragma once
#include "IUiBackend.hpp"
#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI { namespace Automation {

// Serialize a node to the unified JSON shape from the design spec (§5).
// `include_children` controls recursion into UiNode::children.
nlohmann::json node_to_json(const UiNode& node, bool include_children);

// Serialize an application-state snapshot.
nlohmann::json app_state_to_json(const AppState& state);

}}} // namespace
```

- [ ] **Step 3: Write the failing serializer test**

`tests/automation/test_serializer.cpp`:

```cpp
#include <catch2/catch_all.hpp>
#include "slic3r/GUI/Automation/WidgetSerializer.hpp"

using namespace Slic3r::GUI::Automation;

TEST_CASE("node_to_json emits the unified node shape", "[automation][serializer]") {
    UiNode n;
    n.backend = BackendKind::Wx;
    n.id      = "btn_slice";
    n.path    = "MainFrame/Panel[2]/Button[0]";
    n.klass   = "Button";
    n.label   = "Slice plate";
    n.rect    = {100, 200, 120, 32};
    n.enabled = true;
    n.visible = true;

    const nlohmann::json j = node_to_json(n, /*include_children*/ false);

    CHECK(j.at("backend") == "wx");
    CHECK(j.at("id") == "btn_slice");
    CHECK(j.at("path") == "MainFrame/Panel[2]/Button[0]");
    CHECK(j.at("class") == "Button");
    CHECK(j.at("label") == "Slice plate");
    CHECK(j.at("rect").at("x") == 100);
    CHECK(j.at("rect").at("w") == 120);
    CHECK(j.at("enabled") == true);
    CHECK(j.at("visible") == true);
    // `handle` must never leak into JSON.
    CHECK_FALSE(j.contains("handle"));
    // No value set -> no "value" key.
    CHECK_FALSE(j.contains("value"));
}
```

- [ ] **Step 4: Create the Catch2 entry TU**

`tests/automation/automation_tests.cpp`:

```cpp
// Catch2 provides main() via Catch2::Catch2WithMain. This TU exists so the
// executable has at least one source plus a stable name; per-feature TEST_CASEs
// live in the test_*.cpp files.
#include <catch2/catch_all.hpp>
```

- [ ] **Step 5: Create the test CMake target**

`tests/automation/CMakeLists.txt`:

```cmake
get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

# Compile the PURE automation sources directly (no wx/ImGui/GL), so this test
# executable needs no display and does not link libslic3r_gui.
add_executable(${_TEST_NAME}_tests
    automation_tests.cpp
    test_serializer.cpp
    MockUiBackend.cpp
    test_locator.cpp
    test_dispatcher.cpp
    ${CMAKE_SOURCE_DIR}/src/slic3r/GUI/Automation/WidgetSerializer.cpp
    ${CMAKE_SOURCE_DIR}/src/slic3r/GUI/Automation/Locator.cpp
    ${CMAKE_SOURCE_DIR}/src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp
)

target_link_libraries(${_TEST_NAME}_tests test_common Catch2::Catch2WithMain nlohmann_json)
target_include_directories(${_TEST_NAME}_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

catch_discover_tests(${_TEST_NAME}_tests)
```

> NOTE: `MockUiBackend.cpp`, `test_locator.cpp`, `test_dispatcher.cpp`, `Locator.cpp`, and `JsonRpcDispatcher.cpp` are created in later tasks. To build *this* task in isolation, temporarily list only `automation_tests.cpp`, `test_serializer.cpp`, and `WidgetSerializer.cpp`; add the rest as their tasks land. (Subagent-driven execution: add each file in the task that creates it.)

For Task 1 specifically, use this reduced target body:

```cmake
add_executable(${_TEST_NAME}_tests
    automation_tests.cpp
    test_serializer.cpp
    ${CMAKE_SOURCE_DIR}/src/slic3r/GUI/Automation/WidgetSerializer.cpp
)
target_link_libraries(${_TEST_NAME}_tests test_common Catch2::Catch2WithMain nlohmann_json)
target_include_directories(${_TEST_NAME}_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")
orcaslicer_copy_test_dlls()
catch_discover_tests(${_TEST_NAME}_tests)
```

- [ ] **Step 6: Register the test subdirectory**

In `tests/CMakeLists.txt`, after line 54 (`add_subdirectory(sla_print)`), add:

```cmake
add_subdirectory(automation)
```

- [ ] **Step 7: Build and run to verify the test FAILS (no implementation yet)**

Run (Windows, from the build dir):
```
cmake --build . --config RelWithDebInfo --target automation_tests -- -m
ctest --test-dir tests/automation --output-on-failure
```
Expected: link/compile error — `node_to_json` is declared but not defined.

- [ ] **Step 8: Implement the serializer**

`src/slic3r/GUI/Automation/WidgetSerializer.cpp`:

```cpp
#include "WidgetSerializer.hpp"

namespace Slic3r { namespace GUI { namespace Automation {

static const char* backend_name(BackendKind k) {
    return k == BackendKind::Wx ? "wx" : "imgui";
}

nlohmann::json node_to_json(const UiNode& node, bool include_children) {
    nlohmann::json j;
    j["backend"] = backend_name(node.backend);
    j["id"]      = node.id;
    j["path"]    = node.path;
    j["class"]   = node.klass;
    j["label"]   = node.label;
    j["rect"]    = { {"x", node.rect.x}, {"y", node.rect.y},
                     {"w", node.rect.w}, {"h", node.rect.h} };
    j["enabled"] = node.enabled;
    j["visible"] = node.visible;
    if (node.has_value)
        j["value"] = node.value;
    if (include_children && node.backend == BackendKind::Wx) {
        nlohmann::json arr = nlohmann::json::array();
        for (const UiNode& c : node.children)
            arr.push_back(node_to_json(c, true));
        j["children"] = std::move(arr);
    }
    return j;
}

nlohmann::json app_state_to_json(const AppState& s) {
    nlohmann::json j;
    j["active_tab"]     = s.active_tab;
    j["project_loaded"] = s.project_loaded;
    j["slicing"]        = s.slicing;
    j["slice_progress"] = s.slice_progress;
    j["foreground"]     = s.foreground;
    if (s.modal_dialog)
        j["modal_dialog"] = *s.modal_dialog;
    return j;
}

}}} // namespace
```

- [ ] **Step 9: Build and run to verify the test PASSES**

Run:
```
cmake --build . --config RelWithDebInfo --target automation_tests -- -m
ctest --test-dir tests/automation --output-on-failure
```
Expected: `test_serializer.cpp` cases PASS.

- [ ] **Step 10: Commit**

```bash
git add src/slic3r/GUI/Automation/IUiBackend.hpp \
        src/slic3r/GUI/Automation/WidgetSerializer.hpp \
        src/slic3r/GUI/Automation/WidgetSerializer.cpp \
        tests/automation/ tests/CMakeLists.txt
git commit -m "feat(automation): pure UI node model + JSON serializer with unit test"
```

---

### Task 2: Serializer — children, values, ImGui nodes, app_state

**Files:**
- Modify: `tests/automation/test_serializer.cpp`
- (Implementation already covers these; this task locks behavior with tests and fixes any gaps.)

- [ ] **Step 1: Add failing tests for children/value/imgui/app_state**

Append to `tests/automation/test_serializer.cpp`:

```cpp
TEST_CASE("node_to_json includes children only for wx when requested",
          "[automation][serializer]") {
    UiNode parent;
    parent.backend = BackendKind::Wx;
    parent.klass   = "Panel";
    UiNode child;
    child.backend = BackendKind::Wx;
    child.klass   = "Button";
    child.label   = "OK";
    parent.children.push_back(child);

    const auto with    = node_to_json(parent, true);
    const auto without = node_to_json(parent, false);

    REQUIRE(with.contains("children"));
    CHECK(with.at("children").size() == 1);
    CHECK(with.at("children")[0].at("label") == "OK");
    CHECK_FALSE(without.contains("children"));
}

TEST_CASE("node_to_json emits value and imgui backend tag",
          "[automation][serializer]") {
    UiNode n;
    n.backend   = BackendKind::ImGui;
    n.klass     = "combo";
    n.has_value = true;
    n.value     = "PLA";
    const auto j = node_to_json(n, /*include_children*/ true);
    CHECK(j.at("backend") == "imgui");
    CHECK(j.at("value") == "PLA");
    CHECK_FALSE(j.contains("children")); // imgui items are flat
}

TEST_CASE("app_state_to_json shape", "[automation][serializer]") {
    AppState s;
    s.active_tab     = "preview";
    s.project_loaded = true;
    s.slicing        = true;
    s.slice_progress = 42;
    s.foreground     = true;
    s.modal_dialog   = std::string("Save changes?");
    const auto j = app_state_to_json(s);
    CHECK(j.at("active_tab") == "preview");
    CHECK(j.at("project_loaded") == true);
    CHECK(j.at("slice_progress") == 42);
    CHECK(j.at("modal_dialog") == "Save changes?");
}
```

- [ ] **Step 2: Run to confirm PASS (implementation from Task 1 already covers these)**

Run: `ctest --test-dir tests/automation --output-on-failure`
Expected: all serializer cases PASS. If `modal_dialog` or `children` gating fails, fix `WidgetSerializer.cpp` accordingly (it should already match).

- [ ] **Step 3: Commit**

```bash
git add tests/automation/test_serializer.cpp
git commit -m "test(automation): lock serializer children/value/imgui/app_state shapes"
```

---

### Task 3: Locator — flatten + find_matches (id / path / predicate)

**Files:**
- Create: `src/slic3r/GUI/Automation/Locator.hpp`
- Create: `src/slic3r/GUI/Automation/Locator.cpp`
- Create: `tests/automation/test_locator.cpp`
- Modify: `tests/automation/CMakeLists.txt` (add `test_locator.cpp` + `Locator.cpp`)

- [ ] **Step 1: Create the Locator header**

`src/slic3r/GUI/Automation/Locator.hpp`:

```cpp
#pragma once
#include "IUiBackend.hpp"
#include <optional>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Automation {

// A target specification. Resolution order: id -> path -> predicate
// (name OR class OR label OR value, all provided fields must match).
struct Target {
    std::optional<std::string> id;
    std::optional<std::string> path;
    std::optional<std::string> name;  // matches id OR label
    std::optional<std::string> klass;
    std::optional<std::string> label;
    std::optional<std::string> value;
    std::optional<BackendKind> backend;
    bool empty() const {
        return !id && !path && !name && !klass && !label && !value;
    }
};

// Depth-first flatten of a tree into stable-ordered pointers (parents before children).
std::vector<const UiNode*> flatten(const UiNode& root);

// All nodes matching the target spec (resolution-order aware).
std::vector<const UiNode*> find_matches(const UiNode& root, const Target& target);

enum class WaitState { Exists, Visible, Enabled, Value };

// True if `node` satisfies the wait condition. A null node only satisfies a
// negative... here we keep it simple: null => false for all states.
bool evaluate_state(const UiNode* node, WaitState state,
                    const std::optional<std::string>& expected_value);

}}} // namespace
```

- [ ] **Step 2: Write failing locator tests**

`tests/automation/test_locator.cpp`:

```cpp
#include <catch2/catch_all.hpp>
#include "slic3r/GUI/Automation/Locator.hpp"

using namespace Slic3r::GUI::Automation;

namespace {
UiNode make_tree() {
    UiNode root;
    root.klass = "MainFrame";
    root.path  = "MainFrame";

    UiNode panel;
    panel.klass = "Panel";
    panel.path  = "MainFrame/Panel[0]";

    UiNode slice;
    slice.id    = "btn_slice";
    slice.klass = "Button";
    slice.label = "Slice plate";
    slice.path  = "MainFrame/Panel[0]/Button[0]";

    UiNode export_btn;
    export_btn.id    = "btn_export";
    export_btn.klass = "Button";
    export_btn.label = "Export";
    export_btn.path  = "MainFrame/Panel[0]/Button[1]";

    UiNode dup;            // duplicate label, used for ambiguity tests
    dup.klass = "Button";
    dup.label = "Export";
    dup.path  = "MainFrame/Panel[0]/Button[2]";

    panel.children = {slice, export_btn, dup};
    root.children  = {panel};
    return root;
}
} // namespace

TEST_CASE("flatten yields parents before children", "[automation][locator]") {
    const auto tree = make_tree();
    const auto all  = flatten(tree);
    REQUIRE(all.size() == 5);
    CHECK(all.front()->klass == "MainFrame");
}

TEST_CASE("find_matches by exact id returns one", "[automation][locator]") {
    const auto tree = make_tree();
    Target t; t.id = "btn_slice";
    const auto m = find_matches(tree, t);
    REQUIRE(m.size() == 1);
    CHECK(m[0]->label == "Slice plate");
}

TEST_CASE("find_matches by exact path returns one", "[automation][locator]") {
    const auto tree = make_tree();
    Target t; t.path = "MainFrame/Panel[0]/Button[1]";
    const auto m = find_matches(tree, t);
    REQUIRE(m.size() == 1);
    CHECK(m[0]->id == "btn_export");
}

TEST_CASE("find_matches by predicate (label) can be ambiguous",
          "[automation][locator]") {
    const auto tree = make_tree();
    Target t; t.label = "Export";
    const auto m = find_matches(tree, t);
    CHECK(m.size() == 2); // btn_export + the duplicate
}

TEST_CASE("find_matches predicate combines fields (AND)",
          "[automation][locator]") {
    const auto tree = make_tree();
    Target t; t.label = "Export"; t.klass = "Button"; t.id = std::nullopt;
    // id/path absent -> predicate mode. Both fields must match.
    t.id = std::nullopt;
    const auto m = find_matches(tree, t);
    CHECK(m.size() == 2);
}

TEST_CASE("find_matches by name matches id OR label", "[automation][locator]") {
    const auto tree = make_tree();
    Target byId; byId.name = "btn_slice";
    CHECK(find_matches(tree, byId).size() == 1);
    Target byLabel; byLabel.name = "Slice plate";
    CHECK(find_matches(tree, byLabel).size() == 1);
}

TEST_CASE("find_matches not found returns empty", "[automation][locator]") {
    const auto tree = make_tree();
    Target t; t.id = "nope";
    CHECK(find_matches(tree, t).empty());
}
```

- [ ] **Step 3: Add the files to the test target**

In `tests/automation/CMakeLists.txt`, extend the executable source list (now using the fuller set, minus dispatcher which arrives in Task 6):

```cmake
add_executable(${_TEST_NAME}_tests
    automation_tests.cpp
    test_serializer.cpp
    test_locator.cpp
    ${CMAKE_SOURCE_DIR}/src/slic3r/GUI/Automation/WidgetSerializer.cpp
    ${CMAKE_SOURCE_DIR}/src/slic3r/GUI/Automation/Locator.cpp
)
```

- [ ] **Step 4: Run to verify FAIL (Locator.cpp empty / unlinked)**

Run: `cmake --build . --config RelWithDebInfo --target automation_tests -- -m`
Expected: undefined reference to `flatten`/`find_matches`.

- [ ] **Step 5: Implement flatten + find_matches**

`src/slic3r/GUI/Automation/Locator.cpp`:

```cpp
#include "Locator.hpp"

namespace Slic3r { namespace GUI { namespace Automation {

static void flatten_into(const UiNode& n, std::vector<const UiNode*>& out) {
    out.push_back(&n);
    for (const UiNode& c : n.children)
        flatten_into(c, out);
}

std::vector<const UiNode*> flatten(const UiNode& root) {
    std::vector<const UiNode*> out;
    flatten_into(root, out);
    return out;
}

static bool matches_predicate(const UiNode& n, const Target& t) {
    if (t.backend && n.backend != *t.backend) return false;
    if (t.name && !(n.id == *t.name || n.label == *t.name)) return false;
    if (t.klass && n.klass != *t.klass) return false;
    if (t.label && n.label != *t.label) return false;
    if (t.value && !(n.has_value && n.value == *t.value)) return false;
    return true;
}

std::vector<const UiNode*> find_matches(const UiNode& root, const Target& target) {
    const auto all = flatten(root);
    std::vector<const UiNode*> out;

    // Resolution order: exact id -> exact path -> predicate.
    if (target.id) {
        for (const UiNode* n : all)
            if (n->id == *target.id) out.push_back(n);
        return out;
    }
    if (target.path) {
        for (const UiNode* n : all)
            if (n->path == *target.path) out.push_back(n);
        return out;
    }
    if (target.empty())
        return out; // nothing to match on
    for (const UiNode* n : all)
        if (matches_predicate(*n, target)) out.push_back(n);
    return out;
}

bool evaluate_state(const UiNode* node, WaitState state,
                    const std::optional<std::string>& expected_value) {
    if (node == nullptr)
        return false;
    switch (state) {
        case WaitState::Exists:  return true;
        case WaitState::Visible: return node->visible;
        case WaitState::Enabled: return node->enabled && node->visible;
        case WaitState::Value:
            return node->has_value && expected_value && node->value == *expected_value;
    }
    return false;
}

}}} // namespace
```

- [ ] **Step 6: Run to verify PASS**

Run: `ctest --test-dir tests/automation --output-on-failure`
Expected: all locator cases PASS.

- [ ] **Step 7: Commit**

```bash
git add src/slic3r/GUI/Automation/Locator.hpp \
        src/slic3r/GUI/Automation/Locator.cpp \
        tests/automation/test_locator.cpp tests/automation/CMakeLists.txt
git commit -m "feat(automation): pure locator (id/path/predicate) with unit tests"
```

---

### Task 4: Locator — `resolve_unique` + `evaluate_state` edge cases

**Files:**
- Modify: `src/slic3r/GUI/Automation/Locator.hpp`
- Modify: `src/slic3r/GUI/Automation/Locator.cpp`
- Modify: `tests/automation/test_locator.cpp`

- [ ] **Step 1: Declare `resolve_unique`**

Add to `Locator.hpp` (above `WaitState`):

```cpp
// Resolve to exactly one node for actions. Returns the node on a unique match;
// returns nullptr otherwise and sets match_count (0 = not found, >1 = ambiguous).
const UiNode* resolve_unique(const UiNode& root, const Target& target, int& match_count);
```

- [ ] **Step 2: Write failing tests for resolve_unique + evaluate_state**

Append to `tests/automation/test_locator.cpp`:

```cpp
TEST_CASE("resolve_unique success / not-found / ambiguous",
          "[automation][locator]") {
    const auto tree = make_tree();
    int count = -1;

    Target ok; ok.id = "btn_slice";
    CHECK(resolve_unique(tree, ok, count) != nullptr);
    CHECK(count == 1);

    Target missing; missing.id = "nope";
    CHECK(resolve_unique(tree, missing, count) == nullptr);
    CHECK(count == 0);

    Target ambiguous; ambiguous.label = "Export";
    CHECK(resolve_unique(tree, ambiguous, count) == nullptr);
    CHECK(count == 2);
}

TEST_CASE("evaluate_state covers exists/visible/enabled/value",
          "[automation][locator]") {
    UiNode n; n.visible = true; n.enabled = false;
    n.has_value = true; n.value = "PLA";

    CHECK(evaluate_state(&n, WaitState::Exists,  std::nullopt));
    CHECK(evaluate_state(&n, WaitState::Visible, std::nullopt));
    CHECK_FALSE(evaluate_state(&n, WaitState::Enabled, std::nullopt)); // disabled
    CHECK(evaluate_state(&n, WaitState::Value, std::string("PLA")));
    CHECK_FALSE(evaluate_state(&n, WaitState::Value, std::string("ABS")));
    CHECK_FALSE(evaluate_state(nullptr, WaitState::Exists, std::nullopt));
}
```

- [ ] **Step 3: Run to verify FAIL**

Run: `cmake --build . --config RelWithDebInfo --target automation_tests -- -m`
Expected: undefined reference to `resolve_unique`.

- [ ] **Step 4: Implement resolve_unique**

Add to `Locator.cpp`:

```cpp
const UiNode* resolve_unique(const UiNode& root, const Target& target, int& match_count) {
    const auto m = find_matches(root, target);
    match_count = static_cast<int>(m.size());
    return m.size() == 1 ? m.front() : nullptr;
}
```

- [ ] **Step 5: Run to verify PASS**

Run: `ctest --test-dir tests/automation --output-on-failure`
Expected: all locator cases PASS.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/Automation/Locator.hpp \
        src/slic3r/GUI/Automation/Locator.cpp tests/automation/test_locator.cpp
git commit -m "feat(automation): resolve_unique + wait-state evaluation"
```

---

### Task 5: MockUiBackend (test double)

**Files:**
- Create: `tests/automation/MockUiBackend.hpp`
- Create: `tests/automation/MockUiBackend.cpp`
- Modify: `tests/automation/CMakeLists.txt` (add `MockUiBackend.cpp`)

- [ ] **Step 1: Create the mock header**

`tests/automation/MockUiBackend.hpp`:

```cpp
#pragma once
#include "slic3r/GUI/Automation/IUiBackend.hpp"
#include <functional>
#include <vector>

namespace Slic3r { namespace GUI { namespace Automation {

// Deterministic fake backend for dispatcher tests. Records every primitive call
// and returns canned data. `tree_provider` lets a test return different trees on
// successive dump_tree() calls (used for sync.wait_for tests).
class MockUiBackend : public IUiBackend {
public:
    // Recorded calls (inspected by tests).
    int               refresh_count = 0;
    int               dump_count    = 0;
    std::vector<std::string> clicked_ids;       // node.id of each click()
    std::vector<MouseButton> click_buttons;
    std::vector<std::string> typed_text;
    std::vector<std::vector<KeyChord>> sent_keys;
    int               screenshot_window_count   = 0;
    int               screenshot_viewport_count = 0;

    // Canned outputs (set by tests).
    UiNode   tree;                                  // default tree for dump_tree
    AppState state;
    PngImage canned_png{ {0x89,0x50,0x4E,0x47}, 4, 4 }; // fake "PNG" bytes
    bool     click_result = true;

    // Optional: per-call tree provider (overrides `tree` when set).
    std::function<UiNode(int /*call_index*/)> tree_provider;

    void     refresh_ui() override { ++refresh_count; }
    UiNode   dump_tree(const DumpOptions&) override {
        const int idx = dump_count++;
        return tree_provider ? tree_provider(idx) : tree;
    }
    AppState app_state() override { return state; }
    bool     click(const UiNode& node, MouseButton button, bool /*dbl*/,
                   const std::vector<KeyModifier>&) override {
        clicked_ids.push_back(node.id);
        click_buttons.push_back(button);
        return click_result;
    }
    bool     type_text(const std::string& text) override {
        typed_text.push_back(text); return true;
    }
    bool     send_keys(const std::vector<KeyChord>& chords) override {
        sent_keys.push_back(chords); return true;
    }
    PngImage screenshot_window(const UiNode*) override {
        ++screenshot_window_count; return canned_png;
    }
    PngImage screenshot_viewport3d(std::optional<int>, std::optional<int>,
                                   std::optional<int>) override {
        ++screenshot_viewport_count; return canned_png;
    }
};

}}} // namespace
```

- [ ] **Step 2: Create the mock .cpp (sanity test + TU)**

`tests/automation/MockUiBackend.cpp`:

```cpp
#include "MockUiBackend.hpp"
#include <catch2/catch_all.hpp>

using namespace Slic3r::GUI::Automation;

TEST_CASE("MockUiBackend records calls", "[automation][mock]") {
    MockUiBackend mock;
    UiNode n; n.id = "btn_slice";
    mock.click(n, MouseButton::Left, false, {});
    REQUIRE(mock.clicked_ids.size() == 1);
    CHECK(mock.clicked_ids[0] == "btn_slice");
    CHECK(mock.click_buttons[0] == MouseButton::Left);
}
```

- [ ] **Step 3: Add `MockUiBackend.cpp` to the test target**

In `tests/automation/CMakeLists.txt`, add `MockUiBackend.cpp` to the source list.

- [ ] **Step 4: Build and run to verify PASS**

Run: `ctest --test-dir tests/automation --output-on-failure`
Expected: the mock sanity case PASSES.

- [ ] **Step 5: Commit**

```bash
git add tests/automation/MockUiBackend.hpp tests/automation/MockUiBackend.cpp \
        tests/automation/CMakeLists.txt
git commit -m "test(automation): MockUiBackend recording test double"
```

---

### Task 6: JsonRpcDispatcher — envelope, `automation.version`, error model

**Files:**
- Create: `src/slic3r/GUI/Automation/JsonRpcDispatcher.hpp`
- Create: `src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp`
- Create: `tests/automation/test_dispatcher.cpp`
- Modify: `tests/automation/CMakeLists.txt` (add `test_dispatcher.cpp` + `JsonRpcDispatcher.cpp`)

- [ ] **Step 1: Create the dispatcher header**

`src/slic3r/GUI/Automation/JsonRpcDispatcher.hpp`:

```cpp
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
    nlohmann::json m_screenshot_viewport3d(const nlohmann::json& params);

    IUiBackend& m_backend;
};

}}} // namespace
```

- [ ] **Step 2: Write failing dispatcher tests (envelope + version + errors)**

`tests/automation/test_dispatcher.cpp`:

```cpp
#include <catch2/catch_all.hpp>
#include "slic3r/GUI/Automation/JsonRpcDispatcher.hpp"
#include "MockUiBackend.hpp"

using namespace Slic3r::GUI::Automation;
using nlohmann::json;

TEST_CASE("dispatch automation.version", "[automation][rpc]") {
    MockUiBackend mock;
    JsonRpcDispatcher d(mock);
    const json req = {{"jsonrpc","2.0"},{"id",1},{"method","automation.version"}};
    const json resp = d.dispatch(req);
    CHECK(resp.at("jsonrpc") == "2.0");
    CHECK(resp.at("id") == 1);
    CHECK(resp.at("result").at("version") == kAutomationVersion);
    CHECK(resp.at("result").at("protocol") == "2.0");
    CHECK(resp.at("result").at("capabilities").is_array());
}

TEST_CASE("unknown method -> -32601", "[automation][rpc]") {
    MockUiBackend mock;
    JsonRpcDispatcher d(mock);
    const json req = {{"jsonrpc","2.0"},{"id",7},{"method","does.not.exist"}};
    const json resp = d.dispatch(req);
    CHECK(resp.at("id") == 7);
    CHECK(resp.at("error").at("code") == kMethodNotFound);
}

TEST_CASE("malformed JSON body -> parse error", "[automation][rpc]") {
    MockUiBackend mock;
    JsonRpcDispatcher d(mock);
    const std::string resp = d.handle_request("{not json");
    const json j = json::parse(resp);
    CHECK(j.at("error").at("code") == kParseError);
    CHECK(j.at("id").is_null());
}

TEST_CASE("missing method field -> invalid request", "[automation][rpc]") {
    MockUiBackend mock;
    JsonRpcDispatcher d(mock);
    const json req = {{"jsonrpc","2.0"},{"id",2}};
    const json resp = d.dispatch(req);
    CHECK(resp.at("error").at("code") == kInvalidRequest);
}
```

- [ ] **Step 3: Add files to the test target**

In `tests/automation/CMakeLists.txt`, add `test_dispatcher.cpp` and the source `${CMAKE_SOURCE_DIR}/src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp`. The full source list should now match the complete version shown in Task 1, Step 5.

- [ ] **Step 4: Run to verify FAIL**

Run: `cmake --build . --config RelWithDebInfo --target automation_tests -- -m`
Expected: undefined references to `JsonRpcDispatcher` methods.

- [ ] **Step 5: Implement the dispatcher skeleton (envelope + version + errors)**

`src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp`:

```cpp
#include "JsonRpcDispatcher.hpp"
#include "WidgetSerializer.hpp"
#include "Locator.hpp"
#include <chrono>
#include <thread>

namespace Slic3r { namespace GUI { namespace Automation {

JsonRpcDispatcher::JsonRpcDispatcher(IUiBackend& backend) : m_backend(backend) {}

nlohmann::json JsonRpcDispatcher::make_result(const nlohmann::json& id, nlohmann::json result) {
    return { {"jsonrpc","2.0"}, {"id", id}, {"result", std::move(result)} };
}

nlohmann::json JsonRpcDispatcher::make_error(const nlohmann::json& id, int code,
                                             const std::string& msg) {
    return { {"jsonrpc","2.0"}, {"id", id},
             {"error", { {"code", code}, {"message", msg} }} };
}

nlohmann::json JsonRpcDispatcher::m_version(const nlohmann::json&) {
    return { {"version", kAutomationVersion},
             {"protocol", "2.0"},
             {"capabilities", nlohmann::json::array({
                 "tree.dump","tree.find","widget.get","input.click","input.type",
                 "input.key","sync.wait_for","app.state","screenshot.window",
                 "screenshot.viewport3d" })} };
}

nlohmann::json JsonRpcDispatcher::dispatch(const nlohmann::json& request) {
    nlohmann::json id = request.contains("id") ? request.at("id") : nlohmann::json(nullptr);

    if (!request.is_object() || !request.contains("method") ||
        !request.at("method").is_string()) {
        return make_error(id, kInvalidRequest, "missing or invalid 'method'");
    }
    const std::string method = request.at("method").get<std::string>();
    const nlohmann::json params =
        request.contains("params") ? request.at("params") : nlohmann::json::object();

    try {
        if (method == "automation.version")        return make_result(id, m_version(params));
        if (method == "tree.dump")                 return make_result(id, m_tree_dump(params));
        if (method == "tree.find")                 return make_result(id, m_tree_find(params));
        if (method == "widget.get")                return make_result(id, m_widget_get(params));
        if (method == "input.click")               return make_result(id, m_input_click(params));
        if (method == "input.type")                return make_result(id, m_input_type(params));
        if (method == "input.key")                 return make_result(id, m_input_key(params));
        if (method == "sync.wait_for")             return make_result(id, m_sync_wait_for(params));
        if (method == "app.state")                 return make_result(id, m_app_state(params));
        if (method == "screenshot.window")         return make_result(id, m_screenshot_window(params));
        if (method == "screenshot.viewport3d")     return make_result(id, m_screenshot_viewport3d(params));
        return make_error(id, kMethodNotFound, "unknown method: " + method);
    } catch (const AutomationError& e) {
        return make_error(id, e.code, e.what());
    } catch (const std::exception& e) {
        return make_error(id, kInvalidParams, e.what());
    }
}

std::string JsonRpcDispatcher::handle_request(const std::string& body) {
    nlohmann::json req;
    try {
        req = nlohmann::json::parse(body);
    } catch (const std::exception& e) {
        return make_error(nullptr, kParseError, std::string("parse error: ") + e.what()).dump();
    }
    return dispatch(req).dump();
}

// --- method handlers implemented in Tasks 7-10 (stubs throw for now) ---
nlohmann::json JsonRpcDispatcher::m_tree_dump(const nlohmann::json&)            { throw AutomationError(kMethodNotFound, "not implemented"); }
nlohmann::json JsonRpcDispatcher::m_tree_find(const nlohmann::json&)            { throw AutomationError(kMethodNotFound, "not implemented"); }
nlohmann::json JsonRpcDispatcher::m_widget_get(const nlohmann::json&)           { throw AutomationError(kMethodNotFound, "not implemented"); }
nlohmann::json JsonRpcDispatcher::m_input_click(const nlohmann::json&)          { throw AutomationError(kMethodNotFound, "not implemented"); }
nlohmann::json JsonRpcDispatcher::m_input_type(const nlohmann::json&)           { throw AutomationError(kMethodNotFound, "not implemented"); }
nlohmann::json JsonRpcDispatcher::m_input_key(const nlohmann::json&)            { throw AutomationError(kMethodNotFound, "not implemented"); }
nlohmann::json JsonRpcDispatcher::m_sync_wait_for(const nlohmann::json&)        { throw AutomationError(kMethodNotFound, "not implemented"); }
nlohmann::json JsonRpcDispatcher::m_app_state(const nlohmann::json&)            { throw AutomationError(kMethodNotFound, "not implemented"); }
nlohmann::json JsonRpcDispatcher::m_screenshot_window(const nlohmann::json&)    { throw AutomationError(kMethodNotFound, "not implemented"); }
nlohmann::json JsonRpcDispatcher::m_screenshot_viewport3d(const nlohmann::json&){ throw AutomationError(kMethodNotFound, "not implemented"); }

}}} // namespace
```

- [ ] **Step 6: Run to verify PASS**

Run: `ctest --test-dir tests/automation --output-on-failure`
Expected: version + error envelope cases PASS.

- [ ] **Step 7: Commit**

```bash
git add src/slic3r/GUI/Automation/JsonRpcDispatcher.hpp \
        src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp \
        tests/automation/test_dispatcher.cpp tests/automation/CMakeLists.txt
git commit -m "feat(automation): JSON-RPC dispatcher envelope + version + error model"
```

---

### Task 7: Dispatcher — `tree.dump`, `tree.find`, `widget.get`

**Files:**
- Modify: `src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp`
- Modify: `tests/automation/test_dispatcher.cpp`

- [ ] **Step 1: Add a shared `parse_target` helper + failing tests**

Append to `tests/automation/test_dispatcher.cpp`:

```cpp
namespace {
UiNode dispatcher_tree() {
    UiNode root; root.klass = "MainFrame"; root.path = "MainFrame";
    UiNode b; b.id = "btn_slice"; b.klass = "Button"; b.label = "Slice plate";
    b.path = "MainFrame/Button[0]"; b.rect = {10,20,100,30};
    UiNode e; e.id = "btn_export"; e.klass = "Button"; e.label = "Export";
    e.path = "MainFrame/Button[1]"; e.enabled = false;
    root.children = {b, e};
    return root;
}
} // namespace

TEST_CASE("tree.dump returns the serialized tree", "[automation][rpc]") {
    MockUiBackend mock; mock.tree = dispatcher_tree();
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",1},{"method","tree.dump"}});
    const json& result = resp.at("result");
    CHECK(result.at("class") == "MainFrame");
    CHECK(result.at("children").size() == 2);
    CHECK(mock.refresh_count == 1); // refreshed before reading
}

TEST_CASE("tree.find returns matching nodes", "[automation][rpc]") {
    MockUiBackend mock; mock.tree = dispatcher_tree();
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",2},{"method","tree.find"},
                                  {"params",{{"class","Button"}}}});
    CHECK(resp.at("result").size() == 2);
}

TEST_CASE("widget.get returns a single node by id", "[automation][rpc]") {
    MockUiBackend mock; mock.tree = dispatcher_tree();
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",3},{"method","widget.get"},
                                  {"params",{{"target",{{"id","btn_slice"}}}}}});
    CHECK(resp.at("result").at("id") == "btn_slice");
}

TEST_CASE("widget.get not found -> 1001", "[automation][rpc]") {
    MockUiBackend mock; mock.tree = dispatcher_tree();
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",4},{"method","widget.get"},
                                  {"params",{{"target",{{"id","nope"}}}}}});
    CHECK(resp.at("error").at("code") == kErrNotFound);
}
```

- [ ] **Step 2: Run to verify FAIL**

Run: `ctest --test-dir tests/automation --output-on-failure`
Expected: tree.dump/find/get cases FAIL (still "not implemented").

- [ ] **Step 3: Implement a `parse_target` helper + the three handlers**

In `JsonRpcDispatcher.cpp`, add near the top (after the `make_error` definition):

```cpp
namespace {
std::optional<std::string> opt_str(const nlohmann::json& p, const char* key) {
    if (p.is_object() && p.contains(key) && p.at(key).is_string())
        return p.at(key).get<std::string>();
    return std::nullopt;
}

Target parse_target(const nlohmann::json& tj) {
    Target t;
    if (!tj.is_object()) return t;
    t.id    = opt_str(tj, "id");
    t.path  = opt_str(tj, "path");
    t.name  = opt_str(tj, "name");
    t.klass = opt_str(tj, "class");
    t.label = opt_str(tj, "label");
    t.value = opt_str(tj, "value");
    if (auto b = opt_str(tj, "backend"))
        t.backend = (*b == "imgui") ? BackendKind::ImGui : BackendKind::Wx;
    return t;
}

DumpOptions parse_dump_options(const nlohmann::json& p) {
    DumpOptions o;
    if (p.is_object()) {
        if (p.contains("root"))         o.root = opt_str(p, "root");
        if (p.contains("max_depth") && p.at("max_depth").is_number_integer())
            o.max_depth = p.at("max_depth").get<int>();
        if (p.contains("visible_only") && p.at("visible_only").is_boolean())
            o.visible_only = p.at("visible_only").get<bool>();
        if (p.contains("include_imgui") && p.at("include_imgui").is_boolean())
            o.include_imgui = p.at("include_imgui").get<bool>();
    }
    return o;
}
} // namespace
```

Replace the three stub bodies:

```cpp
nlohmann::json JsonRpcDispatcher::m_tree_dump(const nlohmann::json& params) {
    m_backend.refresh_ui();
    const UiNode root = m_backend.dump_tree(parse_dump_options(params));
    return node_to_json(root, /*include_children*/ true);
}

nlohmann::json JsonRpcDispatcher::m_tree_find(const nlohmann::json& params) {
    m_backend.refresh_ui();
    const UiNode root = m_backend.dump_tree(DumpOptions{});
    const Target target =
        parse_target(params.is_object() ? params : nlohmann::json::object());
    nlohmann::json arr = nlohmann::json::array();
    for (const UiNode* n : find_matches(root, target))
        arr.push_back(node_to_json(*n, /*include_children*/ false));
    return arr;
}

nlohmann::json JsonRpcDispatcher::m_widget_get(const nlohmann::json& params) {
    if (!params.is_object() || !params.contains("target"))
        throw AutomationError(kInvalidParams, "widget.get requires 'target'");
    m_backend.refresh_ui();
    const UiNode root = m_backend.dump_tree(DumpOptions{});
    int count = 0;
    const UiNode* node = resolve_unique(root, parse_target(params.at("target")), count);
    if (count == 0) throw AutomationError(kErrNotFound, "target not found");
    if (count > 1)  throw AutomationError(kErrNotFound, "target is ambiguous");
    return node_to_json(*node, /*include_children*/ true);
}
```

- [ ] **Step 4: Run to verify PASS**

Run: `ctest --test-dir tests/automation --output-on-failure`
Expected: tree.dump/find + widget.get cases PASS.

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp tests/automation/test_dispatcher.cpp
git commit -m "feat(automation): tree.dump / tree.find / widget.get handlers"
```

---

### Task 8: Dispatcher — `input.click`, `input.type`, `input.key`

**Files:**
- Modify: `src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp`
- Modify: `tests/automation/test_dispatcher.cpp`

- [ ] **Step 1: Write failing input tests**

Append to `tests/automation/test_dispatcher.cpp`:

```cpp
TEST_CASE("input.click resolves target and clicks it", "[automation][rpc]") {
    MockUiBackend mock; mock.tree = dispatcher_tree();
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",1},{"method","input.click"},
                                  {"params",{{"target",{{"id","btn_slice"}}}}}});
    CHECK(resp.at("result").at("ok") == true);
    REQUIRE(mock.clicked_ids.size() == 1);
    CHECK(mock.clicked_ids[0] == "btn_slice");
    CHECK(mock.click_buttons[0] == MouseButton::Left);
}

TEST_CASE("input.click on disabled widget -> 1002", "[automation][rpc]") {
    MockUiBackend mock; mock.tree = dispatcher_tree();
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",2},{"method","input.click"},
                                  {"params",{{"target",{{"id","btn_export"}}}}}});
    CHECK(resp.at("error").at("code") == kErrNotActionable);
    CHECK(mock.clicked_ids.empty());
}

TEST_CASE("input.type with target clicks to focus then types", "[automation][rpc]") {
    MockUiBackend mock; mock.tree = dispatcher_tree();
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",3},{"method","input.type"},
                                  {"params",{{"target",{{"id","btn_slice"}}},{"text","hello"}}}});
    CHECK(resp.at("result").at("ok") == true);
    CHECK(mock.clicked_ids.size() == 1);          // focused first
    REQUIRE(mock.typed_text.size() == 1);
    CHECK(mock.typed_text[0] == "hello");
}

TEST_CASE("input.key parses 'ctrl+s' string form", "[automation][rpc]") {
    MockUiBackend mock;
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",4},{"method","input.key"},
                                  {"params",{{"keys","ctrl+s"}}}});
    CHECK(resp.at("result").at("ok") == true);
    REQUIRE(mock.sent_keys.size() == 1);
    REQUIRE(mock.sent_keys[0].size() == 1);
    CHECK(mock.sent_keys[0][0].key == "s");
    REQUIRE(mock.sent_keys[0][0].modifiers.size() == 1);
    CHECK(mock.sent_keys[0][0].modifiers[0] == KeyModifier::Ctrl);
}

TEST_CASE("input.key parses array form [\"ctrl\",\"s\"]", "[automation][rpc]") {
    MockUiBackend mock;
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",5},{"method","input.key"},
                                  {"params",{{"keys", json::array({"ctrl","s"})}}}});
    CHECK(resp.at("result").at("ok") == true);
    REQUIRE(mock.sent_keys[0][0].modifiers.size() == 1);
    CHECK(mock.sent_keys[0][0].key == "s");
}
```

- [ ] **Step 2: Run to verify FAIL**

Run: `ctest --test-dir tests/automation --output-on-failure`
Expected: input cases FAIL.

- [ ] **Step 3: Implement input handlers + a key parser**

In `JsonRpcDispatcher.cpp`, add to the anonymous namespace:

```cpp
namespace {
MouseButton parse_button(const nlohmann::json& p) {
    auto b = opt_str(p, "button");
    if (b && *b == "right")  return MouseButton::Right;
    if (b && *b == "middle") return MouseButton::Middle;
    return MouseButton::Left;
}

std::vector<KeyModifier> parse_modifiers(const nlohmann::json& p) {
    std::vector<KeyModifier> mods;
    if (p.is_object() && p.contains("modifiers") && p.at("modifiers").is_array()) {
        for (const auto& m : p.at("modifiers")) {
            if (!m.is_string()) continue;
            const std::string s = m.get<std::string>();
            if (s == "ctrl")  mods.push_back(KeyModifier::Ctrl);
            else if (s == "shift") mods.push_back(KeyModifier::Shift);
            else if (s == "alt")   mods.push_back(KeyModifier::Alt);
            else if (s == "cmd" || s == "meta") mods.push_back(KeyModifier::Cmd);
        }
    }
    return mods;
}

// Parse one chord token list (already split): the last token is the key, the
// earlier ones are modifiers.
KeyChord chord_from_tokens(const std::vector<std::string>& tokens) {
    KeyChord c;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];
        const bool is_mod = (t == "ctrl" || t == "shift" || t == "alt" ||
                             t == "cmd" || t == "meta");
        if (is_mod && i + 1 < tokens.size()) {
            if (t == "ctrl")  c.modifiers.push_back(KeyModifier::Ctrl);
            else if (t == "shift") c.modifiers.push_back(KeyModifier::Shift);
            else if (t == "alt")   c.modifiers.push_back(KeyModifier::Alt);
            else                   c.modifiers.push_back(KeyModifier::Cmd);
        } else {
            c.key = t; // last token (or a lone token) is the key
        }
    }
    return c;
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == delim) { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur.push_back(ch);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// "keys" may be a string ("ctrl+s") or an array (["ctrl","s"]). Returns one chord.
std::vector<KeyChord> parse_keys(const nlohmann::json& params) {
    if (!params.is_object() || !params.contains("keys"))
        throw AutomationError(kInvalidParams, "input.key requires 'keys'");
    const auto& k = params.at("keys");
    std::vector<std::string> tokens;
    if (k.is_string()) {
        tokens = split(k.get<std::string>(), '+');
    } else if (k.is_array()) {
        for (const auto& e : k)
            if (e.is_string()) tokens.push_back(e.get<std::string>());
    } else {
        throw AutomationError(kInvalidParams, "'keys' must be string or array");
    }
    if (tokens.empty())
        throw AutomationError(kInvalidParams, "'keys' is empty");
    return { chord_from_tokens(tokens) };
}

// Resolve a unique, actionable node from params["target"], or throw.
} // namespace
```

Add a private resolver method. First declare it in `JsonRpcDispatcher.hpp` (in the `private:` section):

```cpp
    const UiNode resolve_actionable(const nlohmann::json& params, UiNode& tree_out);
```

Then implement in `JsonRpcDispatcher.cpp`:

```cpp
const UiNode JsonRpcDispatcher::resolve_actionable(const nlohmann::json& params,
                                                   UiNode& tree_out) {
    if (!params.is_object() || !params.contains("target"))
        throw AutomationError(kInvalidParams, "missing 'target'");
    m_backend.refresh_ui();
    tree_out = m_backend.dump_tree(DumpOptions{});
    int count = 0;
    const UiNode* node = resolve_unique(tree_out, parse_target(params.at("target")), count);
    if (count == 0) throw AutomationError(kErrNotFound, "target not found");
    if (count > 1)  throw AutomationError(kErrNotFound, "target is ambiguous");
    if (!node->enabled || !node->visible)
        throw AutomationError(kErrNotActionable, "target is disabled or hidden");
    return *node; // copy: stable even though tree_out outlives this call
}

nlohmann::json JsonRpcDispatcher::m_input_click(const nlohmann::json& params) {
    UiNode tree;
    const UiNode node = resolve_actionable(params, tree);
    const bool dbl = params.contains("double") && params.at("double").is_boolean()
                         && params.at("double").get<bool>();
    const bool ok = m_backend.click(node, parse_button(params), dbl, parse_modifiers(params));
    return { {"ok", ok} };
}

nlohmann::json JsonRpcDispatcher::m_input_type(const nlohmann::json& params) {
    if (!params.is_object() || !params.contains("text") || !params.at("text").is_string())
        throw AutomationError(kInvalidParams, "input.type requires string 'text'");
    const std::string text = params.at("text").get<std::string>();
    // Optional target: click to focus first.
    if (params.contains("target")) {
        UiNode tree;
        const UiNode node = resolve_actionable(params, tree);
        m_backend.click(node, MouseButton::Left, false, {});
    }
    const bool ok = m_backend.type_text(text);
    return { {"ok", ok} };
}

nlohmann::json JsonRpcDispatcher::m_input_key(const nlohmann::json& params) {
    const bool ok = m_backend.send_keys(parse_keys(params));
    return { {"ok", ok} };
}
```

- [ ] **Step 4: Run to verify PASS**

Run: `ctest --test-dir tests/automation --output-on-failure`
Expected: input cases PASS.

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/Automation/JsonRpcDispatcher.hpp \
        src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp tests/automation/test_dispatcher.cpp
git commit -m "feat(automation): input.click / input.type / input.key handlers"
```

---

### Task 9: Dispatcher — `app.state`, `screenshot.window`, `screenshot.viewport3d`

**Files:**
- Modify: `src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp`
- Modify: `tests/automation/test_dispatcher.cpp`

- [ ] **Step 1: Write failing tests**

Append to `tests/automation/test_dispatcher.cpp`:

```cpp
#include <catch2/catch_all.hpp> // already included; harmless if duplicated guard

TEST_CASE("app.state returns serialized state", "[automation][rpc]") {
    MockUiBackend mock;
    mock.state.active_tab = "prepare"; mock.state.project_loaded = true;
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",1},{"method","app.state"}});
    CHECK(resp.at("result").at("active_tab") == "prepare");
    CHECK(resp.at("result").at("project_loaded") == true);
}

TEST_CASE("screenshot.window returns base64 + dims", "[automation][rpc]") {
    MockUiBackend mock;
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",2},{"method","screenshot.window"}});
    CHECK(mock.screenshot_window_count == 1);
    CHECK(resp.at("result").at("width") == 4);
    CHECK(resp.at("result").at("png_base64").is_string());
    CHECK_FALSE(resp.at("result").at("png_base64").get<std::string>().empty());
}

TEST_CASE("screenshot.viewport3d returns base64 + dims", "[automation][rpc]") {
    MockUiBackend mock;
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",3},{"method","screenshot.viewport3d"},
                                  {"params",{{"width",256},{"height",256}}}});
    CHECK(mock.screenshot_viewport_count == 1);
    CHECK(resp.at("result").at("png_base64").is_string());
}
```

- [ ] **Step 2: Run to verify FAIL**

Run: `ctest --test-dir tests/automation --output-on-failure`
Expected: app.state/screenshot cases FAIL.

- [ ] **Step 3: Implement handlers + a base64 encoder**

In `JsonRpcDispatcher.cpp` anonymous namespace, add a small base64 encoder (self-contained — avoids a new dependency):

```cpp
namespace {
std::string base64_encode(const std::vector<unsigned char>& data) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const unsigned n = (data[i] << 16) | (data[i+1] << 8) | data[i+2];
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(tbl[(n >> 6) & 63]);
        out.push_back(tbl[n & 63]);
    }
    if (i < data.size()) {
        unsigned n = data[i] << 16;
        const bool two = (i + 1 < data.size());
        if (two) n |= data[i+1] << 8;
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(two ? tbl[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::optional<int> opt_int(const nlohmann::json& p, const char* key) {
    if (p.is_object() && p.contains(key) && p.at(key).is_number_integer())
        return p.at(key).get<int>();
    return std::nullopt;
}

nlohmann::json image_to_json(const PngImage& img) {
    if (img.png.empty())
        throw AutomationError(kErrScreenshotFail, "screenshot produced no data");
    return { {"png_base64", base64_encode(img.png)},
             {"width", img.width}, {"height", img.height} };
}
} // namespace
```

Replace the stub bodies:

```cpp
nlohmann::json JsonRpcDispatcher::m_app_state(const nlohmann::json&) {
    return app_state_to_json(m_backend.app_state());
}

nlohmann::json JsonRpcDispatcher::m_screenshot_window(const nlohmann::json& params) {
    m_backend.refresh_ui();
    const UiNode* target_ptr = nullptr;
    UiNode resolved;
    if (params.is_object() && params.contains("target")) {
        UiNode tree = m_backend.dump_tree(DumpOptions{});
        int count = 0;
        const UiNode* n = resolve_unique(tree, parse_target(params.at("target")), count);
        if (count == 0) throw AutomationError(kErrNotFound, "target not found");
        if (count > 1)  throw AutomationError(kErrNotFound, "target is ambiguous");
        resolved = *n;
        target_ptr = &resolved;
    }
    return image_to_json(m_backend.screenshot_window(target_ptr));
}

nlohmann::json JsonRpcDispatcher::m_screenshot_viewport3d(const nlohmann::json& params) {
    return image_to_json(m_backend.screenshot_viewport3d(
        opt_int(params, "plate"), opt_int(params, "width"), opt_int(params, "height")));
}
```

- [ ] **Step 4: Run to verify PASS**

Run: `ctest --test-dir tests/automation --output-on-failure`
Expected: app.state/screenshot cases PASS.

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp tests/automation/test_dispatcher.cpp
git commit -m "feat(automation): app.state + screenshot handlers with base64"
```

---

### Task 10: Dispatcher — `sync.wait_for` (poll loop)

**Files:**
- Modify: `src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp`
- Modify: `tests/automation/test_dispatcher.cpp`

- [ ] **Step 1: Write failing tests using a scripted tree provider**

Append to `tests/automation/test_dispatcher.cpp`:

```cpp
TEST_CASE("sync.wait_for succeeds once the condition holds", "[automation][rpc]") {
    MockUiBackend mock;
    // First 2 polls: btn disabled. 3rd poll: enabled.
    mock.tree_provider = [](int call) {
        UiNode root; root.klass = "MainFrame"; root.path = "MainFrame";
        UiNode b; b.id = "btn_slice"; b.klass = "Button"; b.path = "MainFrame/Button[0]";
        b.visible = true; b.enabled = (call >= 2);
        root.children = {b};
        return root;
    };
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",1},{"method","sync.wait_for"},
        {"params",{{"target",{{"id","btn_slice"}}},{"state","enabled"},
                   {"timeout_ms",2000},{"poll_ms",1}}}});
    CHECK(resp.at("result").at("ok") == true);
    CHECK(mock.dump_count >= 3);
}

TEST_CASE("sync.wait_for times out -> 1003", "[automation][rpc]") {
    MockUiBackend mock;
    mock.tree_provider = [](int) {
        UiNode root; root.klass = "MainFrame"; root.path = "MainFrame";
        UiNode b; b.id = "btn_slice"; b.visible = true; b.enabled = false;
        b.path = "MainFrame/Button[0]";
        root.children = {b};
        return root;
    };
    JsonRpcDispatcher d(mock);
    const json resp = d.dispatch({{"jsonrpc","2.0"},{"id",2},{"method","sync.wait_for"},
        {"params",{{"target",{{"id","btn_slice"}}},{"state","enabled"},
                   {"timeout_ms",30},{"poll_ms",5}}}});
    CHECK(resp.at("error").at("code") == kErrWaitTimeout);
}
```

- [ ] **Step 2: Run to verify FAIL**

Run: `ctest --test-dir tests/automation --output-on-failure`
Expected: wait_for cases FAIL.

- [ ] **Step 3: Implement `m_sync_wait_for`**

Replace the stub in `JsonRpcDispatcher.cpp`:

```cpp
nlohmann::json JsonRpcDispatcher::m_sync_wait_for(const nlohmann::json& params) {
    if (!params.is_object() || !params.contains("target") || !params.contains("state"))
        throw AutomationError(kInvalidParams, "sync.wait_for requires 'target' and 'state'");

    const Target target = parse_target(params.at("target"));
    const std::string state_s = params.at("state").get<std::string>();
    WaitState state;
    if      (state_s == "exists")  state = WaitState::Exists;
    else if (state_s == "visible") state = WaitState::Visible;
    else if (state_s == "enabled") state = WaitState::Enabled;
    else if (state_s == "value")   state = WaitState::Value;
    else throw AutomationError(kInvalidParams, "unknown state: " + state_s);

    std::optional<std::string> expected = opt_str(params, "value");
    const int timeout_ms = params.contains("timeout_ms") && params.at("timeout_ms").is_number_integer()
                               ? params.at("timeout_ms").get<int>() : 5000;
    const int poll_ms = params.contains("poll_ms") && params.at("poll_ms").is_number_integer()
                            ? std::max(1, params.at("poll_ms").get<int>()) : 100;

    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        m_backend.refresh_ui();
        const UiNode root = m_backend.dump_tree(DumpOptions{});
        int count = 0;
        const UiNode* node = resolve_unique(root, target, &count == nullptr ? count : count);
        // (count is filled by resolve_unique; node is null when not unique)
        if (evaluate_state(node, state, expected)) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            return { {"ok", true}, {"elapsed_ms", static_cast<int>(elapsed)} };
        }
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start).count();
        if (elapsed_ms >= timeout_ms)
            throw AutomationError(kErrWaitTimeout, "wait_for timed out for state: " + state_s);
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
}
```

> NOTE: simplify the `resolve_unique` call to `const UiNode* node = resolve_unique(root, target, count);` — the inline ternary above is a no-op artifact; write it cleanly:
> ```cpp
> int count = 0;
> const UiNode* node = resolve_unique(root, target, count);
> ```

- [ ] **Step 4: Run to verify PASS**

Run: `ctest --test-dir tests/automation --output-on-failure`
Expected: wait_for cases PASS, full automation suite green.

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/Automation/JsonRpcDispatcher.cpp tests/automation/test_dispatcher.cpp
git commit -m "feat(automation): sync.wait_for poll loop"
```

---

## PHASE 2 — AutomationServer (boost::beast, localhost POST + body)

### Task 11: AutomationServer

**Files:**
- Create: `src/slic3r/GUI/Automation/AutomationServer.hpp`
- Create: `src/slic3r/GUI/Automation/AutomationServer.cpp`

> This component owns a boost::asio thread and a beast acceptor; it is verified by the e2e example (`example_slice.py`) and by a manual `curl` smoke test (Step 4). Unlike the auth `HttpServer`, it uses beast's request parser so the POST body is available. Modeled on `HttpServer` (`src/slic3r/GUI/HttpServer.cpp:110-171`) for the accept/thread lifecycle, but uses `http::read`/`http::write` for a clean request/response cycle.

- [ ] **Step 1: Create the header**

`src/slic3r/GUI/Automation/AutomationServer.hpp`:

```cpp
#pragma once
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace Slic3r { namespace GUI { namespace Automation {

// Localhost-only HTTP/1.1 server. POST /jsonrpc -> handler(body) -> response body.
// GET / -> a tiny health/version page. The handler runs on the server's own
// io thread; it is responsible for any further thread marshaling.
class AutomationServer {
public:
    using RequestHandler = std::function<std::string(const std::string& body)>;

    explicit AutomationServer(unsigned short port);
    ~AutomationServer();

    void set_handler(RequestHandler handler) { m_handler = std::move(handler); }
    void set_health_text(std::string text)   { m_health = std::move(text); }

    void start();           // binds to 127.0.0.1:port, starts the io thread
    void stop();            // stops the io thread, joins
    bool is_started() const { return m_started; }
    unsigned short port() const { return m_port; }

private:
    void do_accept();
    void handle_session(boost::asio::ip::tcp::socket socket);

    unsigned short                                m_port;
    std::atomic<bool>                             m_started{false};
    RequestHandler                                m_handler;
    std::string                                   m_health{"OrcaSlicer automation server"};
    std::unique_ptr<boost::asio::io_context>      m_ioc;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> m_acceptor;
    boost::thread                                 m_thread;
};

}}} // namespace
```

- [ ] **Step 2: Implement the server**

`src/slic3r/GUI/Automation/AutomationServer.cpp`:

```cpp
#include "AutomationServer.hpp"
#include "libslic3r/Utils.hpp" // create_thread / set_current_thread_name

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/log/trivial.hpp>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;

namespace Slic3r { namespace GUI { namespace Automation {

AutomationServer::AutomationServer(unsigned short port) : m_port(port) {}

AutomationServer::~AutomationServer() { stop(); }

void AutomationServer::start() {
    if (m_started) return;
    m_ioc = std::make_unique<net::io_context>(1);
    // Bind to loopback ONLY.
    tcp::endpoint endpoint(net::ip::make_address("127.0.0.1"), m_port);
    m_acceptor = std::make_unique<tcp::acceptor>(*m_ioc);
    m_acceptor->open(endpoint.protocol());
    m_acceptor->set_option(net::socket_base::reuse_address(true));
    m_acceptor->bind(endpoint);
    m_acceptor->listen(net::socket_base::max_listen_connections);
    m_started = true;

    do_accept();

    net::io_context* ioc = m_ioc.get();
    m_thread = create_thread([ioc] {
        set_current_thread_name("orca_automation");
        ioc->run();
    });
    BOOST_LOG_TRIVIAL(info) << "AutomationServer listening on 127.0.0.1:" << m_port;
}

void AutomationServer::stop() {
    if (!m_started) return;
    m_started = false;
    if (m_ioc) m_ioc->stop();
    if (m_thread.joinable()) m_thread.join();
    m_acceptor.reset();
    m_ioc.reset();
}

void AutomationServer::do_accept() {
    m_acceptor->async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (!ec) {
            // v1: single-client, serialized — handle synchronously on the io thread.
            handle_session(std::move(socket));
        }
        if (m_started && m_acceptor && m_acceptor->is_open())
            do_accept();
    });
}

void AutomationServer::handle_session(tcp::socket socket) {
    beast::error_code ec;
    beast::flat_buffer buffer;
    http::request<http::string_body> req;
    http::read(socket, buffer, req, ec);
    if (ec) { socket.shutdown(tcp::socket::shutdown_send, ec); return; }

    http::response<http::string_body> res;
    res.version(req.version());
    res.keep_alive(false);

    if (req.method() == http::verb::post && req.target() == "/jsonrpc") {
        std::string body_out;
        try {
            body_out = m_handler ? m_handler(req.body())
                                 : R"({"jsonrpc":"2.0","id":null,"error":{"code":-32603,"message":"no handler"}})";
        } catch (const std::exception& e) {
            body_out = std::string(R"({"jsonrpc":"2.0","id":null,"error":{"code":-32603,"message":")")
                       + e.what() + R"("}})";
        }
        res.result(http::status::ok);
        res.set(http::field::content_type, "application/json");
        res.body() = std::move(body_out);
    } else if (req.method() == http::verb::get && req.target() == "/") {
        res.result(http::status::ok);
        res.set(http::field::content_type, "text/plain");
        res.body() = m_health;
    } else {
        res.result(http::status::not_found);
        res.set(http::field::content_type, "text/plain");
        res.body() = "not found";
    }
    res.set(http::field::server, "OrcaSlicer/automation");
    res.prepare_payload();
    http::write(socket, res, ec);
    socket.shutdown(tcp::socket::shutdown_send, ec);
}

}}} // namespace
```

- [ ] **Step 3: Wire into the GUI build (production target)**

In `src/slic3r/CMakeLists.txt`, locate the `SLIC3R_GUI_SOURCES` list (the GUI source list around the Gizmos/Jobs entries, parent file confirmed at `src/slic3r/CMakeLists.txt`). Add these lines alongside the other `GUI/...` entries:

```cmake
    GUI/Automation/IUiBackend.hpp
    GUI/Automation/WidgetSerializer.cpp
    GUI/Automation/WidgetSerializer.hpp
    GUI/Automation/Locator.cpp
    GUI/Automation/Locator.hpp
    GUI/Automation/JsonRpcDispatcher.cpp
    GUI/Automation/JsonRpcDispatcher.hpp
    GUI/Automation/AutomationServer.cpp
    GUI/Automation/AutomationServer.hpp
```

(The remaining `WxUiBackend`, `AutomationRegistry`, `ImGuiItemTable` sources are added in Tasks 12–16.)

- [ ] **Step 4: Build the GUI target and manually smoke-test the server**

Build OrcaSlicer (or just the GUI lib) — it should compile and link with the new server. The server is not yet started by the app (that comes in Task 17), so verify compilation only here:
```
cmake --build . --config RelWithDebInfo --target OrcaSlicer -- -m
```
Expected: clean build (no link errors). Full server smoke test (curl) happens after Task 17.

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/Automation/AutomationServer.hpp \
        src/slic3r/GUI/Automation/AutomationServer.cpp src/slic3r/CMakeLists.txt
git commit -m "feat(automation): localhost beast POST /jsonrpc server"
```

---

## PHASE 3 — GUI backend, ImGui recording, lifecycle (manual verification; needs display)

> Tasks 12–19 touch live wx/ImGui/GL code and cannot run in the display-free CI unit tests. Each is verified by **building** and by the **manual e2e** in Task 22. Keep all new behavior behind `wxGetApp().is_automation_enabled()` so a disabled build is unchanged (verified in Task 24).

### Task 12: AutomationRegistry

**Files:**
- Create: `src/slic3r/GUI/Automation/AutomationRegistry.hpp`
- Create: `src/slic3r/GUI/Automation/AutomationRegistry.cpp`
- Modify: `src/slic3r/CMakeLists.txt` (add the two files to `SLIC3R_GUI_SOURCES`)

- [ ] **Step 1: Create the header**

`src/slic3r/GUI/Automation/AutomationRegistry.hpp`:

```cpp
#pragma once
#include <cstdint>
#include <string>

class wxWindow;

namespace Slic3r { namespace GUI { namespace Automation {

// Process-wide wxWindow* <-> automation_id side map. Header is dependency-light so
// widget-construction code can call set_automation_id() unconditionally — it is a
// cheap, safe registration that no-ops when the window is null.
//
// Registration is pruned automatically when the window is destroyed (bound to
// wxEVT_DESTROY inside set_automation_id).
void        set_automation_id(wxWindow* window, const std::string& id);
std::string automation_id_of(const wxWindow* window);   // "" if none
wxWindow*   window_for_automation_id(const std::string& id); // nullptr if none

}}} // namespace
```

- [ ] **Step 2: Implement the registry**

`src/slic3r/GUI/Automation/AutomationRegistry.cpp`:

```cpp
#include "AutomationRegistry.hpp"
#include <wx/window.h>
#include <wx/event.h>
#include <mutex>
#include <unordered_map>

namespace Slic3r { namespace GUI { namespace Automation {

namespace {
std::mutex& mtx() { static std::mutex m; return m; }
std::unordered_map<const wxWindow*, std::string>& fwd() {
    static std::unordered_map<const wxWindow*, std::string> m; return m;
}
std::unordered_map<std::string, wxWindow*>& rev() {
    static std::unordered_map<std::string, wxWindow*> m; return m;
}
void erase_window(const wxWindow* w) {
    std::lock_guard<std::mutex> lk(mtx());
    auto it = fwd().find(w);
    if (it != fwd().end()) { rev().erase(it->second); fwd().erase(it); }
}
} // namespace

void set_automation_id(wxWindow* window, const std::string& id) {
    if (window == nullptr || id.empty()) return;
    {
        std::lock_guard<std::mutex> lk(mtx());
        fwd()[window] = id;
        rev()[id]     = window;
    }
    // Prune on destruction.
    window->Bind(wxEVT_DESTROY, [window](wxWindowDestroyEvent& e) {
        erase_window(window);
        e.Skip();
    });
}

std::string automation_id_of(const wxWindow* window) {
    std::lock_guard<std::mutex> lk(mtx());
    auto it = fwd().find(window);
    return it == fwd().end() ? std::string() : it->second;
}

wxWindow* window_for_automation_id(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx());
    auto it = rev().find(id);
    return it == rev().end() ? nullptr : it->second;
}

}}} // namespace
```

- [ ] **Step 3: Add to the GUI build**

In `src/slic3r/CMakeLists.txt`, add to `SLIC3R_GUI_SOURCES`:
```cmake
    GUI/Automation/AutomationRegistry.cpp
    GUI/Automation/AutomationRegistry.hpp
```

- [ ] **Step 4: Build to verify it compiles/links**

Run: `cmake --build . --config RelWithDebInfo --target OrcaSlicer -- -m`
Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/Automation/AutomationRegistry.hpp \
        src/slic3r/GUI/Automation/AutomationRegistry.cpp src/slic3r/CMakeLists.txt
git commit -m "feat(automation): wxWindow automation-id registry"
```

---

### Task 13: ImGuiItemTable (double-buffered per-frame recorder)

**Files:**
- Create: `src/slic3r/GUI/Automation/ImGuiItemTable.hpp`
- Create: `src/slic3r/GUI/Automation/ImGuiItemTable.cpp`
- Modify: `src/slic3r/CMakeLists.txt`

- [ ] **Step 1: Create the header**

`src/slic3r/GUI/Automation/ImGuiItemTable.hpp`:

```cpp
#pragma once
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Automation {

// One recorded ImGui item. Rect is in ImGui display coords; WxUiBackend maps it
// to screen coords using the canvas client origin + DPI scale.
struct ImGuiItemRecord {
    std::string window_name;
    std::string label;   // visible label / id
    std::string type;    // "button", "checkbox", "combo", "slider", "input", ...
    float       x = 0, y = 0, w = 0, h = 0;
    bool        enabled = true;
    bool        has_value = false;
    std::string value;
};

// A complete recorded frame: items + window-level info.
struct ImGuiWindowRecord {
    std::string name;
    float       x = 0, y = 0, w = 0, h = 0;
    bool        visible = true;
};

struct ImGuiFrameRecord {
    std::vector<ImGuiItemRecord>   items;
    std::vector<ImGuiWindowRecord> windows;
};

// Double-buffered recorder. The drawing code appends to the "back" frame; render()
// swaps it to "front" at frame end. Readers (GUI thread, after marshaling) read the
// front frame. All access is on the GUI thread, but we guard with a mutex anyway
// because the automation read may happen between frames.
class ImGuiItemTable {
public:
    static ImGuiItemTable& instance();

    // Called from ImGuiWrapper drawing hooks (GUI thread). No-op cheap append.
    void record_item(ImGuiItemRecord rec);
    void record_window(ImGuiWindowRecord rec);

    // Called at frame end (ImGuiWrapper::render). Promotes back -> front, clears back.
    void swap_frame();

    // Snapshot the latest complete frame for the backend to read.
    ImGuiFrameRecord snapshot() const;

private:
    mutable std::mutex m_mutex;
    ImGuiFrameRecord   m_back;   // accumulating
    ImGuiFrameRecord   m_front;  // last complete
};

}}} // namespace
```

- [ ] **Step 2: Implement it**

`src/slic3r/GUI/Automation/ImGuiItemTable.cpp`:

```cpp
#include "ImGuiItemTable.hpp"

namespace Slic3r { namespace GUI { namespace Automation {

ImGuiItemTable& ImGuiItemTable::instance() {
    static ImGuiItemTable t;
    return t;
}

void ImGuiItemTable::record_item(ImGuiItemRecord rec) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_back.items.push_back(std::move(rec));
}

void ImGuiItemTable::record_window(ImGuiWindowRecord rec) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_back.windows.push_back(std::move(rec));
}

void ImGuiItemTable::swap_frame() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_front = std::move(m_back);
    m_back  = ImGuiFrameRecord{};
}

ImGuiFrameRecord ImGuiItemTable::snapshot() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_front;
}

}}} // namespace
```

- [ ] **Step 3: Add to the GUI build**

In `src/slic3r/CMakeLists.txt`, add:
```cmake
    GUI/Automation/ImGuiItemTable.cpp
    GUI/Automation/ImGuiItemTable.hpp
```

- [ ] **Step 4: Build to verify**

Run: `cmake --build . --config RelWithDebInfo --target OrcaSlicer -- -m`
Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/Automation/ImGuiItemTable.hpp \
        src/slic3r/GUI/Automation/ImGuiItemTable.cpp src/slic3r/CMakeLists.txt
git commit -m "feat(automation): double-buffered ImGui item table"
```

---

### Task 14: ImGuiWrapper recording hooks (guarded)

**Files:**
- Modify: `src/slic3r/GUI/ImGuiWrapper.hpp` (declare a private helper)
- Modify: `src/slic3r/GUI/ImGuiWrapper.cpp` (hooks in wrapped methods + `render()`)

> All hooks are guarded by `wxGetApp().is_automation_enabled()` (added in Task 17). When automation is off, the guard short-circuits to a single bool check — no allocation, no behavior change (spec §10, verified in Task 24). Insertion points are the exact post-widget locations confirmed in the codebase: `button` (~`ImGuiWrapper.cpp:872`), `checkbox` (~`:1003`), `combo` (~`:1326`), `slider_float` (~`:1149`), `render()` (`:573-578`).

- [ ] **Step 1: Declare the recording helper in the header**

In `src/slic3r/GUI/ImGuiWrapper.hpp`, inside the `private:` section of `class ImGuiWrapper` (near the other private members around line 389), add:

```cpp
    // Automation recording: appends the most-recently-drawn ImGui item to the
    // automation item table. No-op (single bool check) when automation is disabled.
    void automation_record_last_item(const char* type, const std::string& label,
                                     bool has_value, const std::string& value);
```

- [ ] **Step 2: Implement the helper in the .cpp**

At the top of `src/slic3r/GUI/ImGuiWrapper.cpp`, add includes near the existing ones (after the `imgui_internal.h` include at line 26):

```cpp
#include "slic3r/GUI/Automation/ImGuiItemTable.hpp"
#include "slic3r/GUI/GUI_App.hpp"
```

Then add the helper implementation (place it next to `render()`):

```cpp
void ImGuiWrapper::automation_record_last_item(const char* type, const std::string& label,
                                               bool has_value, const std::string& value) {
    if (!wxGetApp().is_automation_enabled())
        return;
    using namespace Slic3r::GUI::Automation;
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    ImGuiItemRecord rec;
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    rec.window_name = (ctx && ctx->CurrentWindow) ? ctx->CurrentWindow->Name : "";
    rec.label   = label;
    rec.type    = type;
    rec.x = mn.x; rec.y = mn.y; rec.w = mx.x - mn.x; rec.h = mx.y - mn.y;
    rec.enabled = !ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled
                      ? true : true; // keep simple: enabled unless explicitly disabled
    rec.has_value = has_value;
    rec.value     = value;
    ImGuiItemTable::instance().record_item(std::move(rec));
}
```

> NOTE on `enabled`: a precise disabled-state read is non-trivial across ImGui versions. v1 records `enabled = true` for recorded items; refine later if needed. Replace the awkward line above with simply `rec.enabled = true;`.

- [ ] **Step 3: Add hooks to the wrapped methods**

`button` — after `const bool ret = ImGui::Button(label_utf8.c_str());` (~line 872):
```cpp
    automation_record_last_item("button", label_utf8, false, {});
```

`bbl_button` — after the `ImGui::BBLButton(...)` call (~line 885), mirror the same:
```cpp
    automation_record_last_item("button", label_utf8, false, {});
```

`checkbox` — change the body (~lines 1000-1004) to capture the result before returning:
```cpp
bool ImGuiWrapper::checkbox(const wxString &label, bool &value)
{
    auto label_utf8 = into_u8(label);
    const bool ret = ImGui::Checkbox(label_utf8.c_str(), &value);
    automation_record_last_item("checkbox", label_utf8, true, value ? "true" : "false");
    return ret;
}
```

`bbl_checkbox` — mirror the same pattern (capture `ret`, record `"checkbox"`, then return).

`combo` (~line 1326, before `return res;`):
```cpp
    {
        const std::string cur = (selection >= 0 && selection < (int)options.size())
                                    ? options[selection] : std::string();
        automation_record_last_item("combo", label, true, cur);
    }
    return res;
```

`slider_float` (~after line 1149, after the `m_last_slider_status` block):
```cpp
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), format ? format : "%.3f", v ? *v : 0.f);
        automation_record_last_item("slider", str_label, true, buf);
    }
```

`input_double` (~line 972, after the `ImGui::InputDouble(...)` call): add
```cpp
    automation_record_last_item("input", into_u8(label), true, into_u8(value_str));
```
(Use whatever local string already holds the formatted value; if none, format `value` with `format`.)

`radio_button` (~line 963, after `ImGui::RadioButton(...)`):
```cpp
    automation_record_last_item("radio", into_u8(label), true, active ? "true" : "false");
```

`menu_item_with_icon` — this is a free function (line 47/~1749), not a member; skip the member helper here. Record only if trivially feasible; otherwise leave for future per-item work (documented limitation). For v1, **skip** instrumenting `menu_item_with_icon` (window-level coverage applies).

- [ ] **Step 4: Add window enumeration + frame swap in `render()`**

Modify `render()` (`src/slic3r/GUI/ImGuiWrapper.cpp:573-578`):

```cpp
void ImGuiWrapper::render()
{
    ImGui::Render();
    render_draw_data(ImGui::GetDrawData());
    if (wxGetApp().is_automation_enabled()) {
        using namespace Slic3r::GUI::Automation;
        ImGuiContext& g = *ImGui::GetCurrentContext();
        for (ImGuiWindow* w : g.Windows) {
            if (w == nullptr) continue;
            ImGuiWindowRecord wr;
            wr.name = w->Name ? w->Name : "";
            wr.x = w->Pos.x; wr.y = w->Pos.y; wr.w = w->Size.x; wr.h = w->Size.y;
            wr.visible = w->Active && !w->Hidden;
            ImGuiItemTable::instance().record_window(std::move(wr));
        }
        ImGuiItemTable::instance().swap_frame();
    }
    m_new_frame_open = false;
}
```

- [ ] **Step 5: Build and verify**

Run: `cmake --build . --config RelWithDebInfo --target OrcaSlicer -- -m`
Expected: clean build. (Behavioral verification happens in Task 22; disabled-overhead check in Task 24.)

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/ImGuiWrapper.hpp src/slic3r/GUI/ImGuiWrapper.cpp
git commit -m "feat(automation): guarded ImGui item/window recording hooks"
```

---

### Task 15: WxUiBackend — GUI-thread marshaller, dump_tree, app_state

**Files:**
- Create: `src/slic3r/GUI/Automation/WxUiBackend.hpp`
- Create: `src/slic3r/GUI/Automation/WxUiBackend.cpp`
- Modify: `src/slic3r/CMakeLists.txt`

- [ ] **Step 1: Create the header**

`src/slic3r/GUI/Automation/WxUiBackend.hpp`:

```cpp
#pragma once
#include "IUiBackend.hpp"

namespace Slic3r { namespace GUI { namespace Automation {

// Real backend. Every public method marshals its work onto the GUI thread via
// wxGetApp().CallAfter + a std::future with a per-call timeout (error 1004 on
// timeout). Walks the wxWindow tree, reads the ImGui item table, drives
// wxUIActionSimulator, captures screenshots.
class WxUiBackend : public IUiBackend {
public:
    explicit WxUiBackend(int gui_timeout_ms = 5000) : m_gui_timeout_ms(gui_timeout_ms) {}

    void     refresh_ui() override;
    UiNode   dump_tree(const DumpOptions& opts) override;
    AppState app_state() override;
    bool     click(const UiNode& node, MouseButton button, bool dbl,
                   const std::vector<KeyModifier>& modifiers) override;
    bool     type_text(const std::string& text) override;
    bool     send_keys(const std::vector<KeyChord>& chords) override;
    PngImage screenshot_window(const UiNode* target) override;
    PngImage screenshot_viewport3d(std::optional<int> plate, std::optional<int> width,
                                   std::optional<int> height) override;

private:
    int m_gui_timeout_ms;
};

}}} // namespace
```

- [ ] **Step 2: Implement the marshaller + dump_tree + app_state**

`src/slic3r/GUI/Automation/WxUiBackend.cpp`:

```cpp
#include "WxUiBackend.hpp"
#include "AutomationRegistry.hpp"
#include "ImGuiItemTable.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <wx/window.h>
#include <wx/toplevel.h>

#include <chrono>
#include <future>
#include <memory>

namespace Slic3r { namespace GUI { namespace Automation {

// Run `fn` on the GUI thread, block until it returns or the timeout elapses.
// Throws AutomationError(1004) on timeout. std::promise is move-only, so we hold
// it via shared_ptr to satisfy CallAfter's copyable-functor requirement.
template <class Fn>
static auto run_on_gui(int timeout_ms, Fn&& fn) -> decltype(fn()) {
    using R = decltype(fn());
    auto prom = std::make_shared<std::promise<R>>();
    auto fut  = prom->get_future();
    wxGetApp().CallAfter([prom, fn = std::forward<Fn>(fn)]() mutable {
        try {
            if constexpr (std::is_void_v<R>) { fn(); prom->set_value(); }
            else { prom->set_value(fn()); }
        } catch (...) { prom->set_exception(std::current_exception()); }
    });
    if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready)
        throw AutomationError(kErrGuiBusy_placeholder, "GUI thread timed out");
    return fut.get();
}

// --- wx tree walking (runs on GUI thread) ---
namespace {
std::string wx_class_name(const wxWindow* w) {
    const wxClassInfo* ci = w->GetClassInfo();
    std::string name = ci ? std::string(ci->GetClassName().ToUTF8()) : "wxWindow";
    // Strip the "wx" prefix for friendlier class names ("wxButton" -> "Button").
    if (name.rfind("wx", 0) == 0 && name.size() > 2) name = name.substr(2);
    return name;
}

std::string wx_value_of(wxWindow* w, bool& has_value) {
    has_value = false;
    if (auto* tc = dynamic_cast<wxTextEntry*>(w))   { has_value = true; return std::string(tc->GetValue().ToUTF8()); }
    if (auto* ch = dynamic_cast<wxChoice*>(w))      { has_value = true; return std::string(ch->GetStringSelection().ToUTF8()); }
    if (auto* cb = dynamic_cast<wxCheckBox*>(w))    { has_value = true; return cb->GetValue() ? "true" : "false"; }
    return {};
}

void build_node(wxWindow* w, UiNode& node, const std::string& parent_path,
                int sibling_index, const DumpOptions& opts, int depth) {
    node.backend = BackendKind::Wx;
    node.klass   = wx_class_name(w);
    node.id      = automation_id_of(w);
    node.path    = parent_path.empty()
                       ? node.klass
                       : parent_path + "/" + node.klass + "[" + std::to_string(sibling_index) + "]";
    node.label   = std::string(w->GetLabel().ToUTF8());
    node.enabled = w->IsEnabled();
    node.visible = w->IsShownOnScreen();
    node.value   = wx_value_of(w, node.has_value);
    node.handle  = reinterpret_cast<std::uint64_t>(w);
    const wxRect r = w->GetScreenRect();
    node.rect = { r.x, r.y, r.width, r.height };

    if (opts.max_depth >= 0 && depth >= opts.max_depth) return;
    int idx = 0;
    for (wxWindow* child : w->GetChildren()) {
        if (opts.visible_only && !child->IsShownOnScreen()) { ++idx; continue; }
        UiNode cn;
        build_node(child, cn, node.path, idx, opts, depth + 1);
        node.children.push_back(std::move(cn));
        ++idx;
    }
}

// Map the recorded ImGui items (display coords) to screen coords using the 3D
// canvas client origin, then append them under the tree root as flat children.
void append_imgui_nodes(UiNode& root) {
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr) return;
    wxWindow* canvas = plater->canvas3D_widget(); // see NOTE below
    if (canvas == nullptr) return;
    const wxPoint origin = canvas->ClientToScreen(wxPoint(0, 0));
    const double scale = canvas->GetContentScaleFactor();

    const auto frame = ImGuiItemTable::instance().snapshot();
    for (const auto& it : frame.items) {
        UiNode n;
        n.backend   = BackendKind::ImGui;
        n.klass     = it.type;
        n.label     = it.label;
        n.path      = "ImGui/" + it.window_name + "/" + it.label;
        n.id        = n.path; // imgui items use their path as id in v1
        n.enabled   = it.enabled;
        n.visible   = true;
        n.has_value = it.has_value;
        n.value     = it.value;
        n.rect = { origin.x + int(it.x / scale), origin.y + int(it.y / scale),
                   int(it.w / scale), int(it.h / scale) };
        root.children.push_back(std::move(n));
    }
}
} // namespace

void WxUiBackend::refresh_ui() {
    run_on_gui(m_gui_timeout_ms, [] {
        // Force a fresh ImGui frame so transient items are recorded, then flush
        // pending events so the latest frame is the one we read.
        if (Plater* p = wxGetApp().plater())
            p->get_current_canvas3D()->set_as_dirty();
        if (Plater* p = wxGetApp().plater())
            p->get_current_canvas3D()->render();
        wxGetApp().Yield();
    });
}

UiNode WxUiBackend::dump_tree(const DumpOptions& opts) {
    return run_on_gui(m_gui_timeout_ms, [&opts]() -> UiNode {
        wxWindow* root_win = nullptr;
        if (opts.root) {
            root_win = window_for_automation_id(*opts.root);
        }
        if (root_win == nullptr)
            root_win = static_cast<wxWindow*>(wxGetApp().mainframe);
        UiNode root;
        if (root_win) build_node(root_win, root, {}, 0, opts, 0);
        if (opts.include_imgui) append_imgui_nodes(root);
        return root;
    });
}

AppState WxUiBackend::app_state() {
    return run_on_gui(m_gui_timeout_ms, []() -> AppState {
        AppState s;
        MainFrame* mf = wxGetApp().mainframe;
        Plater*    p  = wxGetApp().plater();
        if (mf) {
            // active_tab: map the current top tab to a stable name (see MainFrame
            // tab enum tp3DEditor/tpPreview/tpMonitor). Use a best-effort string.
            s.active_tab = std::string(mf->get_title().ToUTF8()); // refine during integration
            s.foreground = mf->IsActive();
        }
        if (p) {
            s.project_loaded = !p->model().objects.empty();
            s.slicing        = p->is_background_process_running(); // refine accessor name
        }
        // modal_dialog: a top-level modal window other than the main frame.
        if (wxWindow* top = wxGetActiveWindow())
            if (auto* tlw = dynamic_cast<wxTopLevelWindow*>(top))
                if (tlw != static_cast<wxWindow*>(mf) && tlw->IsModal())
                    s.modal_dialog = std::string(tlw->GetTitle().ToUTF8());
        return s;
    });
}

// click/type/keys/screenshots implemented in Task 16.

}}} // namespace
```

> NOTE — integration TODOs to confirm while building (these are GUI glue, not logic):
> - `kErrGuiBusy_placeholder` → use the real constant `kErrGuiBusy` (1004) from `JsonRpcDispatcher.hpp`; include that header and replace the placeholder. (Kept distinct here only to flag the include.)
> - `plater->canvas3D_widget()` / `get_current_canvas3D()` — confirm the exact accessor names on `Plater` (they exist; pick the correct one during build). The 3D canvas is a `GLCanvas3D` wrapper; you need its `wxWindow*` for `ClientToScreen`.
> - `p->is_background_process_running()` and `model()` — confirm the exact `Plater` method names; substitute the correct slicing-status accessor.
> - `mf->get_title()` for `active_tab` is a placeholder — prefer mapping the selected main-tab index to `"prepare"/"preview"/"device"` if a getter exists.
> These do not affect the pure CI tests; they are exercised by the Task 22 e2e.

- [ ] **Step 3: Add to the GUI build**

In `src/slic3r/CMakeLists.txt`, add:
```cmake
    GUI/Automation/WxUiBackend.cpp
    GUI/Automation/WxUiBackend.hpp
```

- [ ] **Step 4: Build to verify (resolve the TODO accessor names now)**

Run: `cmake --build . --config RelWithDebInfo --target OrcaSlicer -- -m`
Expected: clean build after replacing the placeholder constant and confirming the `Plater` accessor names.

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/Automation/WxUiBackend.hpp \
        src/slic3r/GUI/Automation/WxUiBackend.cpp src/slic3r/CMakeLists.txt
git commit -m "feat(automation): WxUiBackend marshaller + dump_tree + app_state"
```

---

### Task 16: WxUiBackend — actions + screenshots

**Files:**
- Modify: `src/slic3r/GUI/Automation/WxUiBackend.cpp`

- [ ] **Step 1: Add includes for input + image conversion**

At the top of `WxUiBackend.cpp`, add:
```cpp
#include <wx/uiaction.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/mstream.h>
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
```

- [ ] **Step 2: Implement click / type_text / send_keys**

Append to `WxUiBackend.cpp` (inside the namespace, before the closing braces). Add a key-mapping helper in the anonymous namespace:

```cpp
namespace {
long wx_keycode(const std::string& key) {
    if (key.size() == 1) return (long)std::toupper((unsigned char)key[0]);
    if (key == "enter" || key == "return") return WXK_RETURN;
    if (key == "tab")    return WXK_TAB;
    if (key == "esc" || key == "escape") return WXK_ESCAPE;
    if (key == "space")  return WXK_SPACE;
    if (key == "delete") return WXK_DELETE;
    if (key == "backspace") return WXK_BACK;
    if (key.size() >= 2 && (key[0]=='f' || key[0]=='F')) {
        int n = std::atoi(key.c_str() + 1);
        if (n >= 1 && n <= 12) return WXK_F1 + (n - 1);
    }
    return 0;
}

void apply_modifiers_down(wxUIActionSimulator& sim,
                          const std::vector<KeyModifier>& mods, bool down) {
    for (KeyModifier m : mods) {
        long code = (m == KeyModifier::Ctrl)  ? WXK_CONTROL :
                    (m == KeyModifier::Shift) ? WXK_SHIFT :
                    (m == KeyModifier::Alt)   ? WXK_ALT : WXK_CONTROL; // Cmd~Ctrl
        if (down) sim.KeyDown(code); else sim.KeyUp(code);
    }
}
} // namespace

bool WxUiBackend::click(const UiNode& node, MouseButton button, bool dbl,
                        const std::vector<KeyModifier>& modifiers) {
    return run_on_gui(m_gui_timeout_ms, [&]() -> bool {
        // Raise/focus the owning top-level window so OS input lands on it.
        if (auto* w = reinterpret_cast<wxWindow*>(node.handle)) {
            if (wxWindow* tlw = wxGetTopLevelParent(w)) tlw->Raise();
            w->SetFocus();
        }
        const int cx = node.rect.x + node.rect.w / 2;
        const int cy = node.rect.y + node.rect.h / 2;
        wxUIActionSimulator sim;
        sim.MouseMove(cx, cy);
        apply_modifiers_down(sim, modifiers, true);
        const wxMouseButton b = (button == MouseButton::Right)  ? wxMOUSE_BTN_RIGHT :
                                (button == MouseButton::Middle) ? wxMOUSE_BTN_MIDDLE :
                                                                  wxMOUSE_BTN_LEFT;
        if (dbl) sim.MouseDblClick(b); else sim.MouseClick(b);
        apply_modifiers_down(sim, modifiers, false);
        return true;
    });
}

bool WxUiBackend::type_text(const std::string& text) {
    return run_on_gui(m_gui_timeout_ms, [&]() -> bool {
        wxUIActionSimulator sim;
        sim.Text(wxString::FromUTF8(text.c_str()));
        return true;
    });
}

bool WxUiBackend::send_keys(const std::vector<KeyChord>& chords) {
    return run_on_gui(m_gui_timeout_ms, [&]() -> bool {
        wxUIActionSimulator sim;
        for (const KeyChord& c : chords) {
            const long code = wx_keycode(c.key);
            if (code == 0) continue;
            apply_modifiers_down(sim, c.modifiers, true);
            sim.Char(code);
            apply_modifiers_down(sim, c.modifiers, false);
        }
        return true;
    });
}
```

- [ ] **Step 3: Implement screenshots**

Add a shared `wxImage → PNG bytes` helper in the anonymous namespace:

```cpp
namespace {
PngImage wximage_to_png(const wxImage& image) {
    wxMemoryOutputStream mem;
    if (!image.SaveFile(mem, wxBITMAP_TYPE_PNG))
        throw AutomationError(kErrScreenshotFail, "PNG encode failed");
    PngImage out;
    out.width  = image.GetWidth();
    out.height = image.GetHeight();
    const size_t n = mem.GetSize();
    out.png.resize(n);
    mem.CopyTo(out.png.data(), n);
    return out;
}

// RGBA ThumbnailData -> wxImage (mirrors GLCanvas3D::debug_output_thumbnail,
// GLCanvas3D.cpp:6099 — note the vertical flip).
wxImage thumbnail_to_wximage(const ThumbnailData& td) {
    wxImage image((int)td.width, (int)td.height);
    image.InitAlpha();
    for (unsigned int r = 0; r < td.height; ++r) {
        unsigned int rr = (td.height - 1 - r) * td.width;
        for (unsigned int c = 0; c < td.width; ++c) {
            const unsigned char* px = td.pixels.data() + 4 * (rr + c);
            image.SetRGB((int)c, (int)r, px[0], px[1], px[2]);
            image.SetAlpha((int)c, (int)r, px[3]);
        }
    }
    return image;
}
} // namespace

PngImage WxUiBackend::screenshot_window(const UiNode* target) {
    return run_on_gui(m_gui_timeout_ms, [&]() -> PngImage {
        wxWindow* win = target ? reinterpret_cast<wxWindow*>(target->handle)
                               : static_cast<wxWindow*>(wxGetApp().mainframe);
        if (win == nullptr)
            throw AutomationError(kErrScreenshotFail, "no window to capture");
        const wxSize sz = win->GetClientSize();
        if (sz.x <= 0 || sz.y <= 0)
            throw AutomationError(kErrScreenshotFail, "window has no client area");
        wxBitmap bmp(sz.x, sz.y);
        wxClientDC dc(win);
        wxMemoryDC mdc(bmp);
        mdc.Blit(0, 0, sz.x, sz.y, &dc, 0, 0);
        mdc.SelectObject(wxNullBitmap);
        return wximage_to_png(bmp.ConvertToImage());
    });
}

PngImage WxUiBackend::screenshot_viewport3d(std::optional<int> plate,
                                            std::optional<int> width,
                                            std::optional<int> height) {
    return run_on_gui(m_gui_timeout_ms, [&]() -> PngImage {
        Plater* p = wxGetApp().plater();
        if (p == nullptr)
            throw AutomationError(kErrScreenshotFail, "no plater");
        const unsigned int w = width  ? (unsigned)*width  : 800;
        const unsigned int h = height ? (unsigned)*height : 600;
        ThumbnailData data;
        // Use Plater's thumbnail wrapper which calls GLCanvas3D::render_thumbnail
        // with the GL context current (Plater.cpp:10605). Confirm exact signature
        // during build; pass the requested size and default camera/params.
        p->generate_thumbnail(data, w, h, /*params*/ {}, Camera::EType::Perspective);
        (void)plate; // v1: active plate only; `plate` reserved for future use
        if (!data.is_valid())
            throw AutomationError(kErrScreenshotFail, "thumbnail render failed");
        return wximage_to_png(thumbnail_to_wximage(data));
    });
}
```

> NOTE — confirm during build:
> - `Plater::generate_thumbnail(...)` public signature/availability (the wrapper exists per `Plater.cpp:10605`; the public entry may be `Plater::generate_thumbnail` — wire to whatever is public, or call `get_current_canvas3D()->render_thumbnail(...)` directly with a constructed `ThumbnailsParams`).
> - `Camera::EType` enum value name.
> - `wxUIActionSimulator::Text` exists in the wx build; if not, fall back to per-character `sim.Char(...)`.

- [ ] **Step 4: Build to verify**

Run: `cmake --build . --config RelWithDebInfo --target OrcaSlicer -- -m`
Expected: clean build after confirming the noted accessor/enum names.

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/Automation/WxUiBackend.cpp
git commit -m "feat(automation): WxUiBackend input + window/viewport screenshots"
```

---

### Task 17: GUI_App lifecycle + `is_automation_enabled()`

**Files:**
- Modify: `src/slic3r/GUI/GUI_App.hpp`
- Modify: `src/slic3r/GUI/GUI_App.cpp`

- [ ] **Step 1: Add members + accessor to GUI_App.hpp**

In `src/slic3r/GUI/GUI_App.hpp`, near the existing `HttpServer m_http_server;` member (line 334) add forward-declared members:

```cpp
    // --- UI automation (opt-in; off unless --automation-server) ---
    std::unique_ptr<Slic3r::GUI::Automation::AutomationServer>   m_automation_server;
    std::unique_ptr<Slic3r::GUI::Automation::WxUiBackend>        m_automation_backend;
    std::unique_ptr<Slic3r::GUI::Automation::JsonRpcDispatcher>  m_automation_dispatcher;
    int                                                          m_automation_port{0};
```

Add forward declarations near the top of the file (with the other forward decls):

```cpp
namespace Slic3r { namespace GUI { namespace Automation {
    class AutomationServer; class WxUiBackend; class JsonRpcDispatcher;
}}}
```

Add the public accessor + lifecycle methods (near `is_editor()` etc.):

```cpp
    bool is_automation_enabled() const { return m_automation_port > 0; }
    void start_automation_server();
    void stop_automation_server();
```

- [ ] **Step 2: Implement lifecycle in GUI_App.cpp**

Add includes at the top of `src/slic3r/GUI/GUI_App.cpp`:

```cpp
#include "slic3r/GUI/Automation/AutomationServer.hpp"
#include "slic3r/GUI/Automation/WxUiBackend.hpp"
#include "slic3r/GUI/Automation/JsonRpcDispatcher.hpp"
```

Implement the methods (anywhere in the file, e.g. next to `start_http_server`, ~line 7048):

```cpp
void GUI_App::start_automation_server() {
    if (m_automation_port <= 0) return;            // disabled
    if (m_automation_server)    return;            // already running
    using namespace Slic3r::GUI::Automation;
    m_automation_backend.reset(new WxUiBackend());
    m_automation_dispatcher.reset(new JsonRpcDispatcher(*m_automation_backend));
    m_automation_server.reset(new AutomationServer((unsigned short)m_automation_port));
    JsonRpcDispatcher* disp = m_automation_dispatcher.get();
    m_automation_server->set_handler(
        [disp](const std::string& body) { return disp->handle_request(body); });
    m_automation_server->set_health_text(
        std::string("OrcaSlicer automation server v") + kAutomationVersion);
    m_automation_server->start();
    BOOST_LOG_TRIVIAL(warning)
        << "UI automation server ENABLED on 127.0.0.1:" << m_automation_port
        << " (input injection is active)";
}

void GUI_App::stop_automation_server() {
    if (m_automation_server) m_automation_server->stop();
    m_automation_server.reset();
    m_automation_dispatcher.reset();
    m_automation_backend.reset();
}
```

- [ ] **Step 3: Set the port from init params + start in `post_init()`**

In `GUI_App::post_init()` (starts ~line 727), near the top after the `assert(initialized())` block, add:

```cpp
    if (init_params != nullptr && init_params->automation_port > 0) {
        m_automation_port = init_params->automation_port;
        start_automation_server();
    }
```

- [ ] **Step 4: Stop in `OnExit()`**

In `GUI_App::OnExit()` (line 2464), right after `stop_http_server();` (line 2466), add:

```cpp
    stop_automation_server();
```

- [ ] **Step 5: Build to verify**

Run: `cmake --build . --config RelWithDebInfo --target OrcaSlicer -- -m`
Expected: clean build.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/GUI_App.hpp src/slic3r/GUI/GUI_App.cpp
git commit -m "feat(automation): GUI_App owns automation server lifecycle (opt-in)"
```

---

### Task 18: CLI flag plumbing

**Files:**
- Modify: `src/libslic3r/PrintConfig.cpp` (register options in `CLIMiscConfigDef`)
- Modify: `src/slic3r/GUI/GUI_Init.hpp` (add `automation_port` field)
- Modify: `src/OrcaSlicer.cpp` (read options, populate params)

- [ ] **Step 1: Register the CLI options**

In `src/libslic3r/PrintConfig.cpp`, inside `CLIMiscConfigDef::CLIMiscConfigDef()` (the constructor near line 10675, following the existing `def = this->add(...)` pattern), add:

```cpp
    def = this->add("automation_server", coBool);
    def->label = L("Enable UI automation server");
    def->tooltip = L("Start a localhost JSON-RPC server that lets external scripts "
                     "drive and observe the GUI. For testing/automation only.");
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("automation_server_port", coInt);
    def->label = L("UI automation server port");
    def->tooltip = L("TCP port for the UI automation server (bound to 127.0.0.1).");
    def->min = 1;
    def->cli_params = "port";
    def->set_default_value(new ConfigOptionInt(13619));
```

- [ ] **Step 2: Add the field to GUI_InitParams**

In `src/slic3r/GUI/GUI_Init.hpp` (struct at lines 16-35), add after `bool input_gcode { false };`:

```cpp
    // UI automation: 0 = disabled, else the TCP port for the localhost JSON-RPC server.
    int                         automation_port { 0 };
```

- [ ] **Step 3: Populate it in OrcaSlicer.cpp**

In `src/OrcaSlicer.cpp`, in the block that fills `GUI_InitParams params;` (lines ~1315-1343), before `return Slic3r::GUI::GUI_Run(params);`, add:

```cpp
        // UI automation server (opt-in). --automation-server enables it;
        // --automation-server-port overrides the default 13619.
        if (m_config.has("automation_server") && m_config.opt_bool("automation_server")) {
            int port = m_config.has("automation_server_port")
                           ? m_config.opt_int("automation_server_port") : 13619;
            params.automation_port = port > 0 ? port : 13619;
            BOOST_LOG_TRIVIAL(warning)
                << "UI automation server requested on port " << params.automation_port;
        }
```

- [ ] **Step 4: Build to verify the flag plumbs through**

Run: `cmake --build . --config RelWithDebInfo --target OrcaSlicer -- -m`
Expected: clean build. The CLI now accepts `--automation-server` and `--automation-server-port=PORT`.

- [ ] **Step 5: Manual smoke test (server reachable)**

Launch:
```
OrcaSlicer --automation-server --automation-server-port=13619
```
From another shell:
```
curl -s http://127.0.0.1:13619/
curl -s -X POST http://127.0.0.1:13619/jsonrpc \
  -H "Content-Type: application/json" \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"automation.version\"}"
```
Expected: health text from `GET /`; and a JSON-RPC result with `version`/`protocol`/`capabilities` from the POST. Confirm that launching WITHOUT the flag leaves nothing listening on 13619.

- [ ] **Step 6: Commit**

```bash
git add src/libslic3r/PrintConfig.cpp src/slic3r/GUI/GUI_Init.hpp src/OrcaSlicer.cpp
git commit -m "feat(automation): --automation-server CLI flag plumbed into GUI"
```

---

### Task 19: Widget instrumentation (~15-20 stable IDs)

**Files:**
- Modify: widget-construction sites listed below (each adds one `set_automation_id(...)` call + the registry include).

> Goal: give an external script stable, named targets for the most-used controls (spec §7). Each site adds `#include "slic3r/GUI/Automation/AutomationRegistry.hpp"` (once per file) and a `Slic3r::GUI::Automation::set_automation_id(widget, "id");` after the widget is created. This is a safe no-op when automation is off (the registry just stores a pointer). Use the agreed IDs below.

- [ ] **Step 1: Locate the construction sites**

For each control, find where it is constructed (use Grep for the button/combo labels or member names). Suggested ID set (document these in `doc/automation.md`, Task 23):

| Widget | Automation ID | Likely file |
|---|---|---|
| Slice-plate button | `btn_slice` | `src/slic3r/GUI/MainFrame.cpp` / `Plater.cpp` |
| Export-G-code button | `btn_export` | `Plater.cpp` |
| Printer preset combo | `combo_printer` | `Plater.cpp` (sidebar) |
| Filament preset combo | `combo_filament` | `Plater.cpp` (sidebar) |
| Process/print preset combo | `combo_process` | `Plater.cpp` (sidebar) |
| Prepare/3D-editor tab | `tab_prepare` | `MainFrame.cpp` |
| Preview tab | `tab_preview` | `MainFrame.cpp` |
| Device/Monitor tab | `tab_device` | `MainFrame.cpp` |
| Add/Import object button | `btn_add` | `Plater.cpp` |
| 3D canvas | `canvas_3d` | `Plater.cpp` (GLCanvas3D widget) |

- [ ] **Step 2: Add the calls**

Example (Slice button — adapt to the actual variable name found):

```cpp
#include "slic3r/GUI/Automation/AutomationRegistry.hpp"
// ... after the button is created, e.g.:
// m_slice_btn = new Button(parent, _L("Slice plate"));
Slic3r::GUI::Automation::set_automation_id(m_slice_btn, "btn_slice");
```

Repeat for each row above. For the 3D canvas, register the `wxGLCanvas`-derived widget returned by the plater's canvas accessor:

```cpp
Slic3r::GUI::Automation::set_automation_id(view3D_canvas_widget, "canvas_3d");
```

- [ ] **Step 3: Add common dialog OK/Cancel IDs (if a shared dialog base exists)**

If OrcaSlicer has a common dialog base/factory for OK/Cancel/Yes/No, register them there once (`dlg_ok`, `dlg_cancel`, `dlg_yes`, `dlg_no`). Otherwise, instrument the two or three most-used dialogs. Document whichever you choose.

- [ ] **Step 4: Build to verify**

Run: `cmake --build . --config RelWithDebInfo --target OrcaSlicer -- -m`
Expected: clean build.

- [ ] **Step 5: Verify IDs resolve at runtime**

Launch with `--automation-server`, then:
```
curl -s -X POST http://127.0.0.1:13619/jsonrpc -H "Content-Type: application/json" \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"widget.get\",\"params\":{\"target\":{\"id\":\"btn_slice\"}}}"
```
Expected: a node with `"id":"btn_slice"` and a sensible screen `rect`.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(automation): instrument core widgets with stable automation ids"
```

---

## PHASE 4 — Client, docs, regression

### Task 20: Python reference client

**Files:**
- Create: `tools/automation/orca_automation.py`

- [ ] **Step 1: Write the client**

`tools/automation/orca_automation.py`:

```python
"""Reference client for the OrcaSlicer UI automation JSON-RPC server.

Usage:
    from orca_automation import OrcaClient
    orca = OrcaClient(port=13619)
    print(orca.version())
    orca.click({"id": "btn_slice"})
    orca.wait_for({"id": "btn_export"}, state="enabled", timeout_ms=120000)
    png = orca.screenshot_3d(width=1024, height=768)
    open("preview.png", "wb").write(png)
"""
from __future__ import annotations
import base64
import json
import urllib.request
from typing import Any, Optional


class OrcaError(RuntimeError):
    def __init__(self, code: int, message: str):
        super().__init__(f"[{code}] {message}")
        self.code = code
        self.message = message


class OrcaClient:
    def __init__(self, host: str = "127.0.0.1", port: int = 13619, timeout: float = 30.0):
        self._url = f"http://{host}:{port}/jsonrpc"
        self._timeout = timeout
        self._id = 0

    def _call(self, method: str, params: Optional[dict] = None) -> Any:
        self._id += 1
        payload = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            payload["params"] = params
        data = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(
            self._url, data=data, headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=self._timeout) as resp:
            body = json.loads(resp.read().decode("utf-8"))
        if "error" in body:
            err = body["error"]
            raise OrcaError(err.get("code", -1), err.get("message", "unknown error"))
        return body.get("result")

    # --- protocol methods ---
    def version(self) -> dict:
        return self._call("automation.version")

    def dump_tree(self, root: Optional[str] = None, max_depth: Optional[int] = None,
                  visible_only: bool = False, include_imgui: bool = True) -> dict:
        params: dict = {"visible_only": visible_only, "include_imgui": include_imgui}
        if root is not None:
            params["root"] = root
        if max_depth is not None:
            params["max_depth"] = max_depth
        return self._call("tree.dump", params)

    def find(self, **predicate) -> list:
        # predicate keys: name, class, label, value, backend
        return self._call("tree.find", predicate)

    def get(self, target: dict) -> dict:
        return self._call("widget.get", {"target": target})

    def click(self, target: dict, button: str = "left",
              double: bool = False, modifiers: Optional[list] = None) -> dict:
        params = {"target": target, "button": button, "double": double}
        if modifiers:
            params["modifiers"] = modifiers
        return self._call("input.click", params)

    def type(self, text: str, target: Optional[dict] = None) -> dict:
        params: dict = {"text": text}
        if target is not None:
            params["target"] = target
        return self._call("input.type", params)

    def key(self, keys) -> dict:
        # keys: "ctrl+s" or ["ctrl", "s"]
        return self._call("input.key", {"keys": keys})

    def wait_for(self, target: dict, state: str = "visible",
                 value: Optional[str] = None, timeout_ms: int = 5000,
                 poll_ms: int = 100) -> dict:
        params = {"target": target, "state": state,
                  "timeout_ms": timeout_ms, "poll_ms": poll_ms}
        if value is not None:
            params["value"] = value
        return self._call("sync.wait_for", params)

    def app_state(self) -> dict:
        return self._call("app.state")

    def screenshot(self, target: Optional[dict] = None) -> bytes:
        params = {"target": target} if target is not None else None
        result = self._call("screenshot.window", params)
        return base64.b64decode(result["png_base64"])

    def screenshot_3d(self, plate: Optional[int] = None,
                      width: Optional[int] = None, height: Optional[int] = None) -> bytes:
        params: dict = {}
        if plate is not None:
            params["plate"] = plate
        if width is not None:
            params["width"] = width
        if height is not None:
            params["height"] = height
        result = self._call("screenshot.viewport3d", params or None)
        return base64.b64decode(result["png_base64"])
```

- [ ] **Step 2: Commit**

```bash
git add tools/automation/orca_automation.py
git commit -m "feat(automation): Python reference client"
```

---

### Task 21: End-to-end example / smoke test

**Files:**
- Create: `tools/automation/example_slice.py`

- [ ] **Step 1: Write the example**

`tools/automation/example_slice.py`:

```python
"""End-to-end smoke test: launch OrcaSlicer with the automation server, load a
model, slice it, wait for completion, and save both a window PNG and a 3D PNG.

Run:
    python example_slice.py --orca /path/to/OrcaSlicer --model /path/to/cube.stl

On Linux CI, wrap with a virtual display, e.g.:
    xvfb-run -a python example_slice.py --orca ./OrcaSlicer --model cube.stl
"""
from __future__ import annotations
import argparse
import subprocess
import sys
import time

from orca_automation import OrcaClient, OrcaError


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--orca", required=True, help="path to the OrcaSlicer executable")
    ap.add_argument("--model", required=True, help="path to an STL/3MF to load")
    ap.add_argument("--port", type=int, default=13619)
    args = ap.parse_args()

    proc = subprocess.Popen([
        args.orca,
        "--automation-server",
        f"--automation-server-port={args.port}",
        args.model,
    ])
    try:
        orca = OrcaClient(port=args.port)

        # Wait for the server to come up.
        for _ in range(60):
            try:
                print("connected:", orca.version())
                break
            except OSError:
                time.sleep(0.5)
        else:
            print("ERROR: automation server did not start", file=sys.stderr)
            return 1

        # Wait until the project (model) is loaded.
        deadline = time.time() + 30
        while time.time() < deadline:
            if orca.app_state().get("project_loaded"):
                break
            time.sleep(0.5)

        # Click Slice and wait for the Export button to become enabled
        # (slicing complete) — wait_for replaces fragile fixed sleeps.
        orca.click({"id": "btn_slice"})
        orca.wait_for({"id": "btn_export"}, state="enabled", timeout_ms=180000,
                      poll_ms=500)

        with open("window.png", "wb") as f:
            f.write(orca.screenshot())
        with open("preview_3d.png", "wb") as f:
            f.write(orca.screenshot_3d(width=1024, height=768))
        print("wrote window.png and preview_3d.png")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 2: Run the e2e (manual; needs a display)**

Run:
```
cd tools/automation
python example_slice.py --orca <built OrcaSlicer path> --model <some .stl>
```
Expected: connects, reports `version`, loads the model, slices, `wait_for` returns when Export enables, and `window.png` + `preview_3d.png` are written and visually correct.

- [ ] **Step 3: Commit**

```bash
git add tools/automation/example_slice.py
git commit -m "feat(automation): runnable e2e slice example / smoke test"
```

---

### Task 22: Protocol documentation

**Files:**
- Create: `doc/automation.md`

- [ ] **Step 1: Write the docs**

`doc/automation.md` must contain (write complete prose + tables — no stubs):
- **Overview & activation**: `--automation-server [--automation-server-port=PORT]`, default port 13619, localhost-only, off by default, security note (no token in v1; the localhost bind is the boundary).
- **Transport**: HTTP/1.1, `POST /jsonrpc` (JSON-RPC 2.0 body), `GET /` health page.
- **Methods table**: reproduce the spec §5 method table (params + results) for all ten methods + `automation.version`.
- **Node shape**: the unified JSON node (the spec §5 block: `backend,id,path,class,label,rect,enabled,visible,value,children`).
- **Error codes**: standard (`-32700/-32600/-32601/-32602`) + application (`1001`–`1006`) with meanings.
- **Automation-id naming conventions**: the table from Task 19 (`btn_slice`, `combo_printer`, `tab_preview`, `canvas_3d`, …) + guidance (`btn_`/`combo_`/`tab_`/`dlg_`/`canvas_` prefixes).
- **ImGui notes**: items addressable only while drawn; `refresh_ui` is forced before reads/actions; use `sync.wait_for` to wait for a gizmo panel item to appear; raw-`ImGui::` gizmos (Emboss/SVG/Text) are window-level only in v1.
- **Platform / display caveats**: OS input injection needs a focused, visible window; Linux CI needs a display (Xvfb); input is async — rely on `sync.wait_for`, not fixed sleeps; single-client/serialized in v1.
- **Quick start**: a 10-line `orca_automation.py` snippet (connect → version → click → wait_for → screenshot_3d).
- **Future work**: auth token + Preferences toggle, WebSocket push events, per-item ImGui gizmo instrumentation, MCP wrapper.

- [ ] **Step 2: Commit**

```bash
git add doc/automation.md
git commit -m "docs(automation): protocol reference, ids, and platform caveats"
```

---

### Task 23: Regression verification (automation OFF)

**Files:** none (verification task).

- [ ] **Step 1: Run the full unit-test suite**

Run:
```
cd build && ctest --output-on-failure
```
Expected: all suites pass, including `automation_tests`.

- [ ] **Step 2: Build a normal (no-flag) run and confirm zero footprint**

Launch OrcaSlicer **without** `--automation-server`. Confirm:
- No listener on 13619: `curl -s http://127.0.0.1:13619/` fails to connect.
- No `orca_automation` thread is created (verify in a debugger / process explorer, or add a one-off log line during testing then remove it).
- `wxGetApp().is_automation_enabled()` returns false (the ImGui hooks short-circuit).

- [ ] **Step 3: Confirm ImGui hot path is unchanged when disabled**

Inspect the ImGui hook sites: each begins with `if (!wxGetApp().is_automation_enabled()) return;` (or is wrapped in that guard). Confirm there is no allocation or `ImGuiItemTable` access on the disabled path. Build in RelWithDebInfo and do a quick interactive sanity pass (open a gizmo, move sliders) to confirm no visual/behavior change.

- [ ] **Step 4: Cross-platform build check**

Ensure the new code compiles on all three platforms (per the project's cross-platform constraint). At minimum, confirm the Windows build is clean; note in the PR that macOS/Linux builds must be validated by CI/maintainers. Watch for: `wxUIActionSimulator` availability, `wxImage` PNG handler registration (`wxInitAllImageHandlers`/PNG handler must be present — it is, since OrcaSlicer already loads PNGs), and beast/asio includes.

- [ ] **Step 5: Final commit (if any doc/notes added)**

```bash
git add -A
git commit -m "test(automation): regression checks for disabled-path no-op"
```

---

## Self-Review (performed against the spec)

**Spec coverage check (spec §§1-16):**
- §4 components → Tasks 1,3,6,11,12,13,15,16 (all new files) + §4 touch points → Tasks 14,17,18,19,20(CMake within tasks). ✔
- §5 transport/protocol/methods/node-shape/errors → Tasks 6-11 (dispatcher) + Task 11 (server) + Task 22 (docs). All ten methods + version covered. ✔
- §6 threading (CallAfter + future + 5s timeout → 1004) → Task 15 `run_on_gui`. ✔
- §7 locator & IDs → Tasks 3,4 (resolution) + Task 12 (registry) + Task 19 (instrumentation). ✔
- §8 ImGui coverage (recording, window enum, double-buffer swap, freshness, actions) → Tasks 13,14,15 (`append_imgui_nodes`, `refresh_ui`). ✔
- §9 screenshots (window DC + render_thumbnail) → Task 16. ✔
- §10 activation/security (off by default, localhost, zero overhead disabled) → Tasks 17,18,23. ✔
- §11 testability (pure dispatcher/serializer/locator, MockUiBackend, CI no-display) → Tasks 1-10. ✔
- §12 deliverables (C++ components, python client, example, doc, tests) → Tasks 1-22. ✔
- §13 file inventory → matches File Structure section (plus added `Locator.{hpp,cpp}` — justified in the Architecture note). ✔
- §16 verification plan (CI units, e2e, regression) → Tasks 1-10 (CI), 21 (e2e), 23 (regression). ✔

**Deliberate deviations from the spec (documented inline):**
1. **`IUiBackend` shape** refined for testability (orchestration in the pure dispatcher; backend exposes snapshot + rect-based primitives). External JSON-RPC protocol is unchanged. (Architecture note.)
2. **Added `Locator.{hpp,cpp}`** (not in spec inventory) to make resolution pure & unit-testable — satisfies spec §11. (Architecture note.)
3. **CLI flag is two options** (`--automation-server` + `--automation-server-port`) instead of `--automation-server[=PORT]`, to fit OrcaSlicer's `DynamicConfig` CLI. Same capability. (File Structure → CLI flag mapping.)

**Placeholder scan:** The only `TODO`-style notes are explicitly-flagged GUI-glue accessor names in Tasks 15/16 (`Plater` slicing-status/canvas accessors, `Camera::EType` value, `Plater::generate_thumbnail` signature). These are integration lookups to confirm at build time on real wx headers, not logic gaps; the pure CI-tested core has complete code. Each is called out with the verified anchor (`Plater.cpp:10605`, `GLCanvas3D.cpp:6099`) so the engineer can resolve them immediately. **Action for the executor:** resolve each flagged accessor name during the relevant task's build step before committing.

**Type consistency:** `UiNode`, `Target`, `WaitState`, `DumpOptions`, `KeyChord`, `AppState`, `PngImage`, `AutomationError`, error-code constants, and method handler names are defined once (Task 1 / Task 3 / Task 6) and used consistently across Tasks 7-17. `node_to_json`/`app_state_to_json`/`find_matches`/`resolve_unique`/`evaluate_state` signatures match between header, tests, and call sites.
