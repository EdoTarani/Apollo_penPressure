#include <windows.h>
#include <iostream>
#include <vector>
#include <setupapi.h>
#include <initguid.h>
#include <combaseapi.h>
#include <thread>
#include <algorithm>
#include <climits>
#include <map>
#include <mutex>

#include <wrl/client.h>
#include <dxgi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <dxgi1_6.h>

#include "virtual_display.h"

using namespace SUDOVDA;

namespace VDISPLAY {
// {dff7fd29-5b75-41d1-9731-b32a17a17104}
// static const GUID DEFAULT_DISPLAY_GUID = { 0xdff7fd29, 0x5b75, 0x41d1, { 0x97, 0x31, 0xb3, 0x2a, 0x17, 0xa1, 0x71, 0x04 } };

HANDLE SUDOVDA_DRIVER_HANDLE = INVALID_HANDLE_VALUE;

// ---- Display broker ----
// SudoVDA accepts one open handle at a time, so only one Apollo process can talk to it. With
// extra screens, the main instance keeps the driver and serves add/remove requests over a local
// named pipe; the extra screens' processes create and remove their displays through it.

namespace {
	std::wstring g_brokerPipe;  // non-empty: this process is a broker client (an extra screen)

	// Displays this process created, by GUID: creating one again returns it (the main instance
	// creates the extra screens' displays up front; their own requests then just find them)
	std::mutex g_createdMutex;
	std::map<std::string, std::wstring> g_created;

	std::string guidKey(const GUID& guid) {
		return std::string(reinterpret_cast<const char*>(&guid), sizeof(GUID));
	}

	bool g_physicalDisabled = false;
	int g_screenIndex = 0;           // broker client: which extra screen we are
	bool g_brokerArrange = false;    // broker: place new displays in the row
	bool g_keepPhysicalOff = false;  // broker: switch the monitors off after adding a display

	// All display configuration changes happen one at a time (session thread and broker)
	std::recursive_mutex g_configMutex;

	// Screen of each virtual display (0 = screen 1), for the row layout
	std::map<std::wstring, int> g_slots;

	// The physical monitors' exact layout before they were switched off, to put back
	std::vector<DISPLAYCONFIG_PATH_INFO> g_savedPaths;
	std::vector<DISPLAYCONFIG_MODE_INFO> g_savedModes;

	bool isSudoAdapter(const wchar_t* gdiName) {
		DISPLAY_DEVICEW adapter {};
		adapter.cb = sizeof(adapter);
		for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); i++, adapter.cb = sizeof(adapter)) {
			if (_wcsicmp(adapter.DeviceName, gdiName) == 0) {
				return wcsstr(adapter.DeviceString, L"Sudo") != nullptr;
			}
		}
		return false;
	}

	bool isSudoPath(const DISPLAYCONFIG_PATH_INFO& path) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
		source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		source.header.size = sizeof(source);
		source.header.adapterId = path.sourceInfo.adapterId;
		source.header.id = path.sourceInfo.id;
		return DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS && isSudoAdapter(source.viewGdiDeviceName);
	}

	bool activePaths(std::vector<DISPLAYCONFIG_PATH_INFO>& paths, std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
		UINT32 pathCount = 0, modeCount = 0;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
			return false;
		}
		paths.resize(pathCount);
		modes.resize(modeCount);
		if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) {
			return false;
		}
		paths.resize(pathCount);
		modes.resize(modeCount);
		return true;
	}

	enum : uint32_t { BROKER_ADD = 1, BROKER_REMOVE = 2 };

	struct BrokerRequest {
		uint32_t op;
		GUID guid;
		uint32_t width, height, fps;
		char clientUid[128];
		char clientName[128];
		uint32_t screen;  // the extra screen asking (2, 3)
	};

	struct BrokerReply {
		uint32_t ok;
		wchar_t deviceName[CCHDEVICENAME];
	};

	bool brokerCall(BrokerRequest& request, BrokerReply& reply) {
		DWORD read = 0;
		// Connect, send, receive, disconnect; waits up to 10 s for a free pipe instance
		if (!CallNamedPipeW(g_brokerPipe.c_str(), &request, sizeof(request), &reply, sizeof(reply), &read, 60000)) {
			printf("[SUDOVDA] Display broker unreachable (%lu)\n", GetLastError());
			return false;
		}
		return read == sizeof(reply) && reply.ok;
	}
}

void useDisplayBroker(const std::wstring& pipeName, int screenIndex) {
	g_brokerPipe = pipeName;
	g_screenIndex = screenIndex;
}

bool waitForDisplayActive(const wchar_t* deviceName, int timeoutMs) {
	for (int waited = 0;; waited += 100) {
		DISPLAY_DEVICEW adapter {};
		adapter.cb = sizeof(adapter);
		for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); i++, adapter.cb = sizeof(adapter)) {
			if (_wcsicmp(adapter.DeviceName, deviceName) == 0 && (adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) {
				return true;
			}
		}
		if (waited >= timeoutMs) {
			return false;
		}
		Sleep(100);
	}
}

bool extendAllDisplays() {
	std::lock_guard lock(g_configMutex);
	LONG result = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
	printf("[SUDOVDA] Extend all displays: %ld\n", result);
	return result == ERROR_SUCCESS;
}

/**
 * Switch on the display at (adapter, target) when Windows left it off, e.g. because it applied a
 * remembered layout in which this monitor was off: its path joins the active configuration with
 * a free source, and the result is remembered (so Windows switches it on by itself next time).
 */
bool activateTarget(const LUID& adapter, UINT32 targetId) {
	std::lock_guard lock(g_configMutex);
	UINT32 pathCount = 0, modeCount = 0;
	if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
		return false;
	}
	std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
	if (QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) {
		return false;
	}
	paths.resize(pathCount);
	modes.resize(modeCount);

	auto sameLuid = [](const LUID& a, const LUID& b) {
		return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
	};
	std::vector<DISPLAYCONFIG_PATH_INFO> active;
	for (auto& path : paths) {
		if (path.flags & DISPLAYCONFIG_PATH_ACTIVE) {
			active.push_back(path);
		}
	}

	for (auto& candidate : paths) {
		if ((candidate.flags & DISPLAYCONFIG_PATH_ACTIVE) || !candidate.targetInfo.targetAvailable ||
			!sameLuid(candidate.targetInfo.adapterId, adapter) || candidate.targetInfo.id != targetId) {
			continue;
		}
		bool sourceTaken = false;
		for (auto& path : active) {
			if (sameLuid(path.sourceInfo.adapterId, candidate.sourceInfo.adapterId) && path.sourceInfo.id == candidate.sourceInfo.id) {
				sourceTaken = true;
				break;
			}
		}
		if (sourceTaken) {
			continue;
		}

		auto path = candidate;
		path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
		path.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
		path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
		auto config = active;
		config.push_back(path);
		LONG result = SetDisplayConfig((UINT32) config.size(), config.data(), (UINT32) modes.size(), modes.data(),
			SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
		printf("[SUDOVDA] Switched on display target %u: %ld\n", targetId, result);
		return result == ERROR_SUCCESS;
	}
	printf("[SUDOVDA] Display target %u: no free path to switch it on\n", targetId);
	return false;
}

void setKeepPhysicalOff(bool on) {
	g_keepPhysicalOff = on;
}

std::wstring describeDisplays() {
	std::wstring out;
	DISPLAY_DEVICEW adapter {};
	adapter.cb = sizeof(adapter);
	for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); i++, adapter.cb = sizeof(adapter)) {
		if (!(adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) {
			continue;
		}
		DEVMODEW mode {};
		mode.dmSize = sizeof(mode);
		EnumDisplaySettingsExW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0);
		wchar_t line[256];
		swprintf_s(line, L"%ls%ls %ls%lux%lu at %ld,%ld", out.empty() ? L"" : L"; ", adapter.DeviceName,
			wcsstr(adapter.DeviceString, L"Sudo") ? L"virtual " : L"", mode.dmPelsWidth, mode.dmPelsHeight,
			mode.dmPosition.x, mode.dmPosition.y);
		out += line;
	}
	return out.empty() ? L"none" : out;
}

bool isDisplayBrokerClient() {
	return !g_brokerPipe.empty();
}

