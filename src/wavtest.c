/* wavtest.c - A.26 Path B spike: does VIS waveOut emit to the DAC in MAME?
 *
 * The one question this answers (recon open-Q #1): if we hand a PCM buffer to
 * the Modular-Windows MMSYSTEM waveform stack (waveOutOpen + waveOutWrite),
 * does sound come out of the VIS custom DAC under MAME? SYSTEM.INI already
 * loads `drivers=mmsystem.dll` + `sound.drv=sound.drv`, so the path SHOULD be
 * wired. If a tone plays here, the whole digi-voice feature is near-trivial.
 *
 * Test signal: a 440 Hz square wave, 8-bit unsigned PCM, MONO, ~2 seconds.
 *   Rate = 11025 Hz (a clean native-ish rate) to isolate "does waveOut work
 *   at all" from "does 7000 Hz resample" (the eventual digi rate). A follow-up
 *   can retest at 7000.
 *
 * Diagnostic display (direct A000, A.21 DispDib pattern) — 4 horizontal bands,
 * top to bottom, so we learn WHERE it failed even with no audio:
 *   Band A (devs):  green if waveOutGetNumDevs() >= 1, else red.
 *   Band B (open):  green if waveOutOpen == MMSYSERR_NOERROR, else yellow.
 *   Band C (write): green if waveOutWrite == MMSYSERR_NOERROR, else magenta.
 *   Band D (done):  blue while WHDR_DONE not set, green once playback completes.
 * Each band also renders its raw 16-bit return code as MSB..LSB bit cells
 * (white=1, dark=0) so exact MMSYSERR values are readable on screen.
 *
 * Press button A (PRIMARY) to replay. No floating point, no libc math.
 */

#include <windows.h>
#include <mmsystem.h>
#include <conio.h>
#include <string.h>
#include <dos.h>     /* MK_FP */

/* === Hand-controller VK codes (per reference_vk_hc1_codes memo) === */
#define VK_HC1_PRIMARY    0x72  /* button "A" -> replay */

/* HC.DLL poll kept to satisfy input routing (reference_vis_hc_input_quirks). */
extern void FAR PASCAL hcGetCursorPos(LPPOINT lpp);

/* === Display: DispDib direct A000 (A.21 pattern) === */
extern WORD _A000H;
WORD FAR PASCAL DisplayDib(LPBITMAPINFO, LPSTR, WORD);
#define DD_BEGIN          0x8000
#define DD_END            0x4000
#define DD_MODE_320x200x8 0x0001
#define DD_NOPAL_OR_TASK  0x0040

#define SCR_W 320
#define SCR_H 200

/* === Audio params === */
#define SND_RATE   11025U
#define SND_SECS   2
#define SND_BYTES  ((DWORD)SND_RATE * SND_SECS)   /* 22050 bytes, 8-bit mono */
#define TONE_HZ    440U

/* Palette indices */
#define C_BLACK   0
#define C_RED     1
#define C_YELLOW  2
#define C_MAGENTA 3
#define C_BLUE    4
#define C_GREEN   5
#define C_DARK    6
#define C_WHITE   7

static BYTE __far *g_fb_a000 = NULL;
static BOOL  g_dva_active    = FALSE;
static BITMAPINFO bmi;

/* PCM buffer in its own far segment (under 64 KB so no E1157). */
static unsigned char __far g_pcm[SND_BYTES];

static HWAVEOUT g_hwo = NULL;
static WAVEHDR  g_hdr;
static BOOL     g_playing = FALSE;

/* Last return codes, for on-screen readout. */
static UINT g_devs    = 0;
static UINT g_openrc  = 0xFFFF;
static UINT g_writerc = 0xFFFF;

/* === VGA palette (direct DAC at 0x3C8/0x3C9) === */
static void SetPaletteEntry(BYTE idx, BYTE r6, BYTE g6, BYTE b6)
{
    outp(0x3C8, idx);
    outp(0x3C9, r6);
    outp(0x3C9, g6);
    outp(0x3C9, b6);
}

