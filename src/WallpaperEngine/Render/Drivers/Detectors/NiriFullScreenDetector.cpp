#include "NiriFullScreenDetector.h"

#ifdef ENABLE_WAYLAND

#include "NiriIPC.h"
#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

namespace WallpaperEngine::Render::Drivers::Detectors {

namespace {

// Coverage threshold: aggregate window area / output logical area required to
// consider the output "covered". Tuned below 1.0 to absorb gaps, borders, and
// optional bars (which run as layer-shell clients and don't appear in Niri's
// Windows list).
constexpr double COVERAGE_THRESHOLD = 0.90;

// Minimum stable hold time before a state transition is reported, to absorb
// open/resize animation transients. Applied per output independently.
constexpr auto DEBOUNCE = std::chrono::milliseconds (250);

struct OutputInfo {
    double width = 0.0;
    double height = 0.0;
};

struct WorkspaceInfo {
    int64_t id = 0;
    std::string output;
    bool isActive = false;
};

std::unordered_map<std::string, bool> computeCoveredPerOutput (
    const std::unordered_map<std::string, OutputInfo>& outputs, const std::vector<WorkspaceInfo>& workspaces,
    const nlohmann::json& windows
) {
    std::unordered_map<std::string, bool> result;
    result.reserve (outputs.size ());

    for (const auto& [name, out] : outputs) {
	if (out.width <= 0.0 || out.height <= 0.0) {
	    result.emplace (name, false);
	    continue;
	}
	const double area = out.width * out.height;

	const WorkspaceInfo* activeWs = nullptr;
	for (const auto& ws : workspaces) {
	    if (ws.output == name && ws.isActive) {
		activeWs = &ws;
		break;
	    }
	}
	if (activeWs == nullptr) {
	    result.emplace (name, false);
	    continue;
	}

	double sum = 0.0;
	if (windows.is_array ()) {
	    for (const auto& w : windows) {
		const int64_t wsId = w.value ("workspace_id", static_cast<int64_t> (-1));
		if (wsId != activeWs->id) {
		    continue;
		}
		if (!w.contains ("layout")) {
		    continue;
		}
		const auto& layout = w["layout"];
		if (!layout.is_object () || !layout.contains ("tile_size")) {
		    continue;
		}
		const auto& ts = layout["tile_size"];
		if (!ts.is_array () || ts.size () != 2) {
		    continue;
		}
		double tw = 0.0;
		double th = 0.0;
		try {
		    tw = ts[0].get<double> ();
		    th = ts[1].get<double> ();
		} catch (const nlohmann::json::exception&) {
		    continue;
		}
		if (tw <= 0.0 || th <= 0.0) {
		    continue;
		}
		// Cap each tile contribution at one screen so an oversized
		// tile cannot single-handedly push us past the threshold.
		const double tileArea = std::min (tw * th, area);
		sum += tileArea;
	    }
	}

	const double ratio = sum / area;
	result.emplace (name, ratio >= COVERAGE_THRESHOLD);
    }

    return result;
}

} // namespace

NiriFullScreenDetector::NiriFullScreenDetector (Application::ApplicationContext& appContext) :
    FullScreenDetector (appContext) {
    recompute ();

    m_ipc = std::make_unique<NiriIPC> ([this] (const nlohmann::json&) {
	// Any event triggers a full recompute. The handler runs on the IPC
	// worker thread; recompute opens its own short-lived snapshot
	// connections and is otherwise self-contained.
	this->recompute ();
    });

    sLog.out ("NiriFullScreenDetector: enabled (NIRI_SOCKET=", NiriIPC::socketPath ().value_or (""), ")");
}

NiriFullScreenDetector::~NiriFullScreenDetector () = default;

void NiriFullScreenDetector::recompute () {
    const auto outputsJ = NiriIPC::snapshot ("Outputs");
    const auto workspacesJ = NiriIPC::snapshot ("Workspaces");
    const auto windowsJ = NiriIPC::snapshot ("Windows");

    if (!outputsJ || !workspacesJ || !windowsJ) {
	sLog.debug ("NiriFullScreenDetector: snapshot failed; treating all outputs as not covered");
	const auto now = std::chrono::steady_clock::now ();
	std::lock_guard lock (m_mutex);
	for (auto& [name, state] : m_perOutput) {
	    if (state.raw) {
		state.raw = false;
		state.lastChange = now;
	    }
	}
	return;
    }

    std::unordered_map<std::string, OutputInfo> outputs;
    if (outputsJ->is_object ()) {
	for (auto it = outputsJ->begin (); it != outputsJ->end (); ++it) {
	    const auto& v = it.value ();
	    if (!v.is_object () || !v.contains ("logical")) {
		continue;
	    }
	    const auto& logical = v["logical"];
	    if (!logical.is_object ()) {
		continue;
	    }
	    OutputInfo info;
	    info.width = logical.value ("width", 0.0);
	    info.height = logical.value ("height", 0.0);
	    if (info.width > 0.0 && info.height > 0.0) {
		outputs.emplace (it.key (), info);
	    }
	}
    }

    std::vector<WorkspaceInfo> workspaces;
    if (workspacesJ->is_array ()) {
	for (const auto& w : *workspacesJ) {
	    if (!w.is_object ()) {
		continue;
	    }
	    WorkspaceInfo info;
	    info.id = w.value ("id", static_cast<int64_t> (0));
	    info.output = w.value ("output", std::string {});
	    info.isActive = w.value ("is_active", false);
	    workspaces.push_back (info);
	}
    }

    const auto coveredMap = computeCoveredPerOutput (outputs, workspaces, *windowsJ);

    const auto now = std::chrono::steady_clock::now ();
    std::lock_guard lock (m_mutex);

    // Update raw state and timestamps for outputs present in this snapshot.
    for (const auto& [name, covered] : coveredMap) {
	auto [it, inserted] = m_perOutput.try_emplace (name);
	if (inserted) {
	    it->second.raw = covered;
	    it->second.debounced = false;
	    it->second.lastChange = now;
	    sLog.debug (
		"NiriFullScreenDetector: registered output '", name, "' raw covered = ", covered, " (new)"
	    );
	} else if (it->second.raw != covered) {
	    it->second.raw = covered;
	    it->second.lastChange = now;
	    sLog.debug ("NiriFullScreenDetector: '", name, "' raw covered = ", covered);
	}
    }

    // Prune outputs that have disappeared from Niri's snapshot (hot-unplug).
    for (auto it = m_perOutput.begin (); it != m_perOutput.end ();) {
	if (coveredMap.find (it->first) == coveredMap.end ()) {
	    it = m_perOutput.erase (it);
	} else {
	    ++it;
	}
    }
}

void NiriFullScreenDetector::advanceDebounceLocked () const {
    const auto now = std::chrono::steady_clock::now ();
    for (auto& [name, state] : m_perOutput) {
	if (state.debounced != state.raw && (now - state.lastChange) >= DEBOUNCE) {
	    state.debounced = state.raw;
	}
    }
}

bool NiriFullScreenDetector::anythingFullscreen () const {
    std::lock_guard lock (m_mutex);
    advanceDebounceLocked ();
    return std::ranges::any_of (m_perOutput, [] (const auto& entry) { return entry.second.debounced; });
}

std::optional<std::unordered_set<std::string>> NiriFullScreenDetector::coveredOutputs () const {
    std::unordered_set<std::string> covered;
    std::lock_guard lock (m_mutex);
    advanceDebounceLocked ();
    for (const auto& [name, state] : m_perOutput) {
	if (state.debounced) {
	    covered.insert (name);
	}
    }
    return covered;
}

void NiriFullScreenDetector::reset () { recompute (); }

} // namespace WallpaperEngine::Render::Drivers::Detectors

#endif /* ENABLE_WAYLAND */