bool arrangeInRow(const wchar_t* deviceName, int slot) {
	std::lock_guard lock(g_configMutex);

	// Everything through SetDisplayConfig: the legacy ChangeDisplaySettingsEx(nullptr) apply
	// re-reads the registry and would switch monitors back on that we switched off
	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
	if (!activePaths(paths, modes)) {
		return false;
	}

	LONG right = 0;
	DISPLAYCONFIG_SOURCE_MODE* ours = nullptr;
	for (auto& path : paths) {
		auto idx = path.sourceInfo.modeInfoIdx;
		if (idx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || idx >= modes.size()) {
			continue;
		}
		DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
		source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		source.header.size = sizeof(source);
		source.header.adapterId = path.sourceInfo.adapterId;
		source.header.id = path.sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
			continue;
		}
		auto& mode = modes[idx].sourceMode;
		if (_wcsicmp(source.viewGdiDeviceName, deviceName) == 0) {
			ours = &mode;
		} else if (!isSudoAdapter(source.viewGdiDeviceName)) {
			right = (std::max)(right, mode.position.x + (LONG) mode.width);
		}
	}
	if (ours == nullptr) {
		wprintf(L"[SUDOVDA] Can't place %ls: it isn't on\n", deviceName);
		return false;
	}

	ours->position.x = right + slot * (LONG) ours->width;
	ours->position.y = 0;
	LONG result = SetDisplayConfig((UINT32) paths.size(), paths.data(), (UINT32) modes.size(), modes.data(),
		SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
	wprintf(L"[SUDOVDA] Placed %ls at x=%ld (slot %d): %ld\n", deviceName, ours->position.x, slot, result);
	return result == ERROR_SUCCESS;
}

void removeAllVirtualDisplays() {
	std::vector<std::string> keys;
	{
		std::lock_guard lock(g_createdMutex);
		for (auto& [key, name] : g_created) {
			keys.push_back(key);
		}
	}
	for (auto& key : keys) {
		GUID guid;
		memcpy(&guid, key.data(), sizeof(GUID));
		removeVirtualDisplay(guid);
	}
}

bool keepOnlyVirtualDisplays() {
	std::lock_guard lock(g_configMutex);
	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
	if (!activePaths(paths, modes)) {
		return false;
	}

	// Supply only the virtual displays' paths (and their modes): the others go off.
	// Remember the others (the monitors' positions haven't changed) to put them back later.
	std::vector<DISPLAYCONFIG_PATH_INFO> keep, saved;
	std::vector<DISPLAYCONFIG_MODE_INFO> keepModes, savedModes;
	auto copyMode = [&](std::vector<DISPLAYCONFIG_MODE_INFO>& to, UINT32& idx) {
		if (idx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && idx < modes.size()) {
			to.push_back(modes[idx]);
			idx = (UINT32) to.size() - 1;
		}
	};
	for (auto path : paths) {
		bool isVirtual = isSudoPath(path);
		auto& to = isVirtual ? keepModes : savedModes;
		copyMode(to, path.sourceInfo.modeInfoIdx);
		copyMode(to, path.targetInfo.modeInfoIdx);
		(isVirtual ? keep : saved).push_back(path);
	}
	if (keep.empty() || keep.size() == paths.size()) {
		return false;  // never switch every display off; or nothing to switch off
	}

	// The primary display must sit at 0,0: move the leftmost virtual display there
	LONG minX = LONG_MAX, minY = 0;
	for (auto& mode : keepModes) {
		if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE && mode.sourceMode.position.x < minX) {
			minX = mode.sourceMode.position.x;
			minY = mode.sourceMode.position.y;
		}
	}
	for (auto& mode : keepModes) {
		if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
			mode.sourceMode.position.x -= minX;
			mode.sourceMode.position.y -= minY;
		}
	}

	LONG result = SetDisplayConfig((UINT32) keep.size(), keep.data(), (UINT32) keepModes.size(), keepModes.data(),
		SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
	printf("[SUDOVDA] Physical displays off (%zu virtual kept): %ld\n", keep.size(), result);
	if (result == ERROR_SUCCESS) {
		// Windows may switch the monitors back on when another display arrives; the layout
		// to restore is the one from before the first time
		if (!g_physicalDisabled) {
			g_savedPaths = std::move(saved);
			g_savedModes = std::move(savedModes);
		}
		g_physicalDisabled = true;
	}
	return result == ERROR_SUCCESS;
}

bool makeMainDisplay(const wchar_t* deviceName) {
	std::lock_guard lock(g_configMutex);
	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
	if (!activePaths(paths, modes)) {
		return false;
	}

	// Find our display's position, and remember the monitors' layout (first change only)
	POINTL origin {};
	bool found = false;
	std::vector<DISPLAYCONFIG_PATH_INFO> saved;
	std::vector<DISPLAYCONFIG_MODE_INFO> savedModes;
	for (auto path : paths) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
		source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		source.header.size = sizeof(source);
		source.header.adapterId = path.sourceInfo.adapterId;
		source.header.id = path.sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
			continue;
		}
		auto idx = path.sourceInfo.modeInfoIdx;
		if (_wcsicmp(source.viewGdiDeviceName, deviceName) == 0 && idx < modes.size()) {
			origin = modes[idx].sourceMode.position;
			found = true;
		} else if (!isSudoAdapter(source.viewGdiDeviceName)) {
			for (auto* i : {&path.sourceInfo.modeInfoIdx, &path.targetInfo.modeInfoIdx}) {
				if (*i != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && *i < modes.size()) {
					savedModes.push_back(modes[*i]);
					*i = (UINT32) savedModes.size() - 1;
				}
			}
			saved.push_back(path);
		}
	}
	if (!found) {
		wprintf(L"[SUDOVDA] Can't make %ls the main display: it isn't on\n", deviceName);
		return false;
	}

	// Shift the whole desktop so our display sits at 0,0
	for (auto& mode : modes) {
		if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
			mode.sourceMode.position.x -= origin.x;
			mode.sourceMode.position.y -= origin.y;
		}
	}
	LONG result = SetDisplayConfig((UINT32) paths.size(), paths.data(), (UINT32) modes.size(), modes.data(),
		SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
	wprintf(L"[SUDOVDA] %ls is the main display: %ld\n", deviceName, result);
	if (result == ERROR_SUCCESS) {
		if (!g_physicalDisabled && !saved.empty()) {
			g_savedPaths = std::move(saved);
			g_savedModes = std::move(savedModes);
		}
		g_physicalDisabled = true;  // the layout to restore is saved
	}
	return result == ERROR_SUCCESS;
}

void setDisplaySlot(const wchar_t* deviceName, int slot) {
	std::lock_guard lock(g_configMutex);
	g_slots[deviceName] = slot;
}

bool layoutRow() {
	std::lock_guard lock(g_configMutex);
	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
	if (!activePaths(paths, modes)) {
		return false;
	}

	struct VirtualDisplay {
		int slot;
		UINT32 mode;
		std::wstring name;
	};
	std::vector<VirtualDisplay> virtuals;
	std::vector<UINT32> physicalModes;
	std::vector<DISPLAYCONFIG_PATH_INFO> saved;
	std::vector<DISPLAYCONFIG_MODE_INFO> savedModes;
	for (auto path : paths) {
		auto idx = path.sourceInfo.modeInfoIdx;
		if (idx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || idx >= modes.size()) {
			continue;
		}
		DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
		source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		source.header.size = sizeof(source);
		source.header.adapterId = path.sourceInfo.adapterId;
		source.header.id = path.sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
			continue;
		}
		if (isSudoAdapter(source.viewGdiDeviceName)) {
			auto known = g_slots.find(source.viewGdiDeviceName);
			int slot = known != g_slots.end() ? known->second : 100 + (int) virtuals.size();
			virtuals.push_back({slot, idx, source.viewGdiDeviceName});
		} else {
			physicalModes.push_back(idx);
			// The monitors' layout before we move them, to put back afterwards
			for (auto* i : {&path.sourceInfo.modeInfoIdx, &path.targetInfo.modeInfoIdx}) {
				if (*i != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && *i < modes.size()) {
					savedModes.push_back(modes[*i]);
					*i = (UINT32) savedModes.size() - 1;
				}
			}
			saved.push_back(path);
		}
	}
	if (virtuals.empty()) {
		return false;
	}

	// Screens left to right from 0,0
	std::sort(virtuals.begin(), virtuals.end(), [](auto& a, auto& b) { return a.slot < b.slot; });
	LONG x = 0;
	std::wstring summary;
	for (auto& v : virtuals) {
		auto& mode = modes[v.mode].sourceMode;
		mode.position = {x, 0};
		x += (LONG) mode.width;
		summary += v.name + L" ";
	}

	// Monitors that are on: same layout, right edge at screen 1's left edge
	if (!physicalModes.empty()) {
		LONG maxRight = LONG_MIN, minTop = LONG_MAX;
		for (auto idx : physicalModes) {
			auto& mode = modes[idx].sourceMode;
			maxRight = (std::max)(maxRight, mode.position.x + (LONG) mode.width);
			minTop = (std::min)(minTop, mode.position.y);
		}
		for (auto idx : physicalModes) {
			modes[idx].sourceMode.position.x -= maxRight;
			modes[idx].sourceMode.position.y -= minTop;
		}
	}

	LONG result = SetDisplayConfig((UINT32) paths.size(), paths.data(), (UINT32) modes.size(), modes.data(),
		SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
	wprintf(L"[SUDOVDA] Row %ls: %ld\n", summary.c_str(), result);
	if (result == ERROR_SUCCESS && !saved.empty()) {
		if (!g_physicalDisabled) {
			g_savedPaths = std::move(saved);
			g_savedModes = std::move(savedModes);
		}
		g_physicalDisabled = true;  // the layout to restore is saved
	}
	return result == ERROR_SUCCESS;
}

