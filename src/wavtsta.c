/* wavtsta.c - A.26 Path A spike: raw DMA ch7 -> VIS custom DAC.
 *
 * Path B (waveOut) was falsified: waveOutGetNumDevs()==0, the VIS sound.drv
 * registers no waveform output device with MMSYSTEM. The DAC is reachable
 * only via DMA, so this spike programs the 8237 DMA channel 7 + the VIS DAC
 * registers directly and plays the same 440 Hz square tone.
 *
 * Semantics verified against the emulator source:
 *   - DAC = vis_audio_device (vis.cpp), I/O 0x220-0x22f, DMA ch7, 16-bit DMA.
 *       +0x00 MODE: bit4(0x10)=enable/start(edge: must differ from old mode),
 *                   bit3(0x08)=16bit, bit7(0x80)=mono, bits5-6=rate divisor
 *                   (rate = 44100 / 2^div). mode 0xD0 = 8bit mono, div=2 -> 11025 Hz.
 *       +0x09 CTRL: bit1(0x02)=IRQ7 enable, bit2(0x04)=done (set when
 *                   curcount>=count; cleared by reading +0x00 or +0x09).
 *       +0x0c/+0x0e COUNT lo/hi = number of DMA WORDS (8bit mono packs 2
 *                   samples/word); writing resets curcount.
 *   - 16-bit DMA addressing (at.cpp dma_read_word): byte addr =
 *       ((page<<16) & 0xfe0000) | (wordaddr << 1). So the 8237 holds a word
 *       address (phys>>1) and page reg 0x8A holds A17..A23.
 *   - DMA2 ports (0xC0-0xDF spaced x2): ch7 addr=0xCC, count=0xCE,
 *       single-mask=0xD4, mode=0xD6, clear-ff=0xD8. Mode 0x4B = single +
 *       read(mem->dev) + channel 3. Mask 0x07 / unmask 0x03 for ch7.
 *   - 128KB boundary: wordaddr<<1 wraps at 0x20000, so the buffer must not
 *       cross a 128KB physical boundary. We alloc 2*N and pick a safe sub-window.
 *
 * Diagnostic bands (direct A000), top->bottom:
 *   A: GlobalDosAlloc ok? green/red. bits = paragraph (phys>>4 low word).
 *   B: 128KB-safe placement? green/yellow. bits = phys low word.
 *   C: armed. green. bits = word count.
 *   D: done poll. blue until CTRL bit2 seen, then green.
 * Press button A to replay.
 */

#include <windows.h>
#include <conio.h>
#include <string.h>
#include <dos.h>     /* MK_FP, FP_OFF, FP_SEG */

#define VK_HC1_PRIMARY 0x72

extern void FAR PASCAL hcGetCursorPos(LPPOINT lpp);
DWORD WINAPI GlobalDosAlloc(DWORD);

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
#define SND_BYTES  16384U          /* ~1.49 s @ 11025 Hz, 8-bit mono. EVEN. */
#define SND_WORDS  (SND_BYTES / 2)  /* 8192 words (2 samples/word) */
#define TONE_HZ    440U

/* === VIS DAC ports (base 0x220) === */
#define DAC_MODE   0x220
#define DAC_CTRL   0x229
#define DAC_CNT_LO 0x22C
#define DAC_CNT_HI 0x22E
#define DAC_MODE_8BIT_MONO_11K  0xD0   /* mono(0x80)|rate div2(0x40)|enable(0x10) */

/* === DMA2 (16-bit) ports + page reg for ch7 === */
#define DMA_ADDR7  0xCC
#define DMA_CNT7   0xCE
#define DMA_MASK   0xD4
#define DMA_MODE   0xD6
#define DMA_CLRFF  0xD8
#define DMA_PAGE7  0x8A
#define DMA_MODE_PLAY7  0x4B   /* single|read(mem->dev)|ch3 */
#define DMA_MASK_SET7   0x07
#define DMA_MASK_CLR7   0x03

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

/* DMA buffer */
static WORD  g_sel        = 0;      /* protected-mode selector of the alloc */
static DWORD g_start_phys = 0;      /* physical byte addr of the safe window */
static WORD  g_start_off  = 0;      /* byte offset into the alloc of the window */
static BOOL  g_alloc_ok   = FALSE;
static BOOL  g_safe       = FALSE;
static BOOL  g_playing    = FALSE;

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

static void FillBand(int y0, int h, BYTE col)
{
    int y;
    if (!g_dva_active) return;
    for (y = y0; y < y0 + h; y++) {
        WORD ofs = (WORD)y * (WORD)SCR_W;
        _fmemset(g_fb_a000 + ofs, col, SCR_W);
    }
}

static void DrawBits(int y0, UINT value)
{
    int bit, x, y;
    if (!g_dva_active) return;
    for (bit = 0; bit < 16; bit++) {
        BYTE col = (value & (0x8000U >> bit)) ? C_WHITE : C_DARK;
        int  x0  = 16 + bit * 18;
        for (y = y0; y < y0 + 14; y++) {
            WORD ofs = (WORD)y * (WORD)SCR_W + (WORD)x0;
            for (x = 0; x < 16; x++) g_fb_a000[ofs + x] = col;
        }
    }
}

/* === Build the test tone into the safe window === */
static void BuildTone(void)
{
    unsigned char __far *p;
    UINT  i;
    UINT  half  = (UINT)(SND_RATE / (TONE_HZ * 2));   /* ~12 samples */
    UINT  cnt   = 0;
    unsigned char level = 0xD0;
    if (half == 0) half = 1;
    p = (unsigned char __far *)MK_FP(g_sel, g_start_off);
    for (i = 0; i < SND_BYTES; i++) {
        p[i] = level;
        if (++cnt >= half) {
            cnt = 0;
            level = (level == 0xD0) ? 0x30 : 0xD0;
        }
    }
}

