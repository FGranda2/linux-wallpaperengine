#include "NiriIPC.h"

#ifdef ENABLE_WAYLAND

#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace WallpaperEngine::Render::Drivers::Detectors {

namespace {

int connectToNiri (const std::string& path) {
    if (path.empty ()) {
	return -1;
    }
    const int fd = ::socket (AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
	return -1;
    }
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (path.size () >= sizeof (addr.sun_path)) {
	::close (fd);
	return -1;
    }
    std::memcpy (addr.sun_path, path.data (), path.size ());
    if (::connect (fd, reinterpret_cast<sockaddr*> (&addr), sizeof (addr)) < 0) {
	::close (fd);
	return -1;
    }
    return fd;
}

bool writeAll (int fd, const std::string& data) {
    size_t total = 0;
    while (total < data.size ()) {
	const ssize_t n = ::send (fd, data.data () + total, data.size () - total, MSG_NOSIGNAL);
	if (n <= 0) {
	    if (n < 0 && errno == EINTR) {
		continue;
	    }
	    return false;
	}
	total += static_cast<size_t> (n);
    }
    return true;
}

constexpr size_t SAFETY_CAP = 16ULL * 1024ULL * 1024ULL;

} // namespace

std::optional<std::string> NiriIPC::socketPath () {
    const char* env = std::getenv ("NIRI_SOCKET");
    if (env == nullptr || *env == '\0') {
	return std::nullopt;
    }
    return std::string (env);
}

std::optional<nlohmann::json> NiriIPC::snapshot (const std::string& tag) {
    const auto path = socketPath ();
    if (!path) {
	return std::nullopt;
    }
    const int fd = connectToNiri (*path);
    if (fd < 0) {
	return std::nullopt;
    }

    const std::string req = "\"" + tag + "\"\n";
    if (!writeAll (fd, req)) {
	::close (fd);
	return std::nullopt;
    }

    std::string buf;
    char tmp[4096];
    while (true) {
	const ssize_t n = ::recv (fd, tmp, sizeof (tmp), 0);
	if (n == 0) {
	    break;
	}
	if (n < 0) {
	    if (errno == EINTR) {
		continue;
	    }
	    break;
	}
	buf.append (tmp, static_cast<size_t> (n));
	if (buf.find ('\n') != std::string::npos) {
	    break;
	}
	if (buf.size () > SAFETY_CAP) {
	    break;
	}
    }
    ::close (fd);

    if (const auto nl = buf.find ('\n'); nl != std::string::npos) {
	buf.resize (nl);
    }
    if (buf.empty ()) {
	return std::nullopt;
    }

    try {
	auto j = nlohmann::json::parse (buf);
	if (!j.is_object () || !j.contains ("Ok")) {
	    return std::nullopt;
	}
	const auto& ok = j["Ok"];
	if (!ok.is_object () || !ok.contains (tag)) {
	    return std::nullopt;
	}
	return ok[tag];
    } catch (const nlohmann::json::exception&) {
	return std::nullopt;
    }
}

NiriIPC::NiriIPC (EventHandler handler) : m_handler (std::move (handler)) {
    m_thread = std::thread ([this] { this->workerLoop (); });
}

NiriIPC::~NiriIPC () {
    m_stop.store (true);
    const int fd = m_socketFd.exchange (-1);
    if (fd >= 0) {
	::shutdown (fd, SHUT_RDWR);
	::close (fd);
    }
    if (m_thread.joinable ()) {
	m_thread.join ();
    }
}

void NiriIPC::workerLoop () {
    using namespace std::chrono_literals;
    auto backoff = 200ms;
    constexpr auto maxBackoff = 5000ms;

    std::string buf;
    char tmp[4096];

    while (!m_stop.load ()) {
	const auto path = socketPath ();
	if (!path) {
	    std::this_thread::sleep_for (maxBackoff);
	    continue;
	}
	const int fd = connectToNiri (*path);
	if (fd < 0) {
	    sLog.debug ("NiriIPC: connect failed, retrying");
	    std::this_thread::sleep_for (backoff);
	    backoff = std::min (backoff * 2, maxBackoff);
	    continue;
	}
	if (!writeAll (fd, std::string ("\"EventStream\"\n"))) {
	    ::close (fd);
	    std::this_thread::sleep_for (backoff);
	    backoff = std::min (backoff * 2, maxBackoff);
	    continue;
	}
	m_socketFd.store (fd);
	backoff = 200ms;
	buf.clear ();

	while (!m_stop.load ()) {
	    const ssize_t n = ::recv (fd, tmp, sizeof (tmp), 0);
	    if (n == 0) {
		break;
	    }
	    if (n < 0) {
		if (errno == EINTR) {
		    continue;
		}
		break;
	    }
	    buf.append (tmp, static_cast<size_t> (n));
	    size_t pos;
	    while ((pos = buf.find ('\n')) != std::string::npos) {
		std::string line = buf.substr (0, pos);
		buf.erase (0, pos + 1);
		if (line.empty ()) {
		    continue;
		}
		try {
		    auto j = nlohmann::json::parse (line);
		    if (m_handler) {
			m_handler (j);
		    }
		} catch (const nlohmann::json::exception& e) {
		    sLog.debug ("NiriIPC: parse error: ", e.what ());
		}
	    }
	    if (buf.size () > SAFETY_CAP) {
		buf.clear ();
	    }
	}

	m_socketFd.store (-1);
	::close (fd);
	if (!m_stop.load ()) {
	    sLog.debug ("NiriIPC: connection closed, reconnecting");
	    std::this_thread::sleep_for (backoff);
	    backoff = std::min (backoff * 2, maxBackoff);
	}
    }
}

} // namespace WallpaperEngine::Render::Drivers::Detectors

#endif /* ENABLE_WAYLAND */