void restorePhysicalDisplays() {
	std::lock_guard lock(g_configMutex);
	if (!g_physicalDisabled) {
		return;
	}
	g_physicalDisabled = false;

	// Wait (up to 3 s) until Windows has let go of the removed virtual displays: a layout applied
	// while they're still connected is remembered with them off, and Windows then keeps them off
	// the next time they come (they'd never get a name)
	for (int waited = 0; waited < 3000; waited += 100) {
		bool virtualLeft = false;
		DISPLAY_DEVICEW adapter {};
		adapter.cb = sizeof(adapter);
		for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); i++, adapter.cb = sizeof(adapter)) {
			if ((adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) && wcsstr(adapter.DeviceString, L"Sudo")) {
				virtualLeft = true;
			}
		}
		if (!virtualLeft) {
			break;
		}
		Sleep(100);
	}

	// Exactly the layout from before, not saved as Windows' remembered layout (it has the user's
	// own); if that no longer fits (e.g. a monitor was unplugged), the usual extended layout
	LONG result = ERROR_INVALID_PARAMETER;
	if (!g_savedPaths.empty()) {
		result = SetDisplayConfig((UINT32) g_savedPaths.size(), g_savedPaths.data(), (UINT32) g_savedModes.size(), g_savedModes.data(),
			SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
	}
	if (result != ERROR_SUCCESS) {
		result = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
	}
	g_savedPaths.clear();
	g_savedModes.clear();
	printf("[SUDOVDA] Physical displays back on: %ld\n", result);
}

void ensurePhysicalDisplaysOn() {
	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
	if (!activePaths(paths, modes)) {
		return;
	}
	for (auto& path : paths) {
		if (!isSudoPath(path)) {
			return;  // a physical display is on
		}
	}
	LONG result = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
	printf("[SUDOVDA] No physical display was on, switched them on: %ld\n", result);
}

void startDisplayBroker(const std::wstring& pipeName, bool arrange) {
	g_brokerArrange = arrange;
	std::thread([pipeName] {
		for (;;) {
			// Local clients only; SYSTEM/admins may write (default pipe security)
			HANDLE pipe = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX,
				PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
				PIPE_UNLIMITED_INSTANCES, sizeof(BrokerReply), sizeof(BrokerRequest), 0, nullptr);
			if (pipe == INVALID_HANDLE_VALUE) {
				printf("[SUDOVDA] Display broker: can't create pipe (%lu)\n", GetLastError());
				Sleep(5000);
				continue;
			}

			if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
				BrokerRequest request {};
				BrokerReply reply {};
				DWORD read = 0, written = 0;
				if (ReadFile(pipe, &request, sizeof(request), &read, nullptr) && read == sizeof(request)) {
					request.clientUid[sizeof(request.clientUid) - 1] = 0;
					request.clientName[sizeof(request.clientName) - 1] = 0;
					if (request.op == BROKER_ADD) {
						std::lock_guard lock(g_configMutex);
						auto name = createVirtualDisplay(request.clientUid, request.clientName,
							request.width, request.height, request.fps, request.guid);
						if (!name.empty() && name.size() < CCHDEVICENAME) {
							// One screen at a time: Windows switches it on, then it takes its place
							changeDisplaySettings(name.c_str(), request.width, request.height, request.fps);
							if (!waitForDisplayActive(name.c_str(), 3000)) {
								wprintf(L"[SUDOVDA] %ls isn't on: extending all displays\n", name.c_str());
								extendAllDisplays();
								waitForDisplayActive(name.c_str(), 3000);
							}
							if (request.screen >= 2) {
								setDisplaySlot(name.c_str(), (int) request.screen - 1);
							}
							if (g_keepPhysicalOff) {
								keepOnlyVirtualDisplays();
							}
							if (g_brokerArrange) {
								layoutRow();
							}
							wprintf(L"[SUDOVDA] Displays on: %ls\n", describeDisplays().c_str());
							wcscpy_s(reply.deviceName, name.c_str());
							reply.ok = 1;
						}
					} else if (request.op == BROKER_REMOVE) {
						reply.ok = removeVirtualDisplay(request.guid) ? 1 : 0;
					}
					WriteFile(pipe, &reply, sizeof(reply), &written, nullptr);
					FlushFileBuffers(pipe);
				}
				DisconnectNamedPipe(pipe);
			}
			CloseHandle(pipe);
		}
	}).detach();
}

// START ISOLATED DISPLAY DECLARATIONS
struct positionwidthheight;
struct coordinates;
struct coordinatesdifferences;
struct coordinates
{
	int x;
	int y;
};

struct positionwidthheight
{
	struct coordinates position;
	int width;
	int height;
	int modeindex;
};

struct coordinatesdifferences
{
	struct coordinates left;
	struct coordinates right;
	struct coordinates Difference;
	struct coordinates AbsDifference;

};

std::vector <std::wstring> matchDisplay(std::wstring sMatch);
std::vector< struct positionwidthheight*>rearrangeVirtualDisplayForLowerRight(std::vector< struct positionwidthheight*> displays);
std::string printAllDisplays(std::vector< struct positionwidthheight*> displays);
std::vector < struct coordinates > moveToBeConnected(std::vector < struct coordinates > unknown, std::vector< struct coordinates> connected);

// END ISOLATED DISPLAY DECLARATIONS

LONG getDeviceSettings(const wchar_t* deviceName, DEVMODEW& devMode) {
	devMode.dmSize = sizeof(DEVMODEW);
	return EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &devMode);
}

