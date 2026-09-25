// Music to mic - tray toggle that mixes PC sound (everything except Discord) + your mic
// into VB-CABLE, and switches the Windows default microphone to the cable while ON.
// Windows 11 / Windows 10 20H1+ (needs the process-loopback audio API). MIT licence.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <tlhelp32.h>
#include <timeapi.h>
#include <wrl/implements.h>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "uuid.lib")

using namespace Microsoft::WRL;

// ---------- undocumented but long-stable: IPolicyConfig (set default audio device) ----------
interface DECLSPEC_UUID("f8679f50-850a-41cf-9c72-430f290290c8") IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole eRole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
class DECLSPEC_UUID("870af99c-171d-4f9e-af0d-e63df40c2bc9") CPolicyConfigClient;

// ---------- globals / config ----------
static std::wstring g_dir;            // folder of the exe
static std::wstring g_exePath;
static float g_musicGain = 4.0f, g_micGain = 1.0f;   // music_gain = fixed gain, or the max gain when auto-levelling
static float g_musicTargetDb = -24.f;                   // 0 = no auto-levelling
static FILETIME g_cfgTime = {};
static std::wstring g_excludeProc = L"Discord.exe";
static std::wstring g_micName;        // voice source; empty = whatever mic is default when switching ON
// VB-CABLE 4.5 names its playback side "Speakers (VB-Audio Virtual Cable)", older versions "CABLE Input"
static std::wstring g_cableIn = L"CABLE Input|Speakers (VB-Audio Virtual Cable)", g_cableOut = L"CABLE Output";
static bool g_verbose = false;
static std::mutex g_logMx;

static void Log(const std::wstring& s) {
    std::lock_guard<std::mutex> lk(g_logMx);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t ts[64]; swprintf_s(ts, L"%04d-%02d-%02d %02d:%02d:%02d ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring path = g_dir + L"music-to-mic.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f) { fwprintf(f, L"%s%s\n", ts, s.c_str()); fclose(f); }
}
static std::wstring Hex(HRESULT hr) { wchar_t b[16]; swprintf_s(b, L"0x%08X", (unsigned)hr); return b; }

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}
static void LoadConfig() {
    { WIN32_FILE_ATTRIBUTE_DATA fa; if (GetFileAttributesExW((g_dir + L"config.txt").c_str(), GetFileExInfoStandard, &fa)) g_cfgTime = fa.ftLastWriteTime; }
    std::wifstream f(g_dir + L"config.txt");
    std::wstring line;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == L'#') continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
        if (k == L"music_gain") g_musicGain = (float)_wtof(v.c_str());
        else if (k == L"mic_gain") g_micGain = (float)_wtof(v.c_str());
        else if (k == L"music_target_db") g_musicTargetDb = (float)_wtof(v.c_str());
        else if (k == L"exclude_process") g_excludeProc = v;
        else if (k == L"mic") g_micName = v;
        else if (k == L"cable_playback") g_cableIn = v;
        else if (k == L"cable_recording") g_cableOut = v;
        else if (k == L"verbose") g_verbose = (v == L"1" || v == L"true");
    }
}
static bool ConfigChanged() { WIN32_FILE_ATTRIBUTE_DATA fa; return GetFileAttributesExW((g_dir + L"config.txt").c_str(), GetFileExInfoStandard, &fa) && CompareFileTime(&fa.ftLastWriteTime, &g_cfgTime) != 0; }
static bool IEquals(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
static bool IContains(const std::wstring& hay, const std::wstring& needle) {
    std::wstring h = hay, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), ::towlower);
    std::transform(n.begin(), n.end(), n.begin(), ::towlower);
    return h.find(n) != std::wstring::npos;
}

