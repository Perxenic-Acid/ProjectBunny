#include "DX12Overlay.h"

#include "DX12State.h"

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg) {
	case WM_NCHITTEST:
		return HTTRANSPARENT;
	case WM_TIMER:
		InvalidateRect(hwnd, nullptr, TRUE);
		return 0;
	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		RECT client;
		GetClientRect(hwnd, &client);
		HBRUSH transparentBrush = CreateSolidBrush(RGB(1, 1, 1));
		FillRect(dc, &client, transparentBrush);
		DeleteObject(transparentBrush);

		HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
		HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;

		wchar_t text[256];
		DX12GetOverlayStatus(text, ARRAYSIZE(text));

		SIZE size = {};
		GetTextExtentPoint32W(dc, text, lstrlenW(text), &size);

		RECT background = {
			12,
			8,
			20 + size.cx,
			18 + size.cy
		};
		FillRect(dc, &background, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(0, 255, 0));
		TextOutW(dc, 16, 12, text, lstrlenW(text));

		if (oldFont)
			SelectObject(dc, oldFont);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		DX12SetOverlayWindow(nullptr);
		return 0;
	default:
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

DWORD WINAPI DX12OverlayThread(void*)
{
	const wchar_t className[] = L"3DMigotoDX12LightweightOverlay";

	WNDCLASSW wc = {};
	wc.lpfnWndProc = OverlayWndProc;
	wc.hInstance = DX12GetModule();
	wc.lpszClassName = className;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	RegisterClassW(&wc);

	int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
	int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
	int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	int height = 48;

	HWND hwnd = CreateWindowExW(
		WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		className,
		L"3DMigoto DX12 Lightweight Overlay",
		WS_POPUP,
		x, y, width, height,
		nullptr, nullptr, DX12GetModule(), nullptr);

	if (!hwnd) {
		DX12Log("Failed to create lightweight overlay window, error=%lu\n", GetLastError());
		return 0;
	}

	DX12SetOverlayWindow(hwnd);
	SetLayeredWindowAttributes(hwnd, RGB(1, 1, 1), 220, LWA_COLORKEY | LWA_ALPHA);
	ShowWindow(hwnd, SW_SHOWNOACTIVATE);
	SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height,
		SWP_NOACTIVATE | SWP_SHOWWINDOW);
	SetTimer(hwnd, 1, 250, nullptr);

	DX12Log("Lightweight overlay window created: %p\n", hwnd);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	return 0;
}

void DX12DrawSwapChainText(IDXGISwapChain *swapChain)
{
	if (!swapChain)
		return;

	DXGI_SWAP_CHAIN_DESC desc = {};
	if (FAILED(swapChain->GetDesc(&desc)) || !desc.OutputWindow || !IsWindow(desc.OutputWindow))
		return;

	HDC dc = GetDC(desc.OutputWindow);
	if (!dc)
		return;

	int oldBkMode = SetBkMode(dc, TRANSPARENT);
	COLORREF oldTextColor = SetTextColor(dc, RGB(0, 255, 0));
	HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
	wchar_t text[256];
	DX12GetOverlayStatus(text, ARRAYSIZE(text));
	SIZE size = {};
	GetTextExtentPoint32W(dc, text, lstrlenW(text), &size);
	RECT background = {
		16,
		16,
		24 + size.cx,
		26 + size.cy
	};
	FillRect(dc, &background, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

	TextOutW(dc, 20, 20, text, lstrlenW(text));

	if (oldFont)
		SelectObject(dc, oldFont);
	SetTextColor(dc, oldTextColor);
	SetBkMode(dc, oldBkMode);
	ReleaseDC(desc.OutputWindow, dc);
}

void DX12CloseOverlayWindow()
{
	HWND hwnd = DX12GetOverlayWindow();
	if (hwnd)
		PostMessageW(hwnd, WM_CLOSE, 0, 0);
}