LONG changeDisplaySettings2(const wchar_t* deviceName, int width, int height, int refresh_rate, bool bApplyIsolated) {
	UINT32 pathCount = 0;
	UINT32 modeCount = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount)) {
		wprintf(L"[SUDOVDA] Failed to query display configuration size.\n");
		return ERROR_INVALID_PARAMETER;
	}

	std::vector<DISPLAYCONFIG_PATH_INFO> pathArray(pathCount);
	std::vector<DISPLAYCONFIG_MODE_INFO> modeArray(modeCount);
	std::vector<struct positionwidthheight *> displayArray;
	struct positionwidthheight *pCurrentElement;

	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, pathArray.data(), &modeCount, modeArray.data(), nullptr) != ERROR_SUCCESS) {
		wprintf(L"[SUDOVDA] Failed to query display configuration.\n");
		return ERROR_INVALID_PARAMETER;
	}

	bool bAtVirtualDisplay;
	bool bVirtualDisplayAlreadyAdded = false;
	std::string sDisplayOutput;

	if (bApplyIsolated == true)
	{
		for (UINT32 i = 0; i < pathCount; i++) {
			DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
			sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
			sourceName.header.size = sizeof(sourceName);
			sourceName.header.adapterId = pathArray[i].sourceInfo.adapterId;
			sourceName.header.id = pathArray[i].sourceInfo.id;
			bAtVirtualDisplay = false;

			if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
				continue;
			}

			auto* sourceInfo = &pathArray[i].sourceInfo;
			auto* targetInfo = &pathArray[i].targetInfo;

			if (std::wstring_view(sourceName.viewGdiDeviceName) == std::wstring_view(deviceName))
			{
				bAtVirtualDisplay = true;
			}

			if ( true ) {
				for (UINT32 j = 0; j < modeCount; j++) {
					if (
						modeArray[j].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
						modeArray[j].adapterId.HighPart == sourceInfo->adapterId.HighPart &&
						modeArray[j].adapterId.LowPart == sourceInfo->adapterId.LowPart &&
						modeArray[j].id == sourceInfo->id
						) {
						auto* sourceMode = &modeArray[j].sourceMode;

						wprintf(L"[SUDOVDA] Current mode found: [%dx%dx%d]\n", sourceMode->width, sourceMode->height, targetInfo->refreshRate);

						pCurrentElement = new (struct positionwidthheight);

						pCurrentElement->position.x = modeArray[j].sourceMode.position.x;
						pCurrentElement->position.y = modeArray[j].sourceMode.position.y;
						pCurrentElement->height = modeArray[j].sourceMode.height;
						pCurrentElement->width = modeArray[j].sourceMode.width;
						pCurrentElement->modeindex = j;

						// This is the virtual display - insert at the front of the vector
						if (bAtVirtualDisplay == true && bVirtualDisplayAlreadyAdded == false)
						{
							displayArray.insert( displayArray.begin()+0, pCurrentElement);
							bVirtualDisplayAlreadyAdded = true;
						} else 	{
							displayArray.push_back(pCurrentElement);
						}
					}
				}
			}
		}

		sDisplayOutput = "";
		sDisplayOutput += "Before: \n";
		sDisplayOutput += printAllDisplays(displayArray);

		displayArray = rearrangeVirtualDisplayForLowerRight(displayArray);

		sDisplayOutput += "";
		sDisplayOutput += "After: \n";
		sDisplayOutput += printAllDisplays(displayArray);

		int iIndex;
		int xdifference, ydifference = 0;
		for (iIndex = 0; iIndex < displayArray.size(); iIndex += 1)
		{

			// Find the primary display and get the offset to apply to all of the displays to keep the same primary
			if( modeArray[(displayArray[iIndex]->modeindex)].sourceMode.position.x == 0 &&
			    modeArray[(displayArray[iIndex]->modeindex)].sourceMode.position.y == 0 )
				{
					xdifference = (displayArray[iIndex]->position.x) * -1;
					ydifference = (displayArray[iIndex]->position.y) * -1;
					break;
				}
		}

		// Set all of the OS Displays to their new locations; Do not change the primary
		// Update the real vector for the system call
		for (iIndex = 0; iIndex < displayArray.size(); iIndex += 1)
		{
			modeArray[(displayArray[iIndex]->modeindex)].sourceMode.position.x = displayArray[iIndex]->position.x + xdifference;
			modeArray[(displayArray[iIndex]->modeindex)].sourceMode.position.y = displayArray[iIndex]->position.y + ydifference;
			modeArray[(displayArray[iIndex]->modeindex)].sourceMode.height = displayArray[iIndex]->height;
			modeArray[(displayArray[iIndex]->modeindex)].sourceMode.width = displayArray[iIndex]->width;
		}

		// Apply the changes only if the virtual display was found
		if( bVirtualDisplayAlreadyAdded == true ) {
			LONG status = SetDisplayConfig(
				pathCount,
				pathArray.data(),
				modeCount,
				modeArray.data(),
				SDC_APPLY
				| SDC_USE_SUPPLIED_DISPLAY_CONFIG
				| SDC_SAVE_TO_DATABASE
			);
			if (status != ERROR_SUCCESS) {
				wprintf(L"[SUDOVDA] Failed to apply display settings.\n");
			} else {
				wprintf(L"[SUDOVDA] Display settings updated successfully.\n");
			}
		}
		for (iIndex = 0; iIndex < displayArray.size(); iIndex += 1)
		{
			if (displayArray[iIndex] != nullptr)
			{
				delete displayArray[iIndex];
			}
			displayArray.clear();
		}
	}

	// After performing the isolated display movements, do the regular movements
	for (UINT32 i = 0; i < pathCount; i++) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
		sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		sourceName.header.size = sizeof(sourceName);
		sourceName.header.adapterId = pathArray[i].sourceInfo.adapterId;
		sourceName.header.id = pathArray[i].sourceInfo.id;

		if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
			continue;
		}

		auto* sourceInfo = &pathArray[i].sourceInfo;
		auto* targetInfo = &pathArray[i].targetInfo;

		if (std::wstring_view(sourceName.viewGdiDeviceName) == std::wstring_view(deviceName)) {
			wprintf(L"[SUDOVDA] Display found: %ls\n", deviceName);
			for (UINT32 j = 0; j < modeCount; j++) {
				if (
					modeArray[j].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
					modeArray[j].adapterId.HighPart == sourceInfo->adapterId.HighPart &&
					modeArray[j].adapterId.LowPart == sourceInfo->adapterId.LowPart &&
					modeArray[j].id == sourceInfo->id
				) {
					auto* sourceMode = &modeArray[j].sourceMode;

					wprintf(L"[SUDOVDA] Current mode found: [%dx%dx%d]\n", sourceMode->width, sourceMode->height, targetInfo->refreshRate);

					sourceMode->width = width;
					sourceMode->height = height;

					targetInfo->refreshRate = {(UINT32)refresh_rate, 1000};

					// Apply the changes
					LONG status = SetDisplayConfig(
						pathCount,
						pathArray.data(),
						modeCount,
						modeArray.data(),
						SDC_APPLY
						| SDC_USE_SUPPLIED_DISPLAY_CONFIG
						| SDC_SAVE_TO_DATABASE
					);
					if (status != ERROR_SUCCESS) {
						wprintf(L"[SUDOVDA] Failed to apply display settings.\n");
					} else {
						wprintf(L"[SUDOVDA] Display settings updated successfully.\n");
					}

					return status;
				}
			}

			wprintf(L"[SUDOVDA] Mode [%dx%dx%d] not found for display: %ls\n", width, height, refresh_rate, deviceName);
			return ERROR_INVALID_PARAMETER;
		}
	}

	wprintf(L"[SUDOVDA] Display not found: %ls\n", deviceName);
	return ERROR_DEVICE_NOT_CONNECTED;
}

LONG changeDisplaySettings(const wchar_t* deviceName, int width, int height, int refresh_rate) {
	DEVMODEW devMode = {};
	devMode.dmSize = sizeof(devMode);

	// Old method to set at least baseline refresh rate
	if (EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &devMode)) {
		DWORD targetRefreshRate = refresh_rate / 1000;
		DWORD altRefreshRate = targetRefreshRate;

		if (refresh_rate % 1000) {
			if (refresh_rate % 1000 >= 900) {
				targetRefreshRate += 1;
			} else {
				altRefreshRate += 1;
			}
		} else {
			altRefreshRate -= 1;
		}

		wprintf(L"[SUDOVDA] Applying baseline display mode [%dx%dx%d] for %ls.\n", width, height, targetRefreshRate, deviceName);

		devMode.dmPelsWidth = width;
		devMode.dmPelsHeight = height;
		devMode.dmDisplayFrequency = targetRefreshRate;
		devMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;

		auto res = ChangeDisplaySettingsExW(deviceName, &devMode, NULL, CDS_UPDATEREGISTRY, NULL);

		if (res != ERROR_SUCCESS) {
			wprintf(L"[SUDOVDA] Failed to apply baseline display mode, trying alt mode: [%dx%dx%d].\n", width, height, altRefreshRate);
			devMode.dmDisplayFrequency = altRefreshRate;
			res = ChangeDisplaySettingsExW(deviceName, &devMode, NULL, CDS_UPDATEREGISTRY, NULL);
			if (res != ERROR_SUCCESS) {
				wprintf(L"[SUDOVDA] Failed to apply alt baseline display mode.\n");
			}
		}

		if (res == ERROR_SUCCESS) {
			wprintf(L"[SUDOVDA] Baseline display mode applied successfully.");
		}
	}

	// Use new method to set refresh rate if fine tuned
	return changeDisplaySettings2(deviceName, width, height, refresh_rate);
}


std::wstring getPrimaryDisplay() {
	DISPLAY_DEVICEW displayDevice;
	displayDevice.cb = sizeof(DISPLAY_DEVICE);

	std::wstring primaryDeviceName;

	int deviceIndex = 0;
	while (EnumDisplayDevicesW(NULL, deviceIndex, &displayDevice, 0)) {
		if (displayDevice.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
			primaryDeviceName = displayDevice.DeviceName;
			break;
		}
		deviceIndex++;
	}

	return primaryDeviceName;
}