// ---------- device helpers ----------
struct DevInfo { std::wstring id, name; };
static std::wstring DevName(IMMDevice* d) {
    ComPtr<IPropertyStore> ps; if (FAILED(d->OpenPropertyStore(STGM_READ, &ps))) return L"";
    PROPVARIANT v; PropVariantInit(&v);
    std::wstring r;
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) r = v.pwszVal;
    PropVariantClear(&v); return r;
}
static std::vector<DevInfo> ListDevices(EDataFlow flow) {
    std::vector<DevInfo> out;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return out;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        ComPtr<IMMDevice> d; if (FAILED(col->Item(i, &d))) continue;
        LPWSTR id = nullptr; if (FAILED(d->GetId(&id))) continue;
        out.push_back({ id, DevName(d.Get()) }); CoTaskMemFree(id);
    }
    return out;
}
// nameSub may hold several alternatives separated by '|', tried in order
static std::wstring FindDevice(EDataFlow flow, const std::wstring& nameSub) {
    auto devs = ListDevices(flow);
    size_t start = 0;
    while (start <= nameSub.size()) {
        size_t bar = nameSub.find(L'|', start);
        std::wstring part = Trim(nameSub.substr(start, bar == std::wstring::npos ? std::wstring::npos : bar - start));
        if (!part.empty()) for (auto& d : devs) if (IContains(d.name, part)) return d.id;
        if (bar == std::wstring::npos) break;
        start = bar + 1;
    }
    return L"";
}
static std::wstring DefaultCapture() {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return L"";
    ComPtr<IMMDevice> d; if (FAILED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &d))) return L"";
    LPWSTR id = nullptr; if (FAILED(d->GetId(&id))) return L"";
    std::wstring r = id; CoTaskMemFree(id); return r;
}
static ComPtr<IMMDevice> GetDevice(const std::wstring& id) {
    ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDevice> d;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) en->GetDevice(id.c_str(), &d);
    return d;
}
static std::wstring NameOf(const std::wstring& id) { auto d = GetDevice(id); return d ? DevName(d.Get()) : L"?"; }
static bool SetDefaultDevice(const std::wstring& id) {
    ComPtr<IPolicyConfig> pc;
    HRESULT hr = CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pc));
    if (FAILED(hr)) { Log(L"IPolicyConfig create failed " + Hex(hr)); return false; }
    bool ok = true;
    for (ERole r : { eConsole, eMultimedia, eCommunications }) if (FAILED(pc->SetDefaultEndpoint(id.c_str(), r))) ok = false;
    return ok;
}
static void UnmuteFull(const std::wstring& id) {
    auto d = GetDevice(id); if (!d) return;
    ComPtr<IAudioEndpointVolume> v;
    if (SUCCEEDED(d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &v))) { v->SetMute(FALSE, nullptr); v->SetMasterVolumeLevelScalar(1.0f, nullptr); }
}

// Peak level of a capture device over `ms` milliseconds (0 = digital silence / could not open). Used to skip a dead mic.
static float ProbeMic(const std::wstring& id, int ms) {
    auto d = GetDevice(id); if (!d) return 0.f;
    ComPtr<IAudioClient> ac; if (FAILED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &ac))) return 0.f;
    WAVEFORMATEX* f = nullptr; if (FAILED(ac->GetMixFormat(&f))) return 0.f;
    bool isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE*)f)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    float peak = 0.f;
    if (isFloat && SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 5000000, 0, f, nullptr))) {
        ComPtr<IAudioCaptureClient> cc;
        if (SUCCEEDED(ac->GetService(IID_PPV_ARGS(&cc))) && SUCCEEDED(ac->Start())) {
            ULONGLONG until = GetTickCount64() + ms;
            while (GetTickCount64() < until) {
                Sleep(10); UINT32 pkt = 0;
                while (SUCCEEDED(cc->GetNextPacketSize(&pkt)) && pkt > 0) {
                    BYTE* data; UINT32 frames; DWORD flags;
                    if (FAILED(cc->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) { const float* s = (const float*)data; for (UINT32 i = 0; i < frames * f->nChannels; i++) peak = std::max(peak, fabsf(s[i])); }
                    cc->ReleaseBuffer(frames);
                }
            }
            ac->Stop();
        }
    }
    CoTaskMemFree(f); return peak;
}

// ---------- process helpers ----------
static DWORD FindRootProcess(const std::wstring& exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    std::vector<std::pair<DWORD, DWORD>> procs; // pid, parent
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) do { if (IEquals(pe.szExeFile, exeName)) procs.push_back({ pe.th32ProcessID, pe.th32ParentProcessID }); } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    for (auto& p : procs) {
        bool parentIsSame = false;
        for (auto& q : procs) if (q.first == p.second) parentIsSame = true;
        if (!parentIsSame) return p.first;
    }
    return procs.empty() ? 0 : procs[0].first;
}
static bool ProcessAlive(DWORD pid) {
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0; bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE; CloseHandle(h); return alive;
}

