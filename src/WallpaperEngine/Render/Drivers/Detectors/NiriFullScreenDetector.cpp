#include "NiriFullScreenDetector.h"

#ifdef ENABLE_WAYLAND

#include "NiriIPC.h"
#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <cstdint>
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
// open/resize animation transients.
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

bool computeCovered (
    const std::unordered_map<std::string, OutputInfo>& outputs, const std::vector<WorkspaceInfo>& workspaces,
    const nlohmann::json& windows
) {
    if (outputs.empty ()) {
	return false;
    }

    for (const auto& [name, out] : outputs) {
	if (out.width <= 0.0 || out.height <= 0.0) {
	    return false;
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
	    return false;
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
	if (ratio < COVERAGE_THRESHOLD) {
	    return false;
	}
    }
    return true;
}

} // namespace

NiriFullScreenDetector::NiriFullScreenDetector (Application::ApplicationContext& appContext) :
    FullScreenDetector (appContext) {
    m_lastChange = std::chrono::steady_clock::now ();
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
	sLog.debug ("NiriFullScreenDetector: snapshot failed; treating as not covered");
	std::lock_guard lock (m_mutex);
	if (m_rawCovered) {
	    m_rawCovered = false;
	    m_lastChange = std::chrono::steady_clock::now ();
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

    const bool covered = computeCovered (outputs, workspaces, *windowsJ);

    std::lock_guard lock (m_mutex);
    if (covered != m_rawCovered) {
	m_rawCovered = covered;
	m_lastChange = std::chrono::steady_clock::now ();
	sLog.debug ("NiriFullScreenDetector: raw covered = ", covered);
    }
}

bool NiriFullScreenDetector::anythingFullscreen () const {
    std::lock_guard lock (m_mutex);
    const auto elapsed = std::chrono::steady_clock::now () - m_lastChange;
    if (elapsed >= DEBOUNCE && m_debouncedCovered != m_rawCovered) {
	m_debouncedCovered = m_rawCovered;
    }
    return m_debouncedCovered;
}

void NiriFullScreenDetector::reset () { recompute (); }

} // namespace WallpaperEngine::Render::Drivers::Detectors

#endif /* ENABLE_WAYLAND */
