// HoneyCord Memory-Telemetrie — Implementierung. Siehe Header.
#define _CRT_SECURE_NO_WARNINGS 1
#include "honeycord_mem_telemetry.h"

#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace honeycord {
namespace {

// Log-Rotation wie in honeycord_wasapi_loopback.cc: > 1 MB -> .old (ersetzt
// Vorgaenger), Archiv aelter 14 Tage -> geloescht.
void RotateLogIfNeededA(const char* path) {
  char old_path[MAX_PATH + 8];
  std::snprintf(old_path, sizeof(old_path), "%s.old", path);
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (GetFileAttributesExA(old_path, GetFileExInfoStandard, &fad)) {
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    ULARGE_INTEGER now_u{}, old_u{};
    now_u.LowPart = now_ft.dwLowDateTime;
    now_u.HighPart = now_ft.dwHighDateTime;
    old_u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    old_u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    const unsigned long long k14Days100ns = 14ULL * 24 * 3600 * 10000000ULL;
    if (now_u.QuadPart > old_u.QuadPart &&
        now_u.QuadPart - old_u.QuadPart > k14Days100ns) {
      DeleteFileA(old_path);
    }
  }
  if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
    const unsigned long long size =
        (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) |
        fad.nFileSizeLow;
    if (size > 1024ULL * 1024ULL) {
      MoveFileExA(path, old_path, MOVEFILE_REPLACE_EXISTING);
    }
  }
}

void LogLine() {
  char path[MAX_PATH];
  DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  std::strncat(path, "\\HoneyCord", MAX_PATH - n - 1);
  CreateDirectoryA(path, nullptr);
  std::strncat(path, "\\mem.log", MAX_PATH - std::strlen(path) - 1);

  static bool rotated = false;
  if (!rotated) {
    RotateLogIfNeededA(path);
    rotated = true;
  }

  // Prozess-RAM + Commit.
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  GetProcessMemoryInfo(GetCurrentProcess(),
                       reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                       sizeof(pmc));
  // Handle-/GDI-/USER-Objekte — klassische Leak-Zaehler.
  DWORD handles = 0;
  GetProcessHandleCount(GetCurrentProcess(), &handles);
  DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  DWORD usr = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);

  // VRAM: lokales Segment des ersten Hardware-Adapters (Budget vs. Usage).
  unsigned long long vram_usage = 0, vram_budget = 0;
  {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory1> fac;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) {
      ComPtr<IDXGIAdapter1> a;
      for (UINT i = 0; fac->EnumAdapters1(i, &a) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        ComPtr<IDXGIAdapter3> a3;
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(a.As(&a3))) {
          DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
          if (SUCCEEDED(a3->QueryVideoMemoryInfo(
                  0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) {
            vram_usage = mi.CurrentUsage;
            vram_budget = mi.Budget;
          }
          break;
        }
        a.Reset();
      }
    }
  }

  SYSTEMTIME st;
  GetLocalTime(&st);
  if (FILE* f = std::fopen(path, "a")) {
    std::fprintf(f,
                 "[mem %02d:%02d:%02d] ws=%lluMB priv=%lluMB commit=%lluMB "
                 "handles=%lu gdi=%lu user=%lu vram=%llu/%lluMB\n",
                 st.wHour, st.wMinute, st.wSecond,
                 (unsigned long long)(pmc.WorkingSetSize >> 20),
                 (unsigned long long)(pmc.PrivateUsage >> 20),
                 (unsigned long long)(pmc.PagefileUsage >> 20),
                 (unsigned long)handles, (unsigned long)gdi,
                 (unsigned long)usr, vram_usage >> 20, vram_budget >> 20);
    std::fclose(f);
  }
}

}  // namespace

void StartMemTelemetryOnce() {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  std::thread([] {
    // Erste Zeile sofort (Baseline), danach im Minutentakt.
    LogLine();
    for (;;) {
      Sleep(60 * 1000);
      LogLine();
    }
  }).detach();
}

}  // namespace honeycord