// ---------- process loopback activation ----------
class ActivateHandler : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
    HANDLE ev; ComPtr<IAudioClient> client; HRESULT hr = E_FAIL;
    ActivateHandler() { ev = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~ActivateHandler() { CloseHandle(ev); }
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT hrAct = E_FAIL; ComPtr<IUnknown> unk;
        HRESULT h = op->GetActivateResult(&hrAct, &unk);
        if (FAILED(h)) hr = h; else if (FAILED(hrAct)) hr = hrAct; else hr = unk.As(&client);
        SetEvent(ev); return S_OK;
    }
};
static ComPtr<IAudioClient> ActivateProcessLoopback(DWORD pid, HRESULT& hrOut) {
    AUDIOCLIENT_ACTIVATION_PARAMS ap = {};
    ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    ap.ProcessLoopbackParams.TargetProcessId = pid;
    ap.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv = {}; pv.vt = VT_BLOB; pv.blob.cbSize = sizeof(ap); pv.blob.pBlobData = (BYTE*)&ap;
    auto handler = Make<ActivateHandler>();
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    hrOut = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler.Get(), &op);
    if (FAILED(hrOut)) return nullptr;
    WaitForSingleObject(handler->ev, 5000);
    hrOut = handler->hr;
    return handler->client;
}

struct Ring {
    std::vector<float> buf; size_t head = 0, count = 0; int ch = 2;
    void init(size_t frames, int channels) { ch = channels; buf.assign(frames * ch, 0.f); head = 0; count = 0; }
    size_t capFrames() const { return buf.size() / ch; }
    void push(const float* src, size_t frames) {
        for (size_t i = 0; i < frames; i++) {
            if (count == capFrames()) { head = (head + 1) % capFrames(); count--; } // drop oldest
            size_t idx = ((head + count) % capFrames()) * ch;
            for (int c = 0; c < ch; c++) buf[idx + c] = src ? src[i * ch + c] : 0.f;
            count++;
        }
    }
    size_t pop(float* dst, size_t frames) { // zero-fills what is missing
        size_t got = std::min(frames, count);
        for (size_t i = 0; i < frames; i++) {
            if (i < got) { size_t idx = head * ch; for (int c = 0; c < ch; c++) dst[i * ch + c] = buf[idx + c]; head = (head + 1) % capFrames(); }
            else for (int c = 0; c < ch; c++) dst[i * ch + c] = 0.f;
        }
        count -= got; return got;
    }
    void trimTo(size_t frames) { while (count > frames) { head = (head + 1) % capFrames(); count--; } }
};

struct Capture {
    ComPtr<IAudioClient> client; ComPtr<IAudioCaptureClient> cap; bool pcm16 = false; int ch = 2; HANDLE ev = nullptr;
    void drain(Ring& ring, std::vector<float>& tmp) {
        if (!cap) return;
        UINT32 pkt = 0;
        while (SUCCEEDED(cap->GetNextPacketSize(&pkt)) && pkt > 0) {
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) ring.push(nullptr, frames);
            else if (pcm16) { tmp.resize((size_t)frames * ch); const int16_t* s = (const int16_t*)data; for (size_t i = 0; i < tmp.size(); i++) tmp[i] = s[i] / 32768.f; ring.push(tmp.data(), frames); }
            else ring.push((const float*)data, frames);
            cap->ReleaseBuffer(frames);
        }
    }
    void stop() { if (client) client->Stop(); cap.Reset(); client.Reset(); if (ev) { CloseHandle(ev); ev = nullptr; } }
};

class Engine {
public:
    std::atomic<bool> running{ false };
    std::wstring micId, renderId; DWORD excludePid = 0;
    std::wstring lastError;
    void start(const std::wstring& mic, const std::wstring& render) { micId = mic; renderId = render; lastError.clear(); running = true; th = std::thread(&Engine::run, this); }
    void stop() { running = false; if (th.joinable()) th.join(); }
private:
    std::thread th;
    WAVEFORMATEX* fmt = nullptr; int ch = 2; UINT32 rate = 48000;