bool setPrimaryDisplay(const wchar_t* primaryDeviceName) {
	DEVMODEW primaryDevMode{};
	if (!getDeviceSettings(primaryDeviceName, primaryDevMode)) {
		return false;
	};

	int offset_x = primaryDevMode.dmPosition.x;
	int offset_y = primaryDevMode.dmPosition.y;

	LONG result;

	DISPLAY_DEVICEW displayDevice;
	displayDevice.cb = sizeof(DISPLAY_DEVICEA);
	int device_index = 0;

	while (EnumDisplayDevicesW(NULL, device_index, &displayDevice, 0)) {
		device_index++;
		if (!(displayDevice.StateFlags & DISPLAY_DEVICE_ACTIVE)) {
			continue;
		}

		DEVMODEW devMode{};
		if (getDeviceSettings(displayDevice.DeviceName, devMode)) {
			devMode.dmPosition.x -= offset_x;
			devMode.dmPosition.y -= offset_y;
			devMode.dmFields = DM_POSITION;

			result = ChangeDisplaySettingsExW(displayDevice.DeviceName, &devMode, NULL, CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
			if (result != DISP_CHANGE_SUCCESSFUL) {
				wprintf(L"[SUDOVDA] Changing config for display %ls failed!\n\n", displayDevice.DeviceName);
				return false;
			}
		}
	}

	// Update primary device's config to ensure it's primary
	primaryDevMode.dmPosition.x = 0;
	primaryDevMode.dmPosition.y = 0;
	primaryDevMode.dmFields = DM_POSITION;
	result = ChangeDisplaySettingsExW(primaryDeviceName, &primaryDevMode, NULL, CDS_UPDATEREGISTRY | CDS_NORESET | CDS_SET_PRIMARY, NULL);
	if (result != DISP_CHANGE_SUCCESSFUL) {
		wprintf(L"[SUDOVDA] Changing config for primary display %ls failed!\n\n", primaryDeviceName);
		return false;
	}

	wprintf(L"[SUDOVDA] Applying primary display %ls ...\n\n", primaryDeviceName);

	result = ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
	if (result != DISP_CHANGE_SUCCESSFUL) {
		wprintf(L"[SUDOVDA] Applying display coinfig failed!\n\n");
		return false;
	}

	return true;
}

bool findDisplayIds(const wchar_t* displayName, LUID& adapterId, uint32_t& targetId) {
	UINT32 pathCount;
	UINT32 modeCount;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount)) {
		return false;
	}

	std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr)) {
		return false;
	}

	auto path = std::find_if(paths.begin(), paths.end(), [&displayName](DISPLAYCONFIG_PATH_INFO _path) {
		DISPLAYCONFIG_PATH_SOURCE_INFO sourceInfo = _path.sourceInfo;

		DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
		sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		sourceName.header.size = sizeof(sourceName);
		sourceName.header.adapterId = sourceInfo.adapterId;
		sourceName.header.id = sourceInfo.id;

		if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
			return false;
		}

		return std::wstring_view(displayName) == sourceName.viewGdiDeviceName;
	});

	if (path == paths.end()) {
		return false;
	}

	adapterId = path->sourceInfo.adapterId;
	targetId = path->targetInfo.id;

	return true;
}

bool getDisplayHDR(const LUID& adapterLuid, const wchar_t* displayName) {
	Microsoft::WRL::ComPtr<IDXGIFactory1> dxgiFactory;
	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));
	if (FAILED(hr)) {
		wprintf(L"[SUDOVDA] CreateDXGIFactory1 failed in getDisplayHDR! hr=0x%lx\n", hr);
		return false;
	}

	for (UINT adapterIdx = 0; ; ++adapterIdx) {
		Microsoft::WRL::ComPtr<IDXGIAdapter1> currentAdapter;
		hr = dxgiFactory->EnumAdapters1(adapterIdx, currentAdapter.ReleaseAndGetAddressOf());

		if (hr == DXGI_ERROR_NOT_FOUND) {
			break; // No more adapters
		}
		if (FAILED(hr)) {
			wprintf(L"[SUDOVDA] EnumAdapters1 failed for index %u in getDisplayHDR! hr=0x%lx\n", adapterIdx, hr);
			break;
		}

		DXGI_ADAPTER_DESC1 adapterDesc;
		hr = currentAdapter->GetDesc1(&adapterDesc);
		if (FAILED(hr)) {
			wprintf(L"[SUDOVDA] GetDesc1 (Adapter) failed for index %u in getDisplayHDR! hr=0x%lx\n", adapterIdx, hr);
			continue;
		}

		if (adapterDesc.AdapterLuid.LowPart == adapterLuid.LowPart &&
			adapterDesc.AdapterLuid.HighPart == adapterLuid.HighPart) {

			std::wstring_view displayName_view{displayName};

			// Adapter found. Now iterate its outputs and match against targetGdiDeviceName.
			for (UINT outputIdx = 0; ; ++outputIdx) {
				Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;
				hr = currentAdapter->EnumOutputs(outputIdx, dxgiOutput.ReleaseAndGetAddressOf());

				if (hr == DXGI_ERROR_NOT_FOUND) {
					wprintf(L"[SUDOVDA] No more DXGI outputs on matched adapter for GDI name %ls.\n", displayName);
					break; // No more outputs on this adapter
				}
				if (FAILED(hr) || !dxgiOutput) {
					continue; // Error, try next output
				}

				DXGI_OUTPUT_DESC dxgiOutputDesc;
				hr = dxgiOutput->GetDesc(&dxgiOutputDesc);
				if (FAILED(hr)) {
					continue;
				}

				MONITORINFOEXW monitorInfoEx = {};
				monitorInfoEx.cbSize = sizeof(MONITORINFOEXW);
				if (GetMonitorInfoW(dxgiOutputDesc.Monitor, &monitorInfoEx)) {
					if (displayName_view == monitorInfoEx.szDevice) {
						// This is the correct output!
						wprintf(L"[SUDOVDA] Matched DXGI output GDI name: %ls\n", monitorInfoEx.szDevice);
						Microsoft::WRL::ComPtr<IDXGIOutput6> dxgiOutput6;
						hr = dxgiOutput.As(&dxgiOutput6);

						if (SUCCEEDED(hr) && dxgiOutput6) {
							DXGI_OUTPUT_DESC1 outputDesc1;
							hr = dxgiOutput6->GetDesc1(&outputDesc1);
							if (SUCCEEDED(hr)) {
								if (outputDesc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
									return true; // HDR Active
								}
							} else {
								wprintf(L"[SUDOVDA] GetDesc1 (Output) failed for %ls. hr=0x%lx\n", monitorInfoEx.szDevice, hr);
							}
						} else {
							wprintf(L"[SUDOVDA] QueryInterface for IDXGIOutput6 failed for %ls. hr=0x%lx. HDR check method not available or output not capable.\n", monitorInfoEx.szDevice, hr);
						}
						// Matched the output, checked HDR (it was false or error). This is the only output we care about for this adapter.
						return false; // Return false as HDR not active or error for this specific display
					}
				} else {
					DWORD lastError = GetLastError();
					wprintf(L"[SUDOVDA] GetMonitorInfoW failed for HMONITOR 0x%p from DXGI output %ls. Error: %lu\n", dxgiOutputDesc.Monitor, dxgiOutputDesc.DeviceName, lastError);
				}
			} // end output enumeration loop for the matched adapter

			// If output loop completes, the targetGdiDeviceName was not found among this adapter's DXGI outputs.
			wprintf(L"[SUDOVDA] Target GDI name %ls not found among DXGI outputs of the matched adapter.\n", displayName);
			return false;
		}
	} // end adapter enumeration loop

	// If adapter loop completes without finding the adapterLuidFromCaller
	wprintf(L"[SUDOVDA] Target adapter LUID {%lx-%lx} not found via DXGI.\n", adapterLuid.HighPart, adapterLuid.LowPart);
	return false;
}

bool setDisplayHDR(const LUID& adapterId, const uint32_t& targetId, bool enableAdvancedColor) {
	DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE setHdrInfo = {};
	setHdrInfo.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
	setHdrInfo.header.size = sizeof(setHdrInfo);
	setHdrInfo.header.adapterId = adapterId;
	setHdrInfo.header.id = targetId;
	setHdrInfo.enableAdvancedColor = enableAdvancedColor;

	return DisplayConfigSetDeviceInfo(&setHdrInfo.header) == ERROR_SUCCESS;
}

