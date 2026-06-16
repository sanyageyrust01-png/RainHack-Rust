#include "GUI.h"
#include "Util.h"
#include "Config.h"

#include <Windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <atomic>
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "msimg32.lib")

namespace gui {

static const wchar_t* kClassName = L"RainHackLoaderWnd";
static HWND          g_hwnd = nullptr;
static HWND          g_hwndLogin = nullptr;
static HWND          g_hwndSkip  = nullptr;
static HFONT         g_fontReg = nullptr;
static HFONT         g_fontBold = nullptr;
static HFONT         g_fontTitle = nullptr;
static HFONT         g_fontStatus = nullptr;
static std::string   g_status;
static int           g_statusKind = 0;
static LoginCallback g_loginCb;
static bool          g_busy = false;
static int           g_alpha = 0;
static const int     kRadius = 18;

static std::atomic<bool> g_skipPressed{ false };
static std::atomic<bool> g_skipVisible{ false };

static const COLORREF kBg        = RGB(10, 10, 16);
static const COLORREF kPanelHi   = RGB(22, 22, 32);
static const COLORREF kBorder    = RGB(46, 46, 64);
static const COLORREF kText      = RGB(225, 225, 235);
static const COLORREF kTextDim   = RGB(140, 140, 165);
static const COLORREF kAccent    = RGB(124, 92, 255);
static const COLORREF kAccentDim = RGB(82, 56, 195);
static const COLORREF kSuccess   = RGB(52, 211, 153);
static const COLORREF kError     = RGB(248, 113, 113);
static const COLORREF kWarn      = RGB(245, 158, 11);

void setStatus(const std::string& msg, int kind) {
    g_status = msg; g_statusKind = kind;
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

void setBusy(bool busy) {
    g_busy = busy;
    if (g_hwndLogin) EnableWindow(g_hwndLogin, busy ? FALSE : TRUE);
}

void onLogin(LoginCallback cb) { g_loginCb = cb; }
HWND window() { return g_hwnd; }
void log(const std::string&) {}

void showSkipButton(bool show) {
    g_skipVisible = show;
    if (g_hwndSkip) {
        ShowWindow(g_hwndSkip, show ? SW_SHOW : SW_HIDE);
        EnableWindow(g_hwndSkip, show ? TRUE : FALSE);
    }
    if (!show) g_skipPressed = false;
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

bool consumeSkipPress() {
    return g_skipPressed.exchange(false);
}

static void fillRect(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void roundedRect(HDC dc, int x, int y, int w, int h, int r, COLORREF fill, COLORREF border) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN   p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, x, y, x + w, y + h, r, r);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

static void drawText(HDC dc, const wchar_t* s, int x, int y, int w, int h, COLORREF c, HFONT f, UINT format) {
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, f);
    RECT r = { x, y, x + w, y + h };
    DrawTextW(dc, s, -1, &r, format);
    SelectObject(dc, of);
}

static void drawTextA(HDC dc, const std::string& s, int x, int y, int w, int h, COLORREF c, HFONT f, UINT format) {
    auto ws = util::s2ws(s);
    drawText(dc, ws.c_str(), x, y, w, h, c, f, format);
}

static void onPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC raw = BeginPaint(hwnd, &ps);
    RECT cr; GetClientRect(hwnd, &cr);
    int W = cr.right, H = cr.bottom;

    HDC dc = CreateCompatibleDC(raw);
    HBITMAP bmp = CreateCompatibleBitmap(raw, W, H);
    HGDIOBJ ob = SelectObject(dc, bmp);

    fillRect(dc, cr, kBg);

    {
        TRIVERTEX v[2];
        v[0].x = 0;     v[0].y = 0;     v[0].Red = 0x2200; v[0].Green = 0x1500; v[0].Blue = 0x4500; v[0].Alpha = 0;
        v[1].x = W;     v[1].y = 130;   v[1].Red = 0x0A00; v[1].Green = 0x0A00; v[1].Blue = 0x1000; v[1].Alpha = 0;
        GRADIENT_RECT gr = { 0, 1 };
        GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
    }

    drawText(dc, RAINHACK_LOADER_NAME_W, 0, 24, W, 38, RGB(255, 255, 255), g_fontTitle, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    drawText(dc, L"v" RAINHACK_LOADER_VERSION_W, 0, 64, W, 18, kTextDim, g_fontReg, DT_CENTER | DT_SINGLELINE);

    if (!g_status.empty()) {
        COLORREF c = kTextDim;
        if (g_statusKind == 1) c = kSuccess;
        else if (g_statusKind == 2) c = kError;
        else if (g_statusKind == 3) c = kWarn;
        drawTextA(dc, g_status, 0, H - 36, W, 22, c, g_fontStatus, DT_CENTER | DT_SINGLELINE);
    }

    BitBlt(raw, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);

    EndPaint(hwnd, &ps);
}

static void applyRoundedRegion(HWND hwnd) {
    RECT r; GetWindowRect(hwnd, &r);
    int W = r.right - r.left, H = r.bottom - r.top;
    HRGN rgn = CreateRoundRectRgn(0, 0, W + 1, H + 1, kRadius * 2, kRadius * 2);
    SetWindowRgn(hwnd, rgn, TRUE);
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            applyRoundedRegion(hwnd);
            SetTimer(hwnd, 1, 12, nullptr);
            return 0;
        }
        case WM_TIMER: {
            if (wp == 1) {
                g_alpha += 28;
                if (g_alpha >= 245) { g_alpha = 245; KillTimer(hwnd, 1); }
                SetLayeredWindowAttributes(hwnd, 0, (BYTE)g_alpha, LWA_ALPHA);
            }
            return 0;
        }
        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            if (pt.y < 90) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, kText);
            SetBkColor(dc, kBg);
            static HBRUSH b = CreateSolidBrush(kBg);
            return (LRESULT)b;
        }
        case WM_DRAWITEM: {
            auto* di = (LPDRAWITEMSTRUCT)lp;
            if (di->hwndItem == g_hwndLogin) {
                bool down = (di->itemState & ODS_SELECTED) != 0;
                bool dis  = (di->itemState & ODS_DISABLED) != 0;
                COLORREF fill = dis ? kAccentDim : (down ? kAccentDim : kAccent);
                roundedRect(di->hDC, di->rcItem.left, di->rcItem.top,
                            di->rcItem.right - di->rcItem.left,
                            di->rcItem.bottom - di->rcItem.top,
                            12, fill, fill);
                drawText(di->hDC, dis ? L"Working..." : L"Login",
                         di->rcItem.left, di->rcItem.top,
                         di->rcItem.right - di->rcItem.left,
                         di->rcItem.bottom - di->rcItem.top,
                         RGB(255, 255, 255), g_fontBold,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            if (di->hwndItem == g_hwndSkip) {
                bool down = (di->itemState & ODS_SELECTED) != 0;
                bool dis  = (di->itemState & ODS_DISABLED) != 0;
                COLORREF fill = dis ? RGB(38, 38, 50) : (down ? RGB(58, 58, 78) : RGB(48, 48, 64));
                roundedRect(di->hDC, di->rcItem.left, di->rcItem.top,
                            di->rcItem.right - di->rcItem.left,
                            di->rcItem.bottom - di->rcItem.top,
                            10, fill, fill);
                drawText(di->hDC, L"Skip wait",
                         di->rcItem.left, di->rcItem.top,
                         di->rcItem.right - di->rcItem.left,
                         di->rcItem.bottom - di->rcItem.top,
                         RGB(220, 220, 235), g_fontBold,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            return FALSE;
        }
        case WM_PAINT: onPaint(hwnd); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_COMMAND: {
            if (HIWORD(wp) == BN_CLICKED && (HWND)lp == g_hwndLogin) {
                if (g_busy) return 0;
                if (g_loginCb) g_loginCb();
                return 0;
            }
            if (HIWORD(wp) == BN_CLICKED && (HWND)lp == g_hwndSkip) {
                g_skipPressed = true;
                return 0;
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_RETURN && !g_busy) {
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(0, BN_CLICKED), (LPARAM)g_hwndLogin);
                return 0;
            }
            if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
            break;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HFONT mkFont(int size, int weight, const wchar_t* face) {
    return CreateFontW(size, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

static void registerClass(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
}

void show(const std::string& /*motd*/) {
    HINSTANCE inst = GetModuleHandleW(nullptr);

    INITCOMMONCONTROLSEX icc{}; icc.dwSize = sizeof(icc); icc.dwICC = ICC_STANDARD_CLASSES; InitCommonControlsEx(&icc);

    g_fontTitle  = mkFont(36, FW_BOLD,     L"Segoe UI");
    g_fontBold   = mkFont(17, FW_SEMIBOLD, L"Segoe UI");
    g_fontReg    = mkFont(14, FW_NORMAL,   L"Segoe UI");
    g_fontStatus = mkFont(15, FW_SEMIBOLD, L"Segoe UI");

    registerClass(inst);

    int W = 460, H = 260;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;

    g_alpha = 0;
    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_LAYERED,
                             kClassName, RAINHACK_LOADER_NAME_W,
                             WS_POPUP | WS_SYSMENU,
                             sx, sy, W, H, nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g_hwnd, 0, 0, LWA_ALPHA);

    int btnW = 200, btnH = 44;
    int bx = (W - btnW) / 2;
    int by = 120;
    g_hwndLogin = CreateWindowExW(0, L"BUTTON", L"Login",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  bx, by, btnW, btnH, g_hwnd, (HMENU)2, inst, nullptr);
    SendMessageW(g_hwndLogin, WM_SETFONT, (WPARAM)g_fontBold, TRUE);

    int skipW = 130, skipH = 30;
    int skx = (W - skipW) / 2;
    int sky = by + btnH + 10;
    g_hwndSkip = CreateWindowExW(0, L"BUTTON", L"Skip wait",
                                 WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                 skx, sky, skipW, skipH, g_hwnd, (HMENU)3, inst, nullptr);
    SendMessageW(g_hwndSkip, WM_SETFONT, (WPARAM)g_fontBold, TRUE);

    SetFocus(g_hwndLogin);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
}

void hide() {
    if (g_hwnd) ShowWindow(g_hwnd, SW_HIDE);
}

int runMessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

}