static void InitPalette(void)
{
    SetPaletteEntry(C_BLACK,    0,  0,  0);
    SetPaletteEntry(C_RED,     63, 10, 10);
    SetPaletteEntry(C_YELLOW,  63, 63, 10);
    SetPaletteEntry(C_MAGENTA, 63, 10, 50);
    SetPaletteEntry(C_BLUE,    10, 20, 63);
    SetPaletteEntry(C_GREEN,   10, 63, 10);
    SetPaletteEntry(C_DARK,    12, 12, 12);
    SetPaletteEntry(C_WHITE,   63, 63, 63);
}

/* Fill a horizontal band [y0, y0+h) with a color. */
static void FillBand(int y0, int h, BYTE col)
{
    int y;
    if (!g_dva_active) return;
    for (y = y0; y < y0 + h; y++) {
        WORD ofs = (WORD)y * (WORD)SCR_W;
        _fmemset(g_fb_a000 + ofs, col, SCR_W);
    }
}

/* Render a 16-bit value as 16 cells (MSB..LSB), white=1 / dark=0, at y. */
static void DrawBits(int y0, UINT value)
{
    int bit, x, y;
    if (!g_dva_active) return;
    for (bit = 0; bit < 16; bit++) {
        BYTE col = (value & (0x8000U >> bit)) ? C_WHITE : C_DARK;
        int  x0  = 16 + bit * 18;   /* 16 cells * 18 px = 288, centered-ish */
        for (y = y0; y < y0 + 14; y++) {
            WORD ofs = (WORD)y * (WORD)SCR_W + (WORD)x0;
            for (x = 0; x < 16; x++) g_fb_a000[ofs + x] = col;
        }
    }
}

/* === Build the test tone === */
static void BuildTone(void)
{
    DWORD i;
    UINT  half   = (UINT)(SND_RATE / (TONE_HZ * 2));   /* ~12 samples */
    UINT  cnt    = 0;
    unsigned char level = 0xD0;                        /* high phase */
    if (half == 0) half = 1;
    for (i = 0; i < SND_BYTES; i++) {
        g_pcm[i] = level;
        if (++cnt >= half) {
            cnt = 0;
            level = (level == 0xD0) ? 0x30 : 0xD0;     /* toggle square */
        }
    }
}