bool getDisplayHDRByName(const wchar_t* displayName) {
	LUID adapterId;
	uint32_t targetId;

	if (!findDisplayIds(displayName, adapterId, targetId)) {
		wprintf(L"[SUDOVDA] Failed to find display IDs for %ls!\n", displayName);
		return false;
	}

	return getDisplayHDR(adapterId, displayName);
}

bool setDisplayHDRByName(const wchar_t* displayName, bool enableAdvancedColor) {
	LUID adapterId;
	uint32_t targetId;

	if (!findDisplayIds(displayName, adapterId, targetId)) {
		return false;
	}

	return setDisplayHDR(adapterId, targetId, enableAdvancedColor);
}

void closeVDisplayDevice() {
	if (isDisplayBrokerClient()) {
		return;
	}
	if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return;
	}

	CloseHandle(SUDOVDA_DRIVER_HANDLE);

	SUDOVDA_DRIVER_HANDLE = INVALID_HANDLE_VALUE;
}

DRIVER_STATUS openVDisplayDevice() {
	if (isDisplayBrokerClient()) {
		// The main instance owns the driver; requests go through its broker
		return DRIVER_STATUS::OK;
	}

	uint32_t retryInterval = 20;
	while (true) {
		SUDOVDA_DRIVER_HANDLE = OpenDevice(&SUVDA_INTERFACE_GUID);
		if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
			if (retryInterval > 320) {
				printf("[SUDOVDA] Open device failed!\n");
				return DRIVER_STATUS::FAILED;
			}
			retryInterval *= 2;
			Sleep(retryInterval);
			continue;
		}

		break;
	}

	if (!CheckProtocolCompatible(SUDOVDA_DRIVER_HANDLE)) {
		printf("[SUDOVDA] SUDOVDA protocol not compatible with driver!\n");
		closeVDisplayDevice();
		return DRIVER_STATUS::VERSION_INCOMPATIBLE;
	}

	return DRIVER_STATUS::OK;
}

bool startPingThread(std::function<void()> failCb) {
	if (isDisplayBrokerClient()) {
		return true;  // the main instance keeps the driver's watchdog fed
	}
	if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return false;
	}

	VIRTUAL_DISPLAY_GET_WATCHDOG_OUT watchdogOut;
	if (GetWatchdogTimeout(SUDOVDA_DRIVER_HANDLE, watchdogOut)) {
		printf("[SUDOVDA] Watchdog: Timeout %d, Countdown %d\n", watchdogOut.Timeout, watchdogOut.Countdown);
	} else {
		printf("[SUDOVDA] Watchdog fetch failed!\n");
		return false;
	}

	if (watchdogOut.Timeout) {
		auto sleepInterval = watchdogOut.Timeout * 1000 / 3;
		std::thread ping_thread([sleepInterval, failCb = std::move(failCb)]{
			uint8_t fail_count = 0;
			for (;;) {
				if (!sleepInterval) return;
				if (!PingDriver(SUDOVDA_DRIVER_HANDLE)) {
					fail_count += 1;
					if (fail_count > 3) {
						failCb();
						return;
					}
				};
				Sleep(sleepInterval);
			}
		});

		ping_thread.detach();
	}

	return true;
}

bool setRenderAdapterByName(const std::wstring& adapterName) {
	if (isDisplayBrokerClient()) {
		return true;  // the main instance's render adapter setting applies
	}
	if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return false;
	}

	Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
	if (!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		return false;
	}

	Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
	DXGI_ADAPTER_DESC desc;
	int i = 0;
	while (SUCCEEDED(factory->EnumAdapters(i, &adapter))) {
		i += 1;

		if (!SUCCEEDED(adapter->GetDesc(&desc))) {
			continue;
		}

		if (std::wstring_view(desc.Description) != adapterName) {
			continue;
		}

		if (SetRenderAdapter(SUDOVDA_DRIVER_HANDLE, desc.AdapterLuid)) {
			return true;
		}
	}

	return false;
}

std::wstring createVirtualDisplay(
	const char* s_client_uid,
	const char* s_client_name,
	uint32_t width,
	uint32_t height,
	uint32_t fps,
	const GUID& guid
) {
	if (isDisplayBrokerClient()) {
		BrokerRequest request {};
		BrokerReply reply {};
		request.op = BROKER_ADD;
		request.guid = guid;
		request.width = width;
		request.height = height;
		request.fps = fps;
		strncpy_s(request.clientUid, s_client_uid, _TRUNCATE);
		strncpy_s(request.clientName, s_client_name, _TRUNCATE);
		request.screen = (uint32_t) g_screenIndex;
		if (!brokerCall(request, reply)) {
			printf("[SUDOVDA] Display broker couldn't add the virtual display.\n");
			return std::wstring();
		}
		wprintf(L"[SUDOVDA] Virtual display added via broker: %ls\n", reply.deviceName);
		return std::wstring(reply.deviceName);
	}

	if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return std::wstring();
	}

	{
		std::lock_guard lock(g_createdMutex);
		auto existing = g_created.find(guidKey(guid));
		if (existing != g_created.end()) {
			wprintf(L"[SUDOVDA] Virtual display already there: %ls\n", existing->second.c_str());
			return existing->second;
		}
	}

	// Windows can take seconds to name a new display (e.g. while it reconfigures after the
	// monitors were switched off): wait up to ~8 s; if it still has no name, remove that
	// display (no orphans) and add it once more
	wchar_t deviceName[CCHDEVICENAME]{};
	bool named = false;
	for (int attempt = 0; attempt < 2 && !named; ++attempt) {
		VIRTUAL_DISPLAY_ADD_OUT output;
		if (!AddVirtualDisplay(SUDOVDA_DRIVER_HANDLE, width, height, fps, guid, s_client_name, s_client_uid, output)) {
			printf("[SUDOVDA] Failed to add virtual display.\n");
			return std::wstring();
		}

		uint32_t retryInterval = 20, waited = 0;
		bool extended = false;
		while (!(named = GetAddedDisplayName(output, deviceName)) && waited < 8000) {
			Sleep(retryInterval);
			waited += retryInterval;
			retryInterval = (std::min)(retryInterval * 2, 500u);

			// A display that stays off gets no name. After the monitors were switched off with a
			// supplied layout, Windows leaves the next new display off: switch everything on
			// (the caller switches the monitors off again afterwards)
			if (!extended && waited >= 1500) {
				extended = true;
				if (!activateTarget(output.AdapterLuid, output.TargetId)) {
					extendAllDisplays();
				}
			}
		}
		if (!named) {
			printf("[SUDOVDA] Cannot get name for newly added virtual display (attempt %d)!\n", attempt + 1);
			RemoveVirtualDisplay(SUDOVDA_DRIVER_HANDLE, guid);
		}
	}
	if (!named) {
		return std::wstring();
	}

	wprintf(L"[SUDOVDA] Virtual display added successfully: %ls\n", deviceName);
	printf("[SUDOVDA] Configuration: W: %d, H: %d, FPS: %d\n", width, height, fps);

	{
		std::lock_guard lock(g_createdMutex);
		g_created[guidKey(guid)] = deviceName;
	}

	return std::wstring(deviceName);
}

bool removeVirtualDisplay(const GUID& guid) {
	if (isDisplayBrokerClient()) {
		BrokerRequest request {};
		BrokerReply reply {};
		request.op = BROKER_REMOVE;
		request.guid = guid;
		return brokerCall(request, reply);
	}

	if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
		return false;
	}

	std::wstring removedName;
	{
		std::lock_guard lock(g_createdMutex);
		auto created = g_created.find(guidKey(guid));
		if (created != g_created.end()) {
			removedName = created->second;
			g_created.erase(created);
		}
	}
	if (!removedName.empty()) {
		std::lock_guard configLock(g_configMutex);  // after, not inside, the other lock
		g_slots.erase(removedName);
	}

	if (RemoveVirtualDisplay(SUDOVDA_DRIVER_HANDLE, guid)) {
		printf("[SUDOVDA] Virtual display removed successfully.\n");
		return true;
	} else {
		return false;
	}
}

