#include "WebWindow.h"
#include <stdio.h>
#include <map>
#include <mutex>
#include <condition_variable>
#include <comdef.h>
#include <atomic>
#include <Shlwapi.h>
#include <Dwmapi.h>
#include <Windows.h>
#include <Windowsx.h>
#define WM_USER_SHOWMESSAGE (WM_USER + 0x0001)
#define WM_USER_INVOKE (WM_USER + 0x0002)

using namespace Microsoft::WRL;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
LPCWSTR CLASS_NAME = L"WebWindow";
std::mutex invokeLockMutex;
HINSTANCE WebWindow::_hInstance;
HWND messageLoopRootWindowHandle;
std::map<HWND, WebWindow*> hwndToWebWindow;

struct InvokeWaitInfo
{
	std::condition_variable completionNotifier;
	bool isCompleted;
};

struct ShowMessageParams
{
	std::wstring title;
	std::wstring body;
	UINT type;
};

// we cannot just use WS_POPUP style
// WS_THICKFRAME: without this the window cannot be resized and so aero snap, de-maximizing and minimizing won't work
// WS_SYSMENU: enables the context menu with the move, close, maximize, minize... commands (shift + right-click on the task bar item)
// WS_CAPTION: enables aero minimize animation/transition
// WS_MAXIMIZEBOX, WS_MINIMIZEBOX: enable minimize/maximize
enum class Style : DWORD {
	windowed = WS_OVERLAPPEDWINDOW | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
	aero_borderless = WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MAXIMIZEBOX | WS_MINIMIZEBOX,
	basic_borderless = WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MAXIMIZEBOX | WS_MINIMIZEBOX
};

auto maximized(HWND hwnd) -> bool {
	WINDOWPLACEMENT placement;
	if (!::GetWindowPlacement(hwnd, &placement)) {
		return false;
	}

	return placement.showCmd == SW_MAXIMIZE;
}

/* Adjust client rect to not spill over monitor edges when maximized.
* rect(in/out): in: proposed window rect, out: calculated client rect
* Does nothing if the window is not maximized.
*/
auto adjust_maximized_client_rect(HWND window, RECT& rect) -> void {
	if (!maximized(window)) {
		return;
	}

	auto monitor = ::MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
	if (!monitor) {
		return;
	}

	MONITORINFO monitor_info{};
	monitor_info.cbSize = sizeof(monitor_info);
	if (!::GetMonitorInfoW(monitor, &monitor_info)) {
		return;
	}

	// when maximized, make the client area fill just the monitor (without task bar) rect,
	// not the whole window rect which extends beyond the monitor.
	rect = monitor_info.rcWork;
}

auto composition_enabled() -> bool {
	BOOL composition_enabled = FALSE;
	bool success = ::DwmIsCompositionEnabled(&composition_enabled) == S_OK;
	return composition_enabled && success;
}

auto select_borderless_style() -> Style {
	return composition_enabled() ? Style::aero_borderless : Style::basic_borderless;
}

auto set_shadow(HWND handle, bool enabled) -> void {
	if (composition_enabled()) {
		static const MARGINS shadow_state[2]{ { 0,0,0,0 },{ 1,1,1,1 } };
		::DwmExtendFrameIntoClientArea(handle, &shadow_state[enabled]);
	}
}

auto hit_test(HWND handle, POINT cursor, bool moveable, bool resizable) -> LRESULT {
	// identify borders and corners to allow resizing the window.
	// Note: On Windows 10, windows behave differently and
	// allow resizing outside the visible window frame.
	// This implementation does not replicate that behavior.
	const POINT border{
		::GetSystemMetrics(SM_CXFRAME) + ::GetSystemMetrics(SM_CXPADDEDBORDER),
		::GetSystemMetrics(SM_CYFRAME) + ::GetSystemMetrics(SM_CXPADDEDBORDER)
	};
	RECT window;
	if (!::GetWindowRect(handle, &window)) {
		return HTNOWHERE;
	}

	const auto drag = moveable ? HTCAPTION : HTCLIENT;

	enum region_mask {
		client = 0b0000,
		left = 0b0001,
		right = 0b0010,
		top = 0b0100,
		bottom = 0b1000,
	};

	const auto result =
		left * (cursor.x < (window.left + border.x)) |
		right * (cursor.x >= (window.right - border.x)) |
		top * (cursor.y < (window.top + border.y)) |
		bottom * (cursor.y >= (window.bottom - border.y));

	switch (result) {
	case left: return resizable ? HTLEFT : drag;
	case right: return resizable ? HTRIGHT : drag;
	case top: return resizable ? HTTOP : drag;
	case bottom: return resizable ? HTBOTTOM : drag;
	case top | left: return resizable ? HTTOPLEFT : drag;
	case top | right: return resizable ? HTTOPRIGHT : drag;
	case bottom | left: return resizable ? HTBOTTOMLEFT : drag;
	case bottom | right: return resizable ? HTBOTTOMRIGHT : drag;
	case client: return drag;
	default: return HTNOWHERE;
	}
}

void WebWindow::Register(HINSTANCE hInstance)
{
	_hInstance = hInstance;

	// Register the window class	
	WNDCLASSW wc = { };
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = CLASS_NAME;
	RegisterClass(&wc);

	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
}

WebWindow::WebWindow(AutoString title, WebWindow* parent, WebMessageReceivedCallback webMessageReceivedCallback)
{
	// Create the window
	_webMessageReceivedCallback = webMessageReceivedCallback;
	_parent = parent;
	_hWnd = CreateWindowEx(
		0,                              // Optional window styles.
		CLASS_NAME,                     // Window class
		title,							// Window text
		WS_OVERLAPPEDWINDOW,            // Window style

		// Size and position
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,

		parent ? parent->_hWnd : NULL,       // Parent window
		NULL,       // Menu
		_hInstance, // Instance handle
		this        // Additional application data
	);
	hwndToWebWindow[_hWnd] = this;
	SetFrameless(false);
	SetMoveable(true);
	SetResizable(true);
}

// Needn't to release the handles.
WebWindow::~WebWindow() {}

HWND WebWindow::getHwnd()
{
	return _hWnd;
}

void my_paint(HDC hdc, RECT rc)
{
	HBRUSH brush = CreateSolidBrush(RGB(0, 128, 0));
	FillRect(hdc, &rc, brush);
	DeleteObject(brush);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_DESTROY:
	{
		// Only terminate the message loop if the window being closed is the one that
		// started the message loop
		hwndToWebWindow.erase(hwnd);
		if (hwnd == messageLoopRootWindowHandle)
		{
			PostQuitMessage(0);
		}
		return 0;
	}
	case WM_USER_SHOWMESSAGE:
	{
		ShowMessageParams* params = (ShowMessageParams*)wParam;
		MessageBox(hwnd, params->body.c_str(), params->title.c_str(), params->type);
		delete params;
		return 0;
	}

	case WM_USER_INVOKE:
	{
		ACTION callback = (ACTION)wParam;
		callback();
		InvokeWaitInfo* waitInfo = (InvokeWaitInfo*)lParam;
		{
			std::lock_guard<std::mutex> guard(invokeLockMutex);
			waitInfo->isCompleted = true;
		}
		waitInfo->completionNotifier.notify_one();
		return 0;
	}
	case WM_SIZE:
	{
		WebWindow* webWindow = hwndToWebWindow[hwnd];
		if (webWindow)
		{
			webWindow->RefitContent();
			int width, height;
			webWindow->GetSize(&width, &height);
			webWindow->InvokeResized(width, height);
		}
		return 0;
	}
	case WM_MOVE:
	{
		WebWindow* webWindow = hwndToWebWindow[hwnd];
		if (webWindow)
		{
			int x, y;
			webWindow->GetPosition(&x, &y);
			webWindow->InvokeMoved(x, y);
		}
		return 0;
	}
	case WM_NCCALCSIZE:
	{
		WebWindow* webWindow = hwndToWebWindow[hwnd];
		if (webWindow) {
			if (wParam == TRUE && webWindow->GetFrameless()) {
				auto& params = *reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
				adjust_maximized_client_rect(hwnd, params.rgrc[0]);
				return 0;
			}
		}
		return 0;
	}

	case WM_NCHITTEST:
	{
		// When we have no border or title bar, we need to perform our
		// own hit testing to allow resizing and moving.
		WebWindow* webWindow = hwndToWebWindow[hwnd];
		if (webWindow) {
			if (webWindow->GetFrameless()) {
				return hit_test(hwnd, POINT{
					GET_X_LPARAM(lParam),
					GET_Y_LPARAM(lParam) },
					webWindow->GetMoveable(),
					webWindow->GetResizable());
			}
		}
		return 0;
	}
	case WM_NCACTIVATE:
	{
		if (!composition_enabled()) {
			// Prevents window frame reappearing on window activation
			// in "basic" theme, where no aero shadow is present.
			return 1;
		}
		return 0;
	}
	break;
	}

	return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void WebWindow::RefitContent()
{
	if (_webviewWindow)
	{
		RECT bounds;
		GetClientRect(_hWnd, &bounds);
		_webviewWindow->put_Bounds(bounds);
	}
}

void WebWindow::SetTitle(AutoString title)
{
	SetWindowText(_hWnd, title);
}

void WebWindow::Show()
{
	ShowWindow(_hWnd, SW_SHOWDEFAULT);

	// Strangely, it only works to create the webview2 *after* the window has been shown,
	// so defer it until here. This unfortunately means you can't call the Navigate methods
	// until the window is shown.
	if (!_webviewWindow)
	{
		AttachWebView();
	}
}