/* === The actual Path B test === */
static void DoPlay(void)
{
    PCMWAVEFORMAT wf;

    /* If a prior play is still open, tear it down first. */
    if (g_hwo) {
        if (g_hdr.dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(g_hwo, &g_hdr, sizeof(g_hdr));
        waveOutClose(g_hwo);
        g_hwo = NULL;
    }
    g_playing = FALSE;

    g_devs = waveOutGetNumDevs();
    FillBand(0, 50, g_devs >= 1 ? C_GREEN : C_RED);
    DrawBits(18, g_devs);
    if (g_devs == 0) {
        /* No waveform device at all -> Path B dead, fall back to Path A. */
        FillBand(50, 50, C_DARK);  DrawBits(68, 0xDEAD);
        FillBand(100, 50, C_DARK); DrawBits(118, 0xDEAD);
        FillBand(150, 50, C_RED);
        return;
    }

    wf.wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.wf.nChannels       = 1;
    wf.wf.nSamplesPerSec  = SND_RATE;
    wf.wf.nAvgBytesPerSec = SND_RATE;     /* 8-bit mono: 1 byte/sample */
    wf.wf.nBlockAlign     = 1;
    wf.wBitsPerSample     = 8;

    g_openrc = waveOutOpen(&g_hwo, WAVE_MAPPER, (const WAVEFORMAT FAR *)&wf,
                           0L, 0L, 0L /* CALLBACK_NULL */);
    FillBand(50, 50, g_openrc == MMSYSERR_NOERROR ? C_GREEN : C_YELLOW);
    DrawBits(68, g_openrc);
    if (g_openrc != MMSYSERR_NOERROR) {
        g_hwo = NULL;
        FillBand(100, 50, C_DARK); DrawBits(118, 0xDEAD);
        FillBand(150, 50, C_YELLOW);
        return;
    }

    _fmemset(&g_hdr, 0, sizeof(g_hdr));
    g_hdr.lpData         = (LPSTR)g_pcm;
    g_hdr.dwBufferLength = SND_BYTES;
    g_hdr.dwFlags        = 0;
    g_hdr.dwLoops        = 0;
    waveOutPrepareHeader(g_hwo, &g_hdr, sizeof(g_hdr));

    g_writerc = waveOutWrite(g_hwo, &g_hdr, sizeof(g_hdr));
    FillBand(100, 50, g_writerc == MMSYSERR_NOERROR ? C_GREEN : C_MAGENTA);
    DrawBits(118, g_writerc);

    if (g_writerc == MMSYSERR_NOERROR) {
        g_playing = TRUE;
        FillBand(150, 50, C_BLUE);   /* waiting for WHDR_DONE */
    } else {
        FillBand(150, 50, C_MAGENTA);
    }
}

/* Poll for completion (no callback). Called from WM_TIMER. */
static void PollDone(void)
{
    POINT pt;
    pt.x = 0; pt.y = 0;
    hcGetCursorPos((LPPOINT)&pt);   /* keep input routing alive */

    if (g_playing && g_hwo && (g_hdr.dwFlags & WHDR_DONE)) {
        g_playing = FALSE;
        waveOutUnprepareHeader(g_hwo, &g_hdr, sizeof(g_hdr));
        waveOutClose(g_hwo);
        g_hwo = NULL;
        FillBand(150, 50, C_GREEN);  /* playback completed cleanly */
    }

    /* Replay on button A (edge not needed for a spike). */
    if (GetAsyncKeyState(VK_HC1_PRIMARY) & 0x8000) {
        if (!g_playing) DoPlay();
    }
}

/* === Window proc === */
long FAR PASCAL _export WavTestWndProc(HWND hWnd, UINT msg,
                                       WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        SetTimer(hWnd, 1, 50, NULL);
        return 0;

    case WM_TIMER:
        PollDone();
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, 1);
        if (g_hwo) {
            if (g_hdr.dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(g_hwo, &g_hdr, sizeof(g_hdr));
            waveOutClose(g_hwo);
            g_hwo = NULL;
        }
        if (g_dva_active) {
            DisplayDib(&bmi, NULL,
                (WORD)(DD_END | DD_MODE_320x200x8 | DD_NOPAL_OR_TASK));
            g_dva_active = FALSE;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}

/* === WinMain === */
int PASCAL WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    static const char CLASS_NAME[] = "WavTest";
    WNDCLASS wc;
    HWND     hWnd;
    MSG      msg;

    (void)cmd;

    if (!hPrev) {
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = WavTestWndProc;
        wc.cbClsExtra    = 0;
        wc.cbWndExtra    = 0;
        wc.hInstance     = hInst;
        wc.hIcon         = NULL;
        wc.hCursor       = NULL;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszMenuName  = NULL;
        wc.lpszClassName = CLASS_NAME;
        if (!RegisterClass(&wc)) return 0;
    }

    hWnd = CreateWindow(CLASS_NAME, "WavTest",
        WS_POPUP | WS_VISIBLE,
        0, 0, SCR_W, SCR_H,
        NULL, NULL, hInst, NULL);
    if (!hWnd) return 0;

    ShowCursor(FALSE);
    ShowWindow(hWnd, show);
    UpdateWindow(hWnd);
    SetFocus(hWnd);
    SetActiveWindow(hWnd);

    bmi.bmiHeader.biSize          = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth         = SCR_W;
    bmi.bmiHeader.biHeight        = SCR_H;
    bmi.bmiHeader.biPlanes        = 1;
    bmi.bmiHeader.biBitCount      = 8;
    bmi.bmiHeader.biCompression   = BI_RGB;
    bmi.bmiHeader.biSizeImage     = (DWORD)SCR_W * (DWORD)SCR_H;
    bmi.bmiHeader.biXPelsPerMeter = 0;
    bmi.bmiHeader.biYPelsPerMeter = 0;
    bmi.bmiHeader.biClrUsed       = 0;
    bmi.bmiHeader.biClrImportant  = 0;

    if (DisplayDib(&bmi, NULL,
            (WORD)(DD_BEGIN | DD_MODE_320x200x8 | DD_NOPAL_OR_TASK)) == 0) {
        g_fb_a000    = (BYTE __far *)MK_FP((WORD)((DWORD)(LPVOID)&_A000H), 0);
        g_dva_active = TRUE;
    }

    if (g_dva_active) {
        InitPalette();
        FillBand(0, SCR_H, C_BLACK);
    }

    BuildTone();
    DoPlay();

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return msg.wParam;
}