// START ISOLATED DISPLAY METHODS
// Shows the coordinates/height/width for the displays in the vector structure
std::string printAllDisplays(std::vector< struct positionwidthheight*> displays) {
	int iIndex;
	std::string sOutput;

	for (iIndex = 0; iIndex < displays.size(); iIndex++)
	{
		sOutput += "Index: ";
		sOutput += std::to_string(iIndex);
		sOutput += ", X : ";
		sOutput += std::to_string(displays[iIndex]->position.x);
		sOutput += ", Y : ";
		sOutput += std::to_string(displays[iIndex]->position.y);
		sOutput += ", width : ";
		sOutput += std::to_string(displays[iIndex]->width);
		sOutput += ", height : ";
		sOutput += std::to_string(displays[iIndex]->height);
		sOutput += "\n";

	}
	return sOutput;
}

// Helper method for the rearrangeVirtualDisplayForLowerRight() method to move the unknown unconnected display to be connected to the
// second display which is assumed to be already connected
//
// It will return the move that the unknown display would need to perform
std::vector < struct coordinates > moveToBeConnected(std::vector < struct coordinates > unknown, std::vector< struct coordinates> connected) {
	// Figure out if the boxes are connected
	// Assume that there are 4 points
	int iIndex, iIndex2;

	std::vector< struct coordinatesdifferences > differences;

	std::vector< struct coordinatesdifferences > vertical;
	std::vector< struct coordinatesdifferences > horizontal;

	std::vector < struct coordinates >moveResult;

	std::vector < struct coordinates > unknown2;

	struct coordinatesdifferences sTemp1;
	struct coordinates sNoMove;

	sNoMove.x = 0;
	sNoMove.y = 0;

	struct coordinates sDoMove;
	sDoMove.x = 0;
	sDoMove.y = 0;

	bool bCornerConnect = false;
	bool bVerticalConnect = false;
	bool bHorizontalConnect = false;

	int iCountLess;
	int iCountGreater;

	// Subtract all of the points
	for (iIndex = 0; iIndex < connected.size(); iIndex += 1) {
		for (iIndex2 = 0; iIndex2 < unknown.size(); iIndex2 += 1) {
			sTemp1.left.x = connected[iIndex].x;
			sTemp1.left.y = connected[iIndex].y;
			sTemp1.right.x = unknown[iIndex2].x;
			sTemp1.right.y = unknown[iIndex2].y;

			sTemp1.Difference.x = sTemp1.left.x - sTemp1.right.x;
			sTemp1.Difference.y = sTemp1.left.y - sTemp1.right.y;

			sTemp1.AbsDifference.x = abs(sTemp1.Difference.x);
			sTemp1.AbsDifference.y = abs(sTemp1.Difference.y);

			differences.push_back(sTemp1);
		}
	}

	for (iIndex = 0; iIndex < differences.size(); iIndex += 1) {

		// See if they are any corner connects
		sTemp1 = differences[iIndex];
		if (sTemp1.AbsDifference.x <= 1 && sTemp1.AbsDifference.y <= 1) {
			bCornerConnect = true;
			break;
		}

		// See if there are any vertical connects
		if (sTemp1.AbsDifference.x <= 1) {
			vertical.push_back(sTemp1);
		}

		// See if there are any horizontal connects
		if (sTemp1.AbsDifference.y <= 1) {
			horizontal.push_back(sTemp1);
		}
	}

	// Check the vertical connects
	iCountLess = 0;
	iCountGreater = 0;
	for (iIndex = 0; iIndex < vertical.size(); iIndex += 1) {
		if (vertical[iIndex].left.y <= vertical[iIndex].right.y) {
			iCountLess += 1;
		}
		if (vertical[iIndex].left.y >= vertical[iIndex].right.y) {
			iCountGreater += 1;
		}
	}

	// Check the sum off all of the counts
	if (((iCountLess > 0) && (iCountGreater == 0)) ||
		((iCountGreater > 0) && (iCountLess == 0)) ||
		(iCountLess == 0 && iCountGreater == 0)) {
		// Boxes are on the same vertical but above or below each other
		bVerticalConnect = false;
	} else {
		bVerticalConnect = true;
	}

	// Check the horizontal connects
	iCountLess = 0;
	iCountGreater = 0;
	for (iIndex = 0; iIndex < horizontal.size(); iIndex += 1) {
		if (horizontal[iIndex].left.x <= horizontal[iIndex].right.x) {
			iCountLess += 1;
		}
		if (horizontal[iIndex].left.x >= horizontal[iIndex].right.x) {
			iCountGreater += 1;
		}
	}
	// Check the sum off all of the counts
	if (((iCountLess > 0) && (iCountGreater == 0)) ||
		((iCountGreater > 0) && (iCountLess == 0)) ||
		(iCountLess == 0 && iCountGreater == 0)) {
		// Boxes are on the same horizontal but to the left or right of each other
		bHorizontalConnect = false;
	} else {
		bHorizontalConnect = true;
	}

	// End the logic if there is no move required
	if (bHorizontalConnect == true ||
		bVerticalConnect == true ||
		bCornerConnect == true) {
		moveResult.push_back(sNoMove);
		return moveResult;
	}

	// Otherwise, show the move required
	int iShortestX = INT_MAX;
	int iShortestXIndex = -1;

	// Try the horizontal (x) move first
	for (iIndex = 0; iIndex < differences.size(); iIndex += 1) {
		if (differences[iIndex].AbsDifference.x < iShortestX) {
			iShortestXIndex = iIndex;
			iShortestX = differences[iIndex].AbsDifference.x;
		}
	}

	if (iShortestX <= 1) {
		// X move is not required
	} else {
		// This is the X to move
		sDoMove.x = differences[iShortestXIndex].Difference.x;

		// Perform the x move on the left so that we can check the y
		unknown2 = unknown;
		for (iIndex = 0; iIndex < unknown2.size(); iIndex += 1) {
			unknown2[iIndex].x += sDoMove.x;
		}

		// Call oneself recursively only once so that we can see if there is Y to do.
		std::vector < struct coordinates >moveResult2;
		moveResult2 = moveToBeConnected(unknown2, connected);

		// Format the answer for a return
		sDoMove.y = moveResult2[0].y;

		moveResult.push_back(sDoMove);
		return moveResult;
	}

	// Figure out the y move required
	// Otherwise, show the move required
	int iShortestY = INT_MAX;
	int iShortestYIndex = -1;

	// Try the horizontal (x) move first
	for (iIndex = 0; iIndex < differences.size(); iIndex += 1) {
		if (differences[iIndex].AbsDifference.y < iShortestY) {
			iShortestYIndex = iIndex;
			iShortestY = differences[iIndex].AbsDifference.y;
		}
	}

	if (iShortestY <= 1) {
		// Y move is not required
	} else {
		// This is the Y to move
		sDoMove.y = differences[iShortestYIndex].Difference.y;
		moveResult.push_back(sDoMove);
		return moveResult;
	}
	moveResult.push_back(sNoMove);
	return moveResult;
}

