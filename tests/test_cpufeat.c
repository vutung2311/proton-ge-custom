/* What a game sees when it picks vectorized code paths: IsProcessorFeaturePresent,
 * GetEnabledXStateFeatures, XGETBV and CPUID, plus the context XState size. Output to
 * C:\test_cpufeat.log; run under each runner by tests/run_cpufeat.sh. */
#include <windows.h>
#include <intrin.h>
#include <stdio.h>

typedef DWORD64 (WINAPI *pGetEnabledXStateFeatures_t)(void);

int main(void)
{
    FILE *f = fopen("C:\\test_cpufeat.log", "w");
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    pGetEnabledXStateFeatures_t pGetEnabledXStateFeatures = k32 ? (void *)GetProcAddress(k32, "GetEnabledXStateFeatures") : NULL;
    int r[4];
    static const struct { int id; const char *name; } pf[] = {
        { 6, "PF_XMMI_INSTRUCTIONS_AVAILABLE (SSE)" }, { 10, "PF_XMMI64 (SSE2)" }, { 13, "PF_SSE3" },
        { 36, "PF_SSSE3" }, { 37, "PF_SSE4_1" }, { 38, "PF_SSE4_2" }, { 39, "PF_AVX" },
        { 40, "PF_AVX2" }, { 41, "PF_AVX512F" }, { 12, "PF_NX_ENABLED" }, { 17, "PF_XSAVE_ENABLED" },
        { 42, "PF_ERMS_AVAILABLE" }, { 43, "PF_ARM_V82_DP" },
    };
    unsigned i;
    if (!f) return 1;
    for (i = 0; i < sizeof(pf) / sizeof(pf[0]); i++)
        fprintf(f, "IsProcessorFeaturePresent(%2d) %-40s %d\n", pf[i].id, pf[i].name, IsProcessorFeaturePresent(pf[i].id));
    fprintf(f, "GetEnabledXStateFeatures        %s%#llx\n", pGetEnabledXStateFeatures ? "" : "(missing) ",
            pGetEnabledXStateFeatures ? (unsigned long long)pGetEnabledXStateFeatures() : 0ull);
    __cpuid(r, 1);
    fprintf(f, "cpuid.1 ecx=%#x (OSXSAVE bit27=%d AVX bit28=%d)\n", r[2], (r[2] >> 27) & 1, (r[2] >> 28) & 1);
    __cpuidex(r, 7, 0);
    fprintf(f, "cpuid.7 ebx=%#x (AVX2 bit5=%d AVX512F bit16=%d) ecx=%#x edx=%#x\n", r[1], (r[1] >> 5) & 1, (r[1] >> 16) & 1, r[2], r[3]);
    if ((r[2] >> 27) & 1 || 1)
    {
        unsigned long long x = _xgetbv(0);
        fprintf(f, "XGETBV(0) XCR0=%#llx (SSE=%d AVX=%d opmask=%d zmm_hi256=%d hi16_zmm=%d)\n", x,
                (int)(x >> 1) & 1, (int)(x >> 2) & 1, (int)(x >> 5) & 1, (int)(x >> 6) & 1, (int)(x >> 7) & 1);
    }
    {
        DWORD len = 0;
        BOOL (WINAPI *pInitializeContext)(PVOID, DWORD, PCONTEXT *, PDWORD) = k32 ? (void *)GetProcAddress(k32, "InitializeContext") : NULL;
        if (pInitializeContext)
        {
            pInitializeContext(NULL, CONTEXT_ALL | 0x40 /* CONTEXT_XSTATE */, NULL, &len);
            fprintf(f, "InitializeContext(CONTEXT_XSTATE) size %lu (err %lu)\n", len, GetLastError());
        }
    }
    fclose(f);
    return 0;
}
