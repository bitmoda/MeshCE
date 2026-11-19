/*
WIN32_WINDOW.C

Keep borderless OpenGL windows on the desktop. NVIDIA's driver treats
SDL's WS_POPUP fullscreen window as exclusive even without a display mode,
which blanks the monitor when the player switches to another application.
*/

#include <windows.h>
#include <shellapi.h>
#include <SDL3/SDL.h>

void win32_window_borderless_apply(SDL_Window *window)
{
	HWND handle;
	LONG_PTR style;

	if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) || SDL_GetWindowFullscreenMode(window))
		return;
	handle = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
	if (!handle)
		return;
	style = GetWindowLongPtr(handle, GWL_STYLE);
	if (!(style & WS_POPUP) && !(GetWindowLongPtr(handle, GWL_EXSTYLE) & WS_EX_TOPMOST))
		return;
	SetWindowLongPtr(handle, GWL_STYLE, style & ~WS_POPUP);
	SetWindowPos(handle, HWND_NOTOPMOST, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

/* the notification area. Quit from the game menu hides the window and this
icon stays, so the process keeps introducing peers. */
#define WIN32_TRAY_MESSAGE (WM_APP + 41)
#define WIN32_TRAY_SHOW 1
#define WIN32_TRAY_QUIT 2

static HWND tray_window;
static int tray_added;
static int tray_command;

static void tray_menu(HWND window)
{
	HMENU menu;
	POINT point;
	HWND foreground;
	DWORD foreground_thread;
	DWORD our_thread;
	int choice;

	menu = CreatePopupMenu();
	if (!menu)
		return;
	AppendMenuW(menu, MF_STRING, WIN32_TRAY_SHOW, L"Show MeshCE");
	AppendMenuW(menu, MF_STRING, WIN32_TRAY_QUIT, L"Quit MeshCE");
	GetCursorPos(&point);
	foreground = GetForegroundWindow();
	foreground_thread = foreground ? GetWindowThreadProcessId(foreground, NULL) : 0;
	our_thread = GetCurrentThreadId();
	if (foreground_thread && foreground_thread != our_thread)
		AttachThreadInput(foreground_thread, our_thread, TRUE);
	SetForegroundWindow(window);
	choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
		point.x, point.y, 0, window, NULL);
	if (foreground_thread && foreground_thread != our_thread)
		AttachThreadInput(foreground_thread, our_thread, FALSE);
	PostMessageW(window, WM_NULL, 0, 0);
	DestroyMenu(menu);
	if (choice == WIN32_TRAY_SHOW || choice == WIN32_TRAY_QUIT)
		tray_command = choice;
}

static LRESULT CALLBACK tray_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WIN32_TRAY_MESSAGE)
	{
		if (lparam == WM_LBUTTONUP || lparam == WM_LBUTTONDBLCLK)
			tray_command = WIN32_TRAY_SHOW;
		else if (lparam == WM_RBUTTONUP)
			tray_menu(window);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

/* a hidden game window can stop receiving the icon's clicks, so the icon
belongs to a window that has no screen presence of its own */
static HWND tray_message_window(void)
{
	WNDCLASSW window_class;
	HINSTANCE instance;

	if (tray_window)
		return tray_window;
	instance = GetModuleHandleW(NULL);
	ZeroMemory(&window_class, sizeof(window_class));
	window_class.lpfnWndProc = tray_proc;
	window_class.hInstance = instance;
	window_class.lpszClassName = L"MeshCETray";
	if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		return NULL;
	tray_window = CreateWindowExW(0, L"MeshCETray", L"MeshCE", 0, 0, 0, 0, 0,
		HWND_MESSAGE, NULL, instance, NULL);
	return tray_window;
}

int win32_tray_start(SDL_Window *window)
{
	HWND handle;
	NOTIFYICONDATAW icon;
	HICON image;

	if (tray_added)
		return 1;
	if (!window)
		return 0;
	handle = tray_message_window();
	if (!handle)
		return 0;
	image = LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON,
		GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
	if (!image)
		image = LoadIconW(NULL, MAKEINTRESOURCEW(32512)); /* IDI_APPLICATION */
	ZeroMemory(&icon, sizeof(icon));
	icon.cbSize = sizeof(icon);
	icon.hWnd = handle;
	icon.uID = 1;
	icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	icon.uCallbackMessage = WIN32_TRAY_MESSAGE;
	icon.hIcon = image;
	lstrcpyW(icon.szTip, L"MeshCE is running in the background. Quit from this icon to stop it.");
	if (!Shell_NotifyIconW(NIM_ADD, &icon))
		return 0;
	tray_added = 1;
	return 1;
}

int win32_tray_take_command(void)
{
	int command = tray_command;

	tray_command = 0;
	return command;
}