// Main method to rearrange the displays to have one isolated display in the lower right and
// move the other displays as necessary especially if there are holes
std::vector< struct positionwidthheight*>rearrangeVirtualDisplayForLowerRight(std::vector< struct positionwidthheight*> displays) {

	// Make a temporary connected List based on the current Displays
	// Here connected means that the displays are "touching" by either the
	// vertical axis or a horizontal axis or a corner.
	int count = displays.size();
	std::vector< int > vConnected(count, 0);

	// Need the index of the virtual display to put into the lower right corner as primary
	int changeIndex = 0;

	// Find the Maxx and Maxy for the current displays
	int imaxx = INT_MIN;
	int imaxy = INT_MIN;
	int imaxindex = -1;

	int itempx;
	int itempy;
	int itempvalid = 0;

	// Figure out the maxx and maxy, and the index for that rectangle
	for (int index = 0; index < count; index++) {
		itempx = displays[index]->position.x + displays[index]->width;
		itempy = displays[index]->position.y + displays[index]->height;
		itempvalid = 1;
		if (changeIndex == index) {
			itempvalid = 0;
		}
		if (itempvalid > 0) {
			if (imaxx < itempx) {
				imaxx = itempx;
				imaxy = itempy;
				imaxindex = index;
			} else if (imaxx == itempx) {
				if (imaxy < itempy) {
					imaxy = itempy;
					imaxindex = index;
				}
			}
		}
	}


	// Adjust all of the other windows based on the offset for the display that will be 0,0 in the lower right corner.
	if (imaxindex > -1) {
		// Adjusting other displays based on the offset for the display that will be 0,0 in the lower right corner
		for (int index = 0; index < count; index++) {
			itempvalid = 1;
			if (changeIndex == index) {
				itempvalid = 0;
			}
			if (itempvalid > 0) {
				displays[index]->position.x -= imaxx;
				displays[index]->position.y -= imaxy;
			}
		}
	}

	// Get the location, width and height of the window that is moving
	// Make sure the correct display is set to 0,0.
	for (int index = 0; index < count; index++) {
			if (index == changeIndex) {
				displays[index]->position.x = 0;
				displays[index]->position.y = 0;
				vConnected[index] = 1;
			}
	}

	bool bAddedConnected;
	int connectedboxx, connectedboxy, connectedboxwidth, connectedboxheight;
	int secondboxx, secondboxy, secondboxwidth, secondboxheight, secondboxindex;

	bool bFirstTime = true;
	int xmin;
	int ymin;
	int minindexconnected;
	int minindexnonconnected;

	std::vector< struct coordinates> connectedboxpoints;
	std::vector< struct coordinates> secondboxpoints;
	struct coordinates sTempCoordinates;

	// MAIN LOOP to rearrange displays to be connected to each other.
	// This is either corner to corner or vertical side or horizontal side
	do {
		xmin = INT_MAX;
		ymin = INT_MAX;
		minindexconnected = -1;
		minindexnonconnected = -1;

		do {
			bAddedConnected = false;

			for (int index = 0; index < count; index++) {
				if (vConnected[index] == 1) {
					// Skip the virtual window if this is not the first time because we do not want an displays connected to it
					if (bFirstTime == false && index == changeIndex) {
						continue;
					}

					connectedboxx = displays[index]->position.x;
					connectedboxy = displays[index]->position.y;
					connectedboxwidth = displays[index]->width;
					connectedboxheight = displays[index]->height;

					connectedboxpoints.clear();

					sTempCoordinates.x = connectedboxx;
					sTempCoordinates.y = connectedboxy;
					connectedboxpoints.push_back(sTempCoordinates);

					sTempCoordinates.x = connectedboxx + connectedboxwidth;
					sTempCoordinates.y = connectedboxy;
					connectedboxpoints.push_back(sTempCoordinates);

					sTempCoordinates.x = connectedboxx;
					sTempCoordinates.y = connectedboxy + connectedboxheight;
					connectedboxpoints.push_back(sTempCoordinates);

					sTempCoordinates.x = connectedboxx + connectedboxwidth;
					sTempCoordinates.y = connectedboxy + connectedboxheight;
					connectedboxpoints.push_back(sTempCoordinates);

					// Go through all other boxes and see if there is a connected box to this one
					for (int index2 = 0; index2 < count; index2++) {
						if (index2 == index || vConnected[index2] == 1 || index2 == changeIndex) {
							// Skip oneself and the skip boxes already connected and skip over changeIndex
							continue;
						}
						secondboxx = displays[index2]->position.x;
						secondboxy = displays[index2]->position.y;
						secondboxwidth = displays[index2]->width;
						secondboxheight = displays[index2]->height;
						secondboxindex = index2;

						secondboxpoints.clear();

						sTempCoordinates.x = secondboxx;
						sTempCoordinates.y = secondboxy;
						secondboxpoints.push_back(sTempCoordinates);

						sTempCoordinates.x = secondboxx + secondboxwidth;
						sTempCoordinates.y = secondboxy;
						secondboxpoints.push_back(sTempCoordinates);

						sTempCoordinates.x = secondboxx;
						sTempCoordinates.y = secondboxy + secondboxheight;
						secondboxpoints.push_back(sTempCoordinates);

						sTempCoordinates.x = secondboxx + secondboxwidth;
						sTempCoordinates.y = secondboxy + secondboxheight;
						secondboxpoints.push_back(sTempCoordinates);

						// What would it take to MOVE the display to be connected to another connected display
						// The result of this may not be used as there may be a closer display when we go through the list
						std::vector < struct coordinates > sToMove = moveToBeConnected(secondboxpoints, connectedboxpoints);

						// No movement necessary
						if (sToMove[0].x == 0 && sToMove[0].y == 0) {
							vConnected[secondboxindex] = 1;

							// NEWLY ADDED
							bFirstTime = false;

							bAddedConnected = true;
							xmin = INT_MAX;
							ymin = INT_MAX;

							// Need to restart the whole loop sequence to not connect more than one at the same time.
							break;
						} else {
							if (index != changeIndex) {
								// Want to see if this display would be the closest one to move via the x coordinates
								if (abs(sToMove[0].x) < xmin) {
									xmin = abs(sToMove[0].x);
									ymin = abs(sToMove[0].y);
									minindexconnected = index;
									minindexnonconnected = index2;
								}
							}
						}
					}
				}

				// Need to restart the whole loop sequence to not connect more than one at the same time.
				if (bAddedConnected == true) {
					break;
				}
			}
		} while (bAddedConnected == true);

		// We are finish adding the connected box during the initial pass throguh
		// We should also have the minimal display to move
		bFirstTime = false;

		if (xmin != INT_MAX || ymin != INT_MAX) {
			connectedboxx = displays[minindexconnected]->position.x;
			connectedboxy = displays[minindexconnected]->position.y;
			connectedboxwidth = displays[minindexconnected]->width;
			connectedboxheight = displays[minindexconnected]->height;

			connectedboxpoints.clear();

			sTempCoordinates.x = connectedboxx;
			sTempCoordinates.y = connectedboxy;
			connectedboxpoints.push_back(sTempCoordinates);

			sTempCoordinates.x = connectedboxx + connectedboxwidth;
			sTempCoordinates.y = connectedboxy;
			connectedboxpoints.push_back(sTempCoordinates);

			sTempCoordinates.x = connectedboxx;
			sTempCoordinates.y = connectedboxy + connectedboxheight;
			connectedboxpoints.push_back(sTempCoordinates);

			sTempCoordinates.x = connectedboxx + connectedboxwidth;
			sTempCoordinates.y = connectedboxy + connectedboxheight;
			connectedboxpoints.push_back(sTempCoordinates);



			secondboxx = displays[minindexnonconnected]->position.x;
			secondboxy = displays[minindexnonconnected]->position.y;
			secondboxwidth = displays[minindexnonconnected]->width;
			secondboxheight = displays[minindexnonconnected]->height;
			secondboxindex = minindexnonconnected;

			secondboxpoints.clear();

			sTempCoordinates.x = secondboxx;
			sTempCoordinates.y = secondboxy;
			secondboxpoints.push_back(sTempCoordinates);

			sTempCoordinates.x = secondboxx + secondboxwidth;
			sTempCoordinates.y = secondboxy;
			secondboxpoints.push_back(sTempCoordinates);

			sTempCoordinates.x = secondboxx;
			sTempCoordinates.y = secondboxy + secondboxheight;
			secondboxpoints.push_back(sTempCoordinates);

			sTempCoordinates.x = secondboxx + secondboxwidth;
			sTempCoordinates.y = secondboxy + secondboxheight;
			secondboxpoints.push_back(sTempCoordinates);

			// Perform the actual move
			std::vector < struct coordinates > sToMove = moveToBeConnected(secondboxpoints, connectedboxpoints);

			// Apply the move to the array of displays
			displays[minindexnonconnected]->position.x += sToMove[0].x;
			displays[minindexnonconnected]->position.y += sToMove[0].y;
		}
	} while (xmin != INT_MAX);

	return displays;
}

// Utility function to match the DeviceString to the Display Names
// Typical DeviceStrings are the driver names
//
// Example: matchDisplay(L"SudoMaker Virtual Display Adapter")
// Result: L"\\\\.\\Display2"

std::vector <std::wstring> matchDisplay(std::wstring sMatch) {
	DISPLAY_DEVICEW displayDevice;
	displayDevice.cb = sizeof(DISPLAY_DEVICE);

	std::wstring matchDeviceName;

	std::vector <std::wstring>vMatches;

	int deviceIndex = 0;
	while (EnumDisplayDevicesW(NULL, deviceIndex, &displayDevice, 0)) {
		if (std::wstring(displayDevice.DeviceString) == sMatch &&
			displayDevice.StateFlags > 0) {
			matchDeviceName = displayDevice.DeviceName;
			vMatches.push_back(matchDeviceName);
		}
		deviceIndex++;
	}
	return vMatches;
}

// END ISOLATED DISPLAY METHODS
}