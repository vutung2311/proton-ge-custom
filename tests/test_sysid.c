/* System identity as environment/VM-detection code sees it: SMBIOS (GetSystemFirmwareTable
 * 'RSMB') types 0/1/2/3/4, ACPI table list, BIOS registry keys, and the Wine version.
 * Output to C:\test_sysid.log; run under each runner by tests/run_sysid.sh. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static FILE *f;

static const char *smb_str(const BYTE *hdr, BYTE idx)
{
    const char *s = (const char *)hdr + hdr[1];
    if (!idx) return "";
    while (--idx && *s) s += strlen(s) + 1;
    return *s ? s : "(missing)";
}

static void dump_smbios(void)
{
    DWORD len = GetSystemFirmwareTable('RSMB', 0, NULL, 0);
    BYTE *buf, *p, *end;
    if (!len) { fprintf(f, "SMBIOS: none (err %lu)\n", GetLastError()); return; }
    buf = malloc(len);
    GetSystemFirmwareTable('RSMB', 0, buf, len);
    fprintf(f, "SMBIOS: %lu bytes, version %u.%u\n", len, buf[1], buf[2]);
    p = buf + 8;
    end = buf + len;
    while (p + 4 <= end)
    {
        BYTE type = p[0], hlen = p[1];
        const BYTE *next = p + hlen;
        if (hlen < 4 || next > end) break;
        switch (type)
        {
        case 0: fprintf(f, "  bios     vendor '%s' version '%s' date '%s'\n", smb_str(p, p[4]), smb_str(p, p[5]), smb_str(p, p[8])); break;
        case 1: fprintf(f, "  system   manufacturer '%s' product '%s' version '%s' serial '%s' family '%s'\n",
                        smb_str(p, p[4]), smb_str(p, p[5]), smb_str(p, p[6]), smb_str(p, p[7]),
                        hlen > 0x1a ? smb_str(p, p[0x1a]) : ""); break;
        case 2: fprintf(f, "  board    manufacturer '%s' product '%s' version '%s'\n", smb_str(p, p[4]), smb_str(p, p[5]), smb_str(p, p[6])); break;
        case 3: fprintf(f, "  chassis  manufacturer '%s' type %u\n", smb_str(p, p[4]), p[5]); break;
        case 4: fprintf(f, "  cpu      socket '%s' manufacturer '%s' version '%s'\n", smb_str(p, p[4]), smb_str(p, p[7]), smb_str(p, p[0x10])); break;
        default: break;
        }
        while (next + 1 < end && (next[0] || next[1])) next++;
        p = (BYTE *)next + 2;
        if (type == 127) break;
    }
    free(buf);
}

static void dump_acpi(void)
{
    DWORD len = EnumSystemFirmwareTables('ACPI', NULL, 0), i;
    DWORD *ids;
    if (!len) { fprintf(f, "ACPI tables: none (err %lu)\n", GetLastError()); return; }
    ids = malloc(len);
    EnumSystemFirmwareTables('ACPI', ids, len);
    fprintf(f, "ACPI tables:");
    for (i = 0; i < len / 4; i++) fprintf(f, " %.4s", (char *)&ids[i]);
    fprintf(f, "\n");
    free(ids);
}

static void dump_reg(const char *path)
{
    HKEY k;
    DWORD i, nlen, dlen, type;
    char name[256], data[512];
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k)) { fprintf(f, "HKLM\\%s: absent\n", path); return; }
    fprintf(f, "HKLM\\%s:\n", path);
    for (i = 0; ; i++)
    {
        nlen = sizeof(name); dlen = sizeof(data) - 1;
        if (RegEnumValueA(k, i, name, &nlen, NULL, &type, (BYTE *)data, &dlen)) break;
        data[dlen] = 0;
        if (type == REG_SZ || type == REG_EXPAND_SZ) fprintf(f, "  %s = '%s'\n", name, data);
        else if (type == REG_MULTI_SZ) fprintf(f, "  %s = '%s' (multi)\n", name, data);
        else if (type == REG_DWORD) fprintf(f, "  %s = %#lx\n", name, *(DWORD *)data);
        else fprintf(f, "  %s = <type %lu, %lu bytes>\n", name, type, dlen);
    }
    RegCloseKey(k);
}

int main(void)
{
    const char *(CDECL *pwine_get_version)(void);
    const char *(CDECL *pwine_get_build_id)(void);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    char name[256];
    DWORD n = sizeof(name);

    f = fopen("C:\\test_sysid.log", "w");
    if (!f) return 1;
    pwine_get_version = (void *)GetProcAddress(ntdll, "wine_get_version");
    pwine_get_build_id = (void *)GetProcAddress(ntdll, "wine_get_build_id");
    fprintf(f, "wine_get_version '%s' build '%s'\n", pwine_get_version ? pwine_get_version() : "(none)",
            pwine_get_build_id ? pwine_get_build_id() : "(none)");
    GetComputerNameA(name, &n);
    fprintf(f, "computer name '%s'\n", name);
    dump_smbios();
    dump_acpi();
    dump_reg("HARDWARE\\DESCRIPTION\\System\\BIOS");
    dump_reg("HARDWARE\\DESCRIPTION\\System");
    dump_reg("HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0");
    dump_reg("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion");
    fclose(f);
    return 0;
}
