#include "ajan.hpp"
#include <commctrl.h>
#include <future>
static HWND window, logbox, input, statusbar, target;
static bool staffMode = false;
static Agent *uiAgent;
static Speech *uiSpeech;
static std::atomic<bool> *uiStop;
static std::atomic<bool> voiceStop = true, cancel = false, busy = false;
static std::atomic<bool> englishConversation = false;
static std::thread mic;
static std::vector<std::thread> jobs;
static int seq = 0;
static void append(std::wstring s) {
    auto len = GetWindowTextLengthW(logbox);
    SendMessageW(logbox, EM_SETSEL, len, len);
    SendMessageW(logbox, EM_REPLACESEL, FALSE, (LPARAM)s.c_str());
}
static void post(std::wstring s) {
    auto *p = new std::wstring(std::move(s));
    if (!PostMessageW(window, WM_APP + 1, 0, (LPARAM)p))
        delete p;
}
static void respond(std::string text) {
    if (busy.exchange(true)) {
        post(L"Ajan mevcut talebi tamamlıyor.\r\n");
        return;
    }
    jobs.emplace_back([text] {
        try {
            post(L"Müşteri: " + wide(text) + L"\r\n");
            J request = {
                {"channel", "local"},
                {"sender", "demo"},
                {"id", "ui-" + std::to_string(++seq) + "-" + std::to_string((uint64_t)clock_ms())},
                {"text", text}};
            J r;
            if (staffMode) {
                wchar_t owner[512];
                GetWindowTextW(target, owner, 512);
                r = pipe_request({{"op", "staff_reply"}, {"session", utf8(owner)}, {"text", text}});
            } else
                r = uiAgent ? uiAgent->turn(request) : pipe_request(request);
            post(L"Ajan: " + wide(r.value("text", "")) + L"\r\n\r\n");
            if (r.contains("language"))
                englishConversation = r["language"] == "en";
            if (uiSpeech && !voiceStop) {
                cancel = false;
                uiSpeech->stream(
                    r.value("text", ""), r.value("language", "tr"),
                    [](const float *p, size_t n, int sr) { play_pcm(p, n, sr, cancel); }, cancel);
            }
        } catch (const std::exception &e) {
            post(L"Hata: " + wide(e.what()) + L"\r\n");
        }
        busy = false;
    });
}
static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        CreateWindowW(L"STATIC", L"YerelAjan  •  Yerel müşteri hizmetleri", WS_CHILD | WS_VISIBLE,
                      24, 18, 750, 28, h, 0, 0, 0);
        logbox = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                                 24, 55, 800, 390, h, 0, 0, 0);
        input =
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                            24, 458, 645, 34, h, (HMENU)100, 0, 0);
        CreateWindowW(L"BUTTON", L"Gönder", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 682, 458, 142,
                      34, h, (HMENU)101, 0, 0);
        const wchar_t *labels[] = {L"Mikrofon", L"Randevular / mesajlar", L"Yapılandırma",
                                   L"Çalışana devret", L"Ajana geri ver"};
        for (int i = 0; i < 5; i++)
            CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE, 24 + i * 162, 510, 155, 35,
                          h, (HMENU)(INT_PTR)(102 + i), 0, 0);
        CreateWindowW(L"STATIC", L"Çalışan oturumu:", WS_CHILD | WS_VISIBLE, 24, 555, 130, 24, h, 0,
                      0, 0);
        target =
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"local:demo",
                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 160, 550, 664, 28, h, 0, 0, 0);
        statusbar = CreateWindowW(
            L"STATIC", L"CPU • Türkçe neural TTS • Bilgiler yerel • WhatsApp ağ istisnası",
            WS_CHILD | WS_VISIBLE, 24, 585, 800, 25, h, 0, 0, 0);
        for (HWND c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
            SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        append(L"Demo işletme bilgileri kullanılıyor. Gerçek müşterilere açmadan profilinizi "
               L"düzenleyin.\r\n\r\n");
        SetFocus(input);
        return 0;
    }
    case WM_APP + 1: {
        std::unique_ptr<std::wstring> p((std::wstring *)l);
        append(*p);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case 101: {
            wchar_t b[4097];
            GetWindowTextW(input, b, 4097);
            SetWindowTextW(input, L"");
            if (wcslen(b))
                respond(utf8(b));
            return 0;
        }
        case 102:
            if (!uiSpeech) {
                MessageBoxW(h, L"Ses modelleri yüklenemedi. Konsol tanısını kontrol edin.",
                            L"Ses hazır değil", MB_OK);
                return 0;
            }
            if (!voiceStop) {
                voiceStop = true;
                cancel = true;
                if (mic.joinable())
                    mic.join();
                SetWindowTextW((HWND)l, L"Mikrofon");
            } else {
                voiceStop = false;
                SetWindowTextW((HWND)l, L"Mikrofonu durdur");
                mic = std::thread([] {
                    try {
                        microphone(
                            *uiSpeech,
                            [](std::vector<float> x) {
                                cancel = true;
                                try {
                                    auto t =
                                        uiSpeech->transcribe(x, englishConversation ? "en" : "tr");
                                    respond(t);
                                } catch (const std::exception &e) {
                                    post(wide(e.what()) + L"\r\n");
                                }
                            },
                            voiceStop, [] { cancel = true; });
                    } catch (const std::exception &e) {
                        post(wide(e.what()) + L"\r\n");
                        voiceStop = true;
                    }
                });
            }
            return 0;
        case 103: {
            if (uiAgent) {
                std::lock_guard<std::mutex> g(uiAgent->store.mutex);
                append(wide(uiAgent->store.overview().dump(2)) + L"\r\n");
            } else
                append(wide(pipe_request({{"op", "overview"}}).dump(2)) + L"\r\n");
            return 0;
        }
        case 104:
            ShellExecuteW(h, L"open", L"notepad.exe", L"config.yaml", 0, SW_SHOW);
            return 0;
        case 105: {
            try {
                wchar_t owner[512];
                GetWindowTextW(target, owner, 512);
                pipe_request({{"op", "takeover"}, {"session", utf8(owner)}});
                staffMode = true;
                append(L"Çalışan oturumu etkin; Gönder düğmesi seçili müşteriye yanıt yollar.\r\n");
            } catch (const std::exception &e) {
                append(wide(e.what()) + L"\r\n");
            }
            return 0;
        }
        case 106: {
            wchar_t owner[512];
            GetWindowTextW(target, owner, 512);
            pipe_request({{"op", "resume"}, {"session", utf8(owner)}});
            staffMode = false;
            append(L"Ajan görüşmeyi devraldı.\r\n");
            return 0;
        }
        }
        break;
    case WM_CLOSE:
        voiceStop = true;
        cancel = true;
        if (mic.joinable())
            mic.join();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}
int run_ui(Agent *a, Speech *s, const J &, std::atomic<bool> &stop) {
    uiAgent = a;
    uiSpeech = s;
    uiStop = &stop;
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(0);
    wc.lpszClassName = L"YerelAjanWindow";
    wc.hCursor = LoadCursorW(0, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    window = CreateWindowW(wc.lpszClassName, L"YerelAjan — müşteri hizmetleri",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                           CW_USEDEFAULT, 875, 685, 0, 0, wc.hInstance, 0);
    ShowWindow(window, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && GetFocus() == input) {
            SendMessageW(window, WM_COMMAND, 101, 0);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    for (auto &t : jobs)
        if (t.joinable())
            t.join();
    return 0;
}