/* === Allocate a DMA buffer that does not cross a 128KB boundary === */
static void AllocBuffer(void)
{
    DWORD dosaddr;
    DWORD base_phys;
    WORD  para;

    dosaddr = GlobalDosAlloc((DWORD)SND_BYTES * 2);   /* 2*N for safe sub-window */
    if (dosaddr == 0) {
        g_alloc_ok = FALSE;
        return;
    }
    g_alloc_ok = TRUE;
    para  = HIWORD(dosaddr);   /* real-mode paragraph (segment) */
    g_sel = LOWORD(dosaddr);   /* protected-mode selector */
    base_phys = (DWORD)para << 4;

    /* Pick a window of SND_BYTES that stays within one 128KB page. */
    if (((base_phys & 0x1FFFFL) + SND_BYTES) <= 0x20000L) {
        g_start_off  = 0;
        g_start_phys = base_phys;
        g_safe = TRUE;
    } else {
        /* Boundary lies inside [base, base+N): align up to it. Slack <= N. */
        DWORD bnd = (base_phys + 0x20000L) & ~0x1FFFFL;
        g_start_off  = (WORD)(bnd - base_phys);
        g_start_phys = bnd;
        g_safe = TRUE;   /* guaranteed to fit since we alloc'd 2*N */
    }
}

/* === Program DMA ch7 + VIS DAC, then start playback === */
static void ProgramAndStart(void)
{
    WORD  wordaddr = (WORD)((g_start_phys >> 1) & 0xFFFFL);
    BYTE  page     = (BYTE)((g_start_phys >> 16) & 0xFFL);
    WORD  cntreg   = SND_WORDS - 1;   /* 8237 count = items-1 */

    /* Stop any prior playback (mode 0 = disable; lets next 0xD0 be an edge). */
    outp(DAC_MODE, 0x00);

    /* --- 8237 DMA channel 7 (16-bit) --- */
    outp(DMA_MASK, DMA_MASK_SET7);            /* mask ch7 while programming */
    outp(DMA_CLRFF, 0xFF);                    /* clear byte-pointer flip-flop */
    outp(DMA_MODE, DMA_MODE_PLAY7);           /* single, mem->dev, ch3 */
    outp(DMA_ADDR7, (BYTE)(wordaddr & 0xFF)); /* word address lo */
    outp(DMA_ADDR7, (BYTE)(wordaddr >> 8));   /* word address hi */
    outp(DMA_PAGE7, page);                    /* A17..A23 */
    outp(DMA_CNT7, (BYTE)(cntreg & 0xFF));    /* word count lo */
    outp(DMA_CNT7, (BYTE)(cntreg >> 8));      /* word count hi */
    outp(DMA_MASK, DMA_MASK_CLR7);            /* unmask ch7 */

    /* --- VIS DAC --- */
    outp(DAC_CNT_LO, (BYTE)(SND_WORDS & 0xFF));   /* DAC count = words (resets curcount) */
    outp(DAC_CNT_HI, (BYTE)(SND_WORDS >> 8));
    outp(DAC_CTRL, 0x00);                          /* no IRQ; we poll bit2 */
    outp(DAC_MODE, DAC_MODE_8BIT_MONO_11K);        /* enable + trigger DRQ7 + timer */

    g_playing = TRUE;
}

static void ShowStatus(void)
{
    FillBand(0,  50, g_alloc_ok ? C_GREEN : C_RED);
    DrawBits(18, (UINT)(g_start_phys >> 4));        /* paragraph-ish */
    FillBand(50, 50, g_safe ? C_GREEN : C_YELLOW);
    DrawBits(68, (UINT)(g_start_phys & 0xFFFFL));   /* phys low word */
    FillBand(100, 50, g_alloc_ok ? C_GREEN : C_DARK);
    DrawBits(118, (UINT)SND_WORDS);                 /* word count */
    FillBand(150, 50, C_BLUE);                      /* waiting for done */
}

static void DoPlay(void)
{
    if (!g_alloc_ok) {
        FillBand(150, 50, C_RED);
        return;
    }
    ProgramAndStart();
    FillBand(150, 50, C_BLUE);
}

/* Poll DAC CTRL bit2 (done). Reading +0x09 clears it, so act on first sight. */
static void PollDone(void)
{
    POINT pt;
    pt.x = 0; pt.y = 0;
    hcGetCursorPos((LPPOINT)&pt);

    if (g_playing) {
        BYTE ctrl = (BYTE)inp(DAC_CTRL);
        if (ctrl & 0x04) {
            g_playing = FALSE;
            outp(DAC_MODE, 0x00);          /* disable */
            FillBand(150, 50, C_GREEN);    /* playback completed */
        }
    }

    if (GetAsyncKeyState(VK_HC1_PRIMARY) & 0x8000) {
        if (!g_playing) DoPlay();
    }
}

long FAR PASCAL _export WavTstAWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
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
        outp(DAC_MODE, 0x00);
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

int PASCAL WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    static const char CLASS_NAME[] = "WavTstA";
    WNDCLASS wc;
    HWND     hWnd;
    MSG      msg;

    (void)cmd;

    if (!hPrev) {
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = WavTstAWndProc;
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

    hWnd = CreateWindow(CLASS_NAME, "WavTstA",
        WS_POPUP | WS_VISIBLE, 0, 0, SCR_W, SCR_H,
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

    AllocBuffer();
    if (g_alloc_ok) BuildTone();
    ShowStatus();
    DoPlay();

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return msg.wParam;
}
