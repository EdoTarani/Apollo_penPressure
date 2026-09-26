#pragma once

#include <functional>
#include <vector>

#ifndef FILE_DEVICE_UNKNOWN
#define FILE_DEVICE_UNKNOWN 0x00000022
#endif

#include <ddk/d4iface.h>
#include <ddk/d4drvif.h>
#include <sudovda/sudovda.h>

namespace VDISPLAY {
	enum class DRIVER_STATUS {
		UNKNOWN              = 1,
		OK                   = 0,
		FAILED               = -1,
		VERSION_INCOMPATIBLE = -2,
		WATCHDOG_FAILED      = -3
	};

	extern HANDLE SUDOVDA_DRIVER_HANDLE;

	LONG getDeviceSettings(const wchar_t* deviceName, DEVMODEW& devMode);
	LONG changeDisplaySettings(const wchar_t* deviceName, int width, int height, int refresh_rate);
	LONG changeDisplaySettings2(const wchar_t* deviceName, int width, int height, int refresh_rate, bool bApplyIsolated=false);	
	std::wstring getPrimaryDisplay();
	bool setPrimaryDisplay(const wchar_t* primaryDeviceName);
	bool getDisplayHDRByName(const wchar_t* displayName);
	bool setDisplayHDRByName(const wchar_t* displayName, bool enableAdvancedColor);

	void closeVDisplayDevice();
	DRIVER_STATUS openVDisplayDevice();
	bool startPingThread(std::function<void()> failCb);
	bool setRenderAdapterByName(const std::wstring& adapterName);
	std::wstring createVirtualDisplay(
		const char* s_client_uid,
		const char* s_client_name,
		uint32_t width,
		uint32_t height,
		uint32_t fps,
		const GUID& guid
	);
	bool removeVirtualDisplay(const GUID& guid);

	// SudoVDA allows a single open handle: with extra screens the main instance serves
	// add/remove requests on a local pipe (startDisplayBroker) and the extra screens' processes
	// send theirs there (useDisplayBroker, before any other VDISPLAY call).
	// arrange: place each extra screen's display in the row (slot = screen - 1) as it's created
	void startDisplayBroker(const std::wstring& pipeName, bool arrange);
	// screenIndex: which extra screen this process is (2, 3)
	void useDisplayBroker(const std::wstring& pipeName, int screenIndex);

	// Wait until Windows has switched a new display on (attached it to the desktop)
	bool waitForDisplayActive(const wchar_t* deviceName, int timeoutMs);

	// Switch every connected display on, extended
	bool extendAllDisplays();

	// While on, every display the broker adds is followed by switching the physical monitors off
	void setKeepPhysicalOff(bool on);

	// "\\.\DISPLAY5 virtual 2560x1440 at 0,0; ..." for the log
	std::wstring describeDisplays();
	bool isDisplayBrokerClient();

	// Place a virtual display in slot `slot` (0, 1, 2) of a row to the right of the other
	// (physical) displays, all at the top edge; slots are the display's own width apart
	bool arrangeInRow(const wchar_t* deviceName, int slot);

	// Remove every virtual display this process created (the extra screens' too)
	void removeAllVirtualDisplays();

	// Switch off every display that isn't a SudoVDA virtual display (never all of them);
	// restorePhysicalDisplays() switches them back on in the user's usual layout
	bool keepOnlyVirtualDisplays();
	void restorePhysicalDisplays();

	// At startup: if no physical display is on (e.g. left off by a crash), switch them on
	void ensurePhysicalDisplaysOn();

	std::vector<std::wstring> matchDisplay(std::wstring sMatch);
}