    bool openLoopback(Capture& c) {
        c.stop();
        DWORD pid = FindRootProcess(g_excludeProc);
        excludePid = pid ? pid : GetCurrentProcessId();
        HRESULT hr = E_FAIL;
        c.client = ActivateProcessLoopback(excludePid, hr);
        if (FAILED(hr) || !c.client) { lastError = L"process loopback activate " + Hex(hr); Log(lastError); return false; }
        c.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = c.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 2000000, 0, fmt, nullptr);
        c.pcm16 = false;
        if (FAILED(hr)) {
            // fall back to 16-bit PCM at the same rate/channels (Initialize is once-per-client, so re-activate)
            Log(L"loopback float Initialize " + Hex(hr) + L", trying 16-bit PCM");
            WAVEFORMATEX p = {}; p.wFormatTag = WAVE_FORMAT_PCM; p.nChannels = (WORD)ch; p.nSamplesPerSec = rate; p.wBitsPerSample = 16; p.nBlockAlign = p.nChannels * 2; p.nAvgBytesPerSec = p.nBlockAlign * rate;
            c.client = ActivateProcessLoopback(excludePid, hr);
            if (FAILED(hr) || !c.client) { lastError = L"process loopback re-activate " + Hex(hr); Log(lastError); return false; }
            hr = c.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 2000000, 0, &p, nullptr);
            c.pcm16 = true;
        }
        if (FAILED(hr)) { lastError = L"loopback Initialize " + Hex(hr); Log(lastError); return false; }
        c.client->SetEventHandle(c.ev); // required with EVENTCALLBACK; we still poll on a timer
        c.ch = ch;
        if (FAILED(c.client->GetService(IID_PPV_ARGS(&c.cap))) || FAILED(c.client->Start())) { lastError = L"loopback start failed"; Log(lastError); return false; }
        Log(L"loopback capture started, excluding pid " + std::to_wstring(excludePid) + (pid ? L" (" + g_excludeProc + L")" : L" (self; " + g_excludeProc + L" not running)") + (c.pcm16 ? L" pcm16" : L" float"));
        return true;
    }
    bool openMic(Capture& c) {
        auto d = GetDevice(micId); if (!d) { lastError = L"mic device not found"; return false; }
        HRESULT hr = d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c.client);
        if (FAILED(hr)) { lastError = L"mic activate " + Hex(hr); Log(lastError); return false; }
        hr = c.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 2000000, 0, fmt, nullptr);
        if (FAILED(hr)) { lastError = L"mic Initialize " + Hex(hr); Log(lastError); return false; }
        c.pcm16 = false; c.ch = ch;
        if (FAILED(c.client->GetService(IID_PPV_ARGS(&c.cap))) || FAILED(c.client->Start())) { lastError = L"mic start failed"; return false; }
        Log(L"mic capture started: " + NameOf(micId));
        return true;
    }
    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        timeBeginPeriod(1);
        ComPtr<IAudioClient> render; ComPtr<IAudioRenderClient> rc; UINT32 rbuf = 0;
        Capture mic, loop; Ring micRing, loopRing; std::vector<float> tmp, mixA, mixB;
        bool ok = false;
        do {
            auto d = GetDevice(renderId);
            if (!d || FAILED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &render))) { lastError = L"cable playback device not found"; Log(lastError); break; }
            if (FAILED(render->GetMixFormat(&fmt))) { lastError = L"GetMixFormat failed"; break; }
            ch = fmt->nChannels; rate = fmt->nSamplesPerSec;
            bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE*)fmt)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
            if (!isFloat || fmt->wBitsPerSample != 32) { lastError = L"unexpected mix format"; Log(lastError); break; }
            HRESULT hr = render->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, fmt, nullptr);
            if (FAILED(hr)) { lastError = L"render Initialize " + Hex(hr); Log(lastError); break; }
            render->GetBufferSize(&rbuf);
            if (FAILED(render->GetService(IID_PPV_ARGS(&rc)))) { lastError = L"render service failed"; break; }
            Log(L"render to " + NameOf(renderId) + L" " + std::to_wstring(rate) + L"Hz x" + std::to_wstring(ch) + L" buffer " + std::to_wstring(rbuf));
            micRing.init(rate / 2, ch); loopRing.init(rate / 2, ch);
            if (!openMic(mic)) break;
            if (!openLoopback(loop)) break;
            // pre-fill 20 ms of silence so the first packets are not rushed
            { std::vector<float> z((size_t)rate / 50 * ch, 0.f); micRing.push(z.data(), rate / 50); loopRing.push(z.data(), rate / 50); }
            render->Start();
            ok = true;
        } while (false);
        if (ok) {
            const UINT32 target = rate * 20 / 1000;       // keep ~20 ms queued in the cable (low latency helps Discord's echo canceller)
            const UINT32 maxQueue = rate * 120 / 1000;    // never let a source lag more than 120 ms
            ULONGLONG lastCheck = GetTickCount64(), lastStat = lastCheck;
            double micE = 0, loopE = 0; size_t nE = 0;
            float env = 0.f, musicG = g_musicTargetDb != 0.f ? 1.f : g_musicGain;   // auto-level state
            while (running) {
                mic.drain(micRing, tmp); loop.drain(loopRing, tmp);
                micRing.trimTo(maxQueue); loopRing.trimTo(maxQueue);
                UINT32 pad = 0; render->GetCurrentPadding(&pad);
                if (pad < target) {
                    UINT32 want = std::min(target - pad, rbuf - pad);
                    // do not run ahead of the sources: write what they have (but at least 5 ms to avoid underrun)
                    UINT32 avail = (UINT32)std::max(micRing.count, loopRing.count);
                    UINT32 n = std::min(want, std::max(avail, (UINT32)(rate / 200)));
                    BYTE* out = nullptr;
                    if (n && SUCCEEDED(rc->GetBuffer(n, &out))) {
                        mixA.resize((size_t)n * ch); mixB.resize((size_t)n * ch);
                        micRing.pop(mixA.data(), n); loopRing.pop(mixB.data(), n);
                        float* o = (float*)out;
                        if (g_musicTargetDb != 0.f) {
                            // auto-level the music: fast attack, slow release, capped at music_gain
                            double e = 0; for (float b : mixB) e += (double)b * b; float rmsBlk = (float)sqrt(e / mixB.size());
                            env = env * 0.9f + rmsBlk * 0.1f;
                            float want = std::min(g_musicGain, powf(10.f, g_musicTargetDb / 20.f) / std::max(env, 1e-5f));
                            musicG += (want - musicG) * (want < musicG ? 0.3f : 0.01f);
                        } else musicG = g_musicGain;
                        for (size_t i = 0; i < mixA.size(); i++) {
                            float v = mixA[i] * g_micGain + mixB[i] * musicG;
                            if (g_verbose) { micE += mixA[i] * mixA[i]; loopE += mixB[i] * mixB[i]; nE++; }
                            o[i] = v > 1.f ? 1.f : (v < -1.f ? -1.f : v);
                        }
                        rc->ReleaseBuffer(n, 0);
                    }
                }
                ULONGLONG now = GetTickCount64();
                if (now - lastCheck > 2000) {
                    lastCheck = now;
                    if (ConfigChanged()) { LoadConfig(); Log(L"config reloaded"); }
                    DWORD root = FindRootProcess(g_excludeProc);
                    if ((root && root != excludePid) || (!root && excludePid != GetCurrentProcessId()) || !ProcessAlive(excludePid)) {
                        Log(L"excluded process changed, reopening loopback");
                        if (!openLoopback(loop)) Log(L"reopen failed: " + lastError);
                    }
                }
                if (g_verbose && now - lastStat > 5000 && nE) {
                    wchar_t b[160]; swprintf_s(b, L"levels mic %.1f dB  music %.1f dB (gain x%.2f)  queue mic %zu loop %zu", 10 * log10(micE / nE + 1e-12), 10 * log10(loopE / nE + 1e-12), musicG, micRing.count, loopRing.count);
                    Log(b); micE = loopE = 0; nE = 0; lastStat = now;
                }
                Sleep(4);
            }
        }
        if (render) render->Stop();
        mic.stop(); loop.stop();
        if (fmt) { CoTaskMemFree(fmt); fmt = nullptr; }
        timeEndPeriod(1);
        running = false;
        CoUninitialize();
        Log(L"engine stopped");
    }
};