void WebWindow::WaitForExit()
{
	messageLoopRootWindowHandle = _hWnd;

	// Run the message loop
	MSG msg = { };
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
}

void WebWindow::ShowMessage(AutoString title, AutoString body, UINT type)
{
	ShowMessageParams* params = new ShowMessageParams;
	params->title = title;
	params->body = body;
	params->type = type;
	PostMessage(_hWnd, WM_USER_SHOWMESSAGE, (WPARAM)params, 0);
}

void WebWindow::Invoke(ACTION callback)
{
	InvokeWaitInfo waitInfo = {};
	PostMessage(_hWnd, WM_USER_INVOKE, (WPARAM)callback, (LPARAM)&waitInfo);

	// Block until the callback is actually executed and completed
	// TODO: Add return values, exception handling, etc.
	std::unique_lock<std::mutex> uLock(invokeLockMutex);
	waitInfo.completionNotifier.wait(uLock, [&] { return waitInfo.isCompleted; });
}

void WebWindow::AttachWebView()
{
	std::atomic_flag flag = ATOMIC_FLAG_INIT;
	flag.test_and_set();

	HRESULT envResult = CreateWebView2EnvironmentWithDetails(nullptr, nullptr, nullptr,
		Callback<IWebView2CreateWebView2EnvironmentCompletedHandler>(
			[&, this](HRESULT result, IWebView2Environment* env) -> HRESULT {
				HRESULT envResult = env->QueryInterface(&_webviewEnvironment);
				if (envResult != S_OK)
				{
					return envResult;
				}

				// Create a WebView, whose parent is the main window hWnd
				env->CreateWebView(_hWnd, Callback<IWebView2CreateWebViewCompletedHandler>(
					[&, this](HRESULT result, IWebView2WebView* webview) -> HRESULT {
						if (result != S_OK) { return result; }
						result = webview->QueryInterface(&_webviewWindow);
						if (result != S_OK) { return result; }

						// Add a few settings for the webview
						// this is a redundant demo step as they are the default settings values
						IWebView2Settings* Settings;
						_webviewWindow->get_Settings(&Settings);
						Settings->put_IsScriptEnabled(TRUE);
						Settings->put_AreDefaultScriptDialogsEnabled(TRUE);
						Settings->put_IsWebMessageEnabled(TRUE);

						// Register interop APIs
						EventRegistrationToken webMessageToken;
						_webviewWindow->AddScriptToExecuteOnDocumentCreated(L"window.external = { sendMessage: function(message) { window.chrome.webview.postMessage(message); }, receiveMessage: function(callback) { window.chrome.webview.addEventListener(\'message\', function(e) { callback(e.data); }); } };", nullptr);
						_webviewWindow->add_WebMessageReceived(Callback<IWebView2WebMessageReceivedEventHandler>(
							[this](IWebView2WebView* webview, IWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
								wil::unique_cotaskmem_string message;
								args->get_WebMessageAsString(&message);
								_webMessageReceivedCallback(message.get());
								return S_OK;
							}).Get(), &webMessageToken);

						EventRegistrationToken webResourceRequestedToken;
						_webviewWindow->AddWebResourceRequestedFilter(L"*", WEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
						_webviewWindow->add_WebResourceRequested(Callback<IWebView2WebResourceRequestedEventHandler>(
							[this](IWebView2WebView* sender, IWebView2WebResourceRequestedEventArgs* args)
							{
								IWebView2WebResourceRequest* req;
								args->get_Request(&req);

								wil::unique_cotaskmem_string uri;
								req->get_Uri(&uri);
								std::wstring uriString = uri.get();
								size_t colonPos = uriString.find(L':', 0);
								if (colonPos > 0)
								{
									std::wstring scheme = uriString.substr(0, colonPos);
									WebResourceRequestedCallback handler = _schemeToRequestHandler[scheme];
									if (handler != NULL)
									{
										int numBytes;
										AutoString contentType;
										wil::unique_cotaskmem dotNetResponse(handler(uriString.c_str(), &numBytes, &contentType));

										if (dotNetResponse != nullptr && contentType != nullptr)
										{
											std::wstring contentTypeWS = contentType;

											IStream* dataStream = SHCreateMemStream((BYTE*)dotNetResponse.get(), numBytes);
											wil::com_ptr<IWebView2WebResourceResponse> response;
											_webviewEnvironment->CreateWebResourceResponse(
												dataStream, 200, L"OK", (L"Content-Type: " + contentTypeWS).c_str(),
												&response);
											args->put_Response(response.get());
										}
									}
								}

								return S_OK;
							}
						).Get(), &webResourceRequestedToken);

						RefitContent();

						flag.clear();
						return S_OK;
					}).Get());
				return S_OK;
			}).Get());

	if (envResult != S_OK)
	{
		_com_error err(envResult);
		LPCTSTR errMsg = err.ErrorMessage();
		MessageBox(_hWnd, errMsg, L"Error instantiating webview", MB_OK);
	}
	else
	{
		// Block until it's ready. This simplifies things for the caller, so they
		// don't need to regard this process as async.
		MSG msg = { };
		while (flag.test_and_set() && GetMessage(&msg, NULL, 0, 0))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}
}

void WebWindow::NavigateToUrl(AutoString url)
{
	_webviewWindow->Navigate(url);
}

void WebWindow::NavigateToString(AutoString content)
{
	_webviewWindow->NavigateToString(content);
}

void WebWindow::SendMessage(AutoString message)
{
	_webviewWindow->PostWebMessageAsString(message);
}

void WebWindow::AddCustomScheme(AutoString scheme, WebResourceRequestedCallback requestHandler)
{
	_schemeToRequestHandler[scheme] = requestHandler;
}

void WebWindow::GetSize(int* width, int* height)
{
	RECT rect = {};
	GetWindowRect(_hWnd, &rect);
	if (width) *width = rect.right - rect.left;
	if (height) *height = rect.bottom - rect.top;
}

void WebWindow::SetSize(int width, int height)
{
	SetWindowPos(_hWnd, HWND_TOP, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER);
}

BOOL MonitorEnum(HMONITOR monitor, HDC, LPRECT, LPARAM arg)
{
	auto callback = (GetAllMonitorsCallback)arg;
	MONITORINFO info = {};
	info.cbSize = sizeof(MONITORINFO);
	GetMonitorInfo(monitor, &info);
	Monitor props = {};
	props.monitor.x = info.rcMonitor.left;
	props.monitor.y = info.rcMonitor.top;
	props.monitor.width = info.rcMonitor.right - info.rcMonitor.left;
	props.monitor.height = info.rcMonitor.bottom - info.rcMonitor.top;
	props.work.x = info.rcWork.left;
	props.work.y = info.rcWork.top;
	props.work.width = info.rcWork.right - info.rcWork.left;
	props.work.height = info.rcWork.bottom - info.rcWork.top;
	return callback(&props) ? TRUE : FALSE;
}

void WebWindow::GetAllMonitors(GetAllMonitorsCallback callback)
{
	if (callback)
	{
		EnumDisplayMonitors(NULL, NULL, MonitorEnum, (LPARAM)callback);
	}
}

unsigned int WebWindow::GetScreenDpi()
{
	return GetDpiForWindow(_hWnd);
}

void WebWindow::GetPosition(int* x, int* y)
{
	RECT rect = {};
	GetWindowRect(_hWnd, &rect);
	if (x) *x = rect.left;
	if (y) *y = rect.top;
}

void WebWindow::SetPosition(int x, int y)
{
	SetWindowPos(_hWnd, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

void WebWindow::SetTopmost(bool topmost)
{
	SetWindowPos(_hWnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

bool WebWindow::GetFrameless()
{
	return _frameless;
}

void WebWindow::SetFrameless(bool frameless)
{
	Style new_style = (frameless) ? select_borderless_style() : Style::windowed;
	Style old_style = static_cast<Style>(::GetWindowLongPtrW(_hWnd, GWL_STYLE));

	if (new_style != old_style) {
		_frameless = frameless;

		::SetWindowLongPtrW(_hWnd, GWL_STYLE, static_cast<LONG>(new_style));

		// when switching between borderless and windowed, restore appropriate shadow state
		set_shadow(_hWnd, new_style != Style::windowed);

		// redraw frame
		::SetWindowPos(_hWnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE);
		::ShowWindow(_hWnd, SW_SHOW);
	}
}

bool WebWindow::GetMoveable()
{
	return _moveable;
}

void WebWindow::SetMoveable(bool moveable)
{
	_moveable = moveable;
}

bool WebWindow::GetResizable()
{
	return _resizable;
}

void WebWindow::SetResizable(bool resizable)
{
	_resizable = resizable;
	/*LONG_PTR style = GetWindowLongPtr(_hWnd, GWL_STYLE);
	if (resizable) style |= WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
	else style &= (~WS_THICKFRAME) & (~WS_MINIMIZEBOX) & (~WS_MAXIMIZEBOX);
	SetWindowLongPtr(_hWnd, GWL_STYLE, style);*/
}

void WebWindow::SetIconFile(AutoString filename)
{
	HICON icon = (HICON)LoadImage(NULL, filename, IMAGE_ICON, 0, 0, LR_LOADFROMFILE);
	if (icon)
	{
		::SendMessage(_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
	}
}
