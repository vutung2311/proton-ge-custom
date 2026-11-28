/* Print what a game sees when it sizes its streaming budget from system memory.
 * Built with mingw and run under each runner by tests/run_memstatus.sh. */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>

int main(void)
{
    MEMORYSTATUSEX ms = { sizeof(ms) };
    ULONGLONG kb = 0;
    SYSTEM_INFO si;
    PROCESS_MEMORY_COUNTERS_EX pmc = { sizeof(pmc) };
    HMODULE psapi = LoadLibraryA("psapi.dll");
    BOOL (WINAPI *pGetProcessMemoryInfo)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD) =
        psapi ? (void *)GetProcAddress(psapi, "GetProcessMemoryInfo") : NULL;
    FILE *f = fopen("C:\\test_memstatus.log", "w");
    if (!f) return 1;

    GlobalMemoryStatusEx(&ms);
    GetPhysicallyInstalledSystemMemory(&kb);
    GetSystemInfo(&si);
    fprintf(f, "MemoryLoad            %lu %%\n", ms.dwMemoryLoad);
    fprintf(f, "TotalPhys             %llu MB\n", ms.ullTotalPhys >> 20);
    fprintf(f, "AvailPhys             %llu MB\n", ms.ullAvailPhys >> 20);
    fprintf(f, "TotalPageFile         %llu MB\n", ms.ullTotalPageFile >> 20);
    fprintf(f, "AvailPageFile         %llu MB\n", ms.ullAvailPageFile >> 20);
    fprintf(f, "TotalVirtual          %llu MB\n", ms.ullTotalVirtual >> 20);
    fprintf(f, "AvailVirtual          %llu MB\n", ms.ullAvailVirtual >> 20);
    fprintf(f, "PhysicallyInstalled   %llu MB\n", kb >> 10);
    fprintf(f, "Processors            %lu\n", si.dwNumberOfProcessors);
    fprintf(f, "PageSize/Granularity  %lu / %lu\n", si.dwPageSize, si.dwAllocationGranularity);
    fprintf(f, "MinMaxAppAddr         %p / %p\n", si.lpMinimumApplicationAddress, si.lpMaximumApplicationAddress);
    if (pGetProcessMemoryInfo && pGetProcessMemoryInfo(GetCurrentProcess(), (PPROCESS_MEMORY_COUNTERS)&pmc, sizeof(pmc)))
        fprintf(f, "WorkingSet/Private    %llu / %llu MB\n",
                (unsigned long long)pmc.WorkingSetSize >> 20, (unsigned long long)pmc.PrivateUsage >> 20);
    fclose(f);
    return 0;
}