// ---------- tray UI ----------
#define WM_TRAY (WM_APP + 1)
#define WM_TOGGLE (WM_APP + 2)
#define ID_TOGGLE 1
#define ID_AUTOSTART 2
#define ID_FOLDER 3
#define ID_EXIT 4
static const wchar_t* kClass = L"MusicToMicTrayWnd";
static HWND g_hwnd; static NOTIFYICONDATAW g_nid = {}; static HICON g_icoOn, g_icoOff;
static Engine g_engine; static bool g_on = false; static std::wstring g_prevMic;

static HICON MakeIcon(COLORREF fill, bool on) {
    const int S = 32; BITMAPV5HEADER bi = {}; bi.bV5Size = sizeof(bi); bi.bV5Width = S; bi.bV5Height = -S; bi.bV5Planes = 1; bi.bV5BitCount = 32; bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr; HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0); ReleaseDC(nullptr, dc);
    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    DWORD* px = (DWORD*)bits; float cx = 15.5f, cy = 15.5f, R = 15.f;
    BYTE r = GetRValue(fill), g = GetGValue(fill), b = GetBValue(fill);
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
        float a = d <= R - 0.5f ? 1.f : (d >= R + 0.5f ? 0.f : (R + 0.5f - d));
        bool ring = !on && d > R - 4.f;                 // OFF = hollow ring
        bool bar = false; int hgt[3] = { on ? 10 : 6, on ? 18 : 6, on ? 13 : 6 }; // three "equalizer" bars
        for (int k = 0; k < 3; k++) { int bx = 9 + k * 5; if (x >= bx && x < bx + 3 && y >= 23 - hgt[k] && y < 23) bar = true; }
        BYTE cr = r, cg = g, cb = b;
        if (bar) { cr = cg = cb = on ? 255 : 200; }
        if (!on && !ring && !bar) a = 0.f;
        BYTE A = (BYTE)(a * 255);
        px[y * S + x] = (A << 24) | ((cr * A / 255) << 16) | ((cg * A / 255) << 8) | (cb * A / 255);
    }
    ICONINFO ii = { TRUE, 0, 0, mask, color }; HICON h = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask); return h;
}
static void UpdateTray(const wchar_t* balloon = nullptr) {
    g_nid.hIcon = g_on ? g_icoOn : g_icoOff;
    wcscpy_s(g_nid.szTip, g_on ? L"Music to mic: ON  (click to stop)" : L"Music to mic: OFF  (click to start)");
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    if (balloon) { g_nid.uFlags |= NIF_INFO; wcscpy_s(g_nid.szInfo, balloon); wcscpy_s(g_nid.szInfoTitle, L"Music to mic"); g_nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND; }
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
static std::wstring StatePath() { return g_dir + L"state.txt"; }
static void SaveState(const std::wstring& prev) { std::wofstream f(StatePath()); f << prev; }
static std::wstring LoadState() { std::wifstream f(StatePath()); std::wstring s; std::getline(f, s); return Trim(s); }

static bool IsCable(const std::wstring& id) { return IContains(NameOf(id), L"CABLE"); }
static std::wstring PickRealMic() {
    std::wstring saved = LoadState();
    if (!saved.empty() && GetDevice(saved) && !IsCable(saved)) return saved;
    for (auto& d : ListDevices(eCapture)) if (!IContains(d.name, L"CABLE")) return d.id;
    return L"";
}
// The heavy work (device probing, engine start, default-device switch) runs on a worker thread so the
// tray icon flips instantly. WM_RESULT brings the outcome back to the UI thread.
#define WM_RESULT (WM_APP + 3)
static std::thread g_worker;
static std::atomic<bool> g_busy{ false };
static void PostResult(bool ok, const std::wstring& msg) { PostMessageW(g_hwnd, WM_RESULT, ok ? 1 : 0, (LPARAM)new std::wstring(msg)); }

static void DoOn() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring cableOut = FindDevice(eCapture, g_cableOut), cableIn = FindDevice(eRender, g_cableIn);
    if (cableOut.empty() || cableIn.empty()) {
        Log(L"VB-CABLE not found");
        PostResult(false, L"VB-CABLE is not installed (or needs a reboot). Run install-cable.cmd in the tool folder as administrator.");
        CoUninitialize(); return;
    }
    std::wstring cur = DefaultCapture();
    g_prevMic = (cur.empty() || IsCable(cur)) ? PickRealMic() : cur;
    if (g_prevMic.empty()) { PostResult(false, L"No real microphone found."); CoUninitialize(); return; }
    SaveState(g_prevMic);
    UnmuteFull(cableIn); UnmuteFull(cableOut);
    // voice source: the configured mic (e.g. a noise-cancelling virtual mic) if present, else the previous default
    std::wstring voice = g_micName.empty() ? L"" : FindDevice(eCapture, g_micName);
    if (voice.empty() || IsCable(voice)) voice = g_prevMic;
    else {
        UnmuteFull(voice);
        if (voice != g_prevMic) {
            // a virtual mic whose app is not running gives digital zeros: fall back to the real mic in that case
            const float kSilent = 1e-4f; // about -80 dB: below this a mic is considered dead
            float pv = ProbeMic(voice, 1000), pp = pv > kSilent ? 1.f : ProbeMic(g_prevMic, 500);
            if (pv <= kSilent && pp > kSilent) { Log(L"configured mic " + NameOf(voice) + L" is silent, using " + NameOf(g_prevMic)); voice = g_prevMic; }
        }
    }
    g_engine.start(voice, cableIn);
    for (int i = 0; i < 150 && g_engine.running && g_engine.lastError.empty(); i++) Sleep(20);
    if (!g_engine.running || !g_engine.lastError.empty()) { std::wstring e = g_engine.lastError; g_engine.stop(); PostResult(false, L"Could not start: " + e); CoUninitialize(); return; }
    if (!SetDefaultDevice(cableOut)) Log(L"warning: could not set default mic to cable");
    Log(L"ON: voice=" + NameOf(voice) + L" -> cable; default mic switched (was " + NameOf(g_prevMic) + L")");
    PostResult(true, L"ON - everything you hear (except Discord) now goes into your mic.");
    CoUninitialize();
}
static void DoOff() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!g_prevMic.empty()) SetDefaultDevice(g_prevMic);
    g_engine.stop();
    Log(L"OFF: default mic restored to " + NameOf(g_prevMic));
    CoUninitialize();
}
static void JoinWorker() { if (g_worker.joinable()) g_worker.join(); g_busy = false; }
static void TurnOn() {
    if (g_on || g_busy) return;
    JoinWorker();
    g_on = true; g_busy = true; UpdateTray();          // icon flips immediately
    g_worker = std::thread(DoOn);
}
static void TurnOff(bool quiet = false) {
    if (!g_on || g_busy) return;
    JoinWorker();
    g_on = false; g_busy = true; if (!quiet) UpdateTray(L"OFF - microphone back to normal."); else UpdateTray();
    g_worker = std::thread([] { DoOff(); PostResult(true, L""); });
}
static void ShutdownSync() {                           // exit / logoff: finish any pending work, restore the mic
    JoinWorker();
    if (g_on) { g_on = false; DoOff(); }
}
static void RestoreIfLeftover() {
    std::wstring cur = DefaultCapture();
    if (!cur.empty() && IsCable(cur)) { std::wstring prev = PickRealMic(); if (!prev.empty()) { SetDefaultDevice(prev); Log(L"restored leftover default mic to " + NameOf(prev)); } }
}
static std::wstring StartupLnk() { wchar_t p[MAX_PATH]; SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, 0, p); return std::wstring(p) + L"\\Music to mic.lnk"; }
static bool AutostartOn() { return GetFileAttributesW(StartupLnk().c_str()) != INVALID_FILE_ATTRIBUTES; }
static void SetAutostart(bool on) {
    if (!on) { DeleteFileW(StartupLnk().c_str()); return; }
    ComPtr<IShellLinkW> sl; if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    sl->SetPath(g_exePath.c_str()); sl->SetWorkingDirectory(g_dir.c_str()); sl->SetDescription(L"Music to mic tray toggle");
    ComPtr<IPersistFile> pf; if (SUCCEEDED(sl.As(&pf))) pf->Save(StartupLnk().c_str(), TRUE);
}
// Windows 11 hides new tray icons in the overflow; ask for this one to be shown on the bar.
static bool PromoteTrayIcon() {
    HKEY root; if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS) return false;
    bool done = false; wchar_t name[256];
    for (DWORD i = 0; ; i++) {
        DWORD len = 256; if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY k; if (RegOpenKeyExW(root, name, 0, KEY_READ | KEY_SET_VALUE, &k) != ERROR_SUCCESS) continue;
        wchar_t path[1024]; DWORD sz = sizeof(path), type = 0;
        if (RegQueryValueExW(k, L"ExecutablePath", nullptr, &type, (BYTE*)path, &sz) == ERROR_SUCCESS && IEquals(path, g_exePath)) {
            DWORD one = 1, cur = 0; DWORD csz = sizeof(cur);
            if (RegQueryValueExW(k, L"IsPromoted", nullptr, nullptr, (BYTE*)&cur, &csz) != ERROR_SUCCESS || cur != 1) RegSetValueExW(k, L"IsPromoted", 0, REG_DWORD, (BYTE*)&one, sizeof(one));
            done = true;
        }
        RegCloseKey(k);
    }
    RegCloseKey(root); return done;
}
static void ShowMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_on ? MF_CHECKED : 0), ID_TOGGLE, g_on ? L"Music to mic is ON  (click to stop)" : L"Start music to mic");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (AutostartOn() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_STRING, ID_FOLDER, L"Open folder (config, log)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT p; GetCursorPos(&p); SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0); DestroyMenu(m);
}
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) { if (g_on) TurnOff(); else TurnOn(); }
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowMenu();
        return 0;
    case WM_TOGGLE: if (g_on) TurnOff(); else TurnOn(); return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TOGGLE: if (g_on) TurnOff(); else TurnOn(); break;
        case ID_AUTOSTART: SetAutostart(!AutostartOn()); break;
        case ID_FOLDER: ShellExecuteW(nullptr, L"open", g_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_EXIT: DestroyWindow(h); break;
        }
        return 0;
    case WM_RESULT: {
        std::wstring* msg = (std::wstring*)lp; g_busy = false;
        if (!wp) g_on = false;
        UpdateTray(msg && !msg->empty() ? msg->c_str() : nullptr); delete msg; return 0;
    }
    case WM_QUERYENDSESSION: ShutdownSync(); return TRUE;
    case WM_DESTROY: ShutdownSync(); Shell_NotifyIconW(NIM_DELETE, &g_nid); PostQuitMessage(0); return 0;
    default:
        if (msg == taskbarCreated) { Shell_NotifyIconW(NIM_ADD, &g_nid); Shell_NotifyIconW(NIM_SETVERSION, &g_nid); UpdateTray(); }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmd, int) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH); g_exePath = exe;
    g_dir = g_exePath.substr(0, g_exePath.find_last_of(L"\\/") + 1);
    LoadConfig();
    // keep the log small
    { WIN32_FILE_ATTRIBUTE_DATA fa; if (GetFileAttributesExW((g_dir + L"music-to-mic.log").c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 200000) DeleteFileW((g_dir + L"music-to-mic.log").c_str()); }
    // single instance: launching a second copy just toggles the running one
    HWND existing = FindWindowW(kClass, nullptr);
    if (existing) { PostMessageW(existing, WM_TOGGLE, 0, 0); return 0; }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    RestoreIfLeftover();
    WNDCLASSW wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = kClass; RegisterClassW(&wc);
    g_hwnd = CreateWindowW(kClass, L"Music to mic", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hi, nullptr);
    g_icoOn = MakeIcon(RGB(46, 204, 113), true); g_icoOff = MakeIcon(RGB(150, 150, 150), false);
    g_nid.cbSize = sizeof(g_nid); g_nid.hWnd = g_hwnd; g_nid.uID = 1; g_nid.uCallbackMessage = WM_TRAY; g_nid.uVersion = NOTIFYICON_VERSION_4;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE; g_nid.hIcon = g_icoOff; wcscpy_s(g_nid.szTip, L"Music to mic: OFF  (click to start)");
    Shell_NotifyIconW(NIM_ADD, &g_nid); Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    if (!PromoteTrayIcon()) { Sleep(800); if (PromoteTrayIcon()) { Shell_NotifyIconW(NIM_DELETE, &g_nid); Shell_NotifyIconW(NIM_ADD, &g_nid); Shell_NotifyIconW(NIM_SETVERSION, &g_nid); } }
    if (!AutostartOn() && wcsstr(cmd, L"--no-autostart") == nullptr) SetAutostart(true);
    Log(L"started");
    if (wcsstr(cmd, L"--on")) TurnOn();
    MSG m; while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    CoUninitialize();
    return 0;
}
