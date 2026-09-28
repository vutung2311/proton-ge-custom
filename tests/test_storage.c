/* Storage and volume queries as the game issues them during the slow load phase (a game thread calls
 * IOCTL_STORAGE_QUERY_PROPERTY and IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS about once a second).
 * Engines use the seek-penalty property to tell SSDs from HDDs and pick a streaming strategy, so a
 * different answer between Wine builds could change how the game streams.
 *
 *   test_storage.exe [--log file] [target ...]
 *                                     default targets: \\.\D: \\.\C: \\.\Z: \\.\PhysicalDrive0
 *                                     and the directories D:\ and D:\Games\wwm
 * For every target: open result, then each query's status (Win32 error), bytes returned, the fields
 * that matter, and how long the call took. Output on stdout.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -Wall -o test_storage.exe test_storage.c
 */
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <string.h>

#ifndef StorageDeviceSeekPenaltyProperty
#define StorageDeviceSeekPenaltyProperty 7
#endif
#ifndef StorageDeviceTrimProperty
#define StorageDeviceTrimProperty 8
#endif
#define StorageDeviceMediumProductType_ 15

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

static BOOL ioctl(HANDLE h, DWORD code, void *in, DWORD inlen, void *out, DWORD outlen, DWORD *got, DWORD *err, double *ms)
{
    double t = now_ms();
    BOOL ok = DeviceIoControl(h, code, in, inlen, out, outlen, got, NULL);
    *ms = now_ms() - t;
    *err = ok ? 0 : GetLastError();
    return ok;
}

static void query_property(HANDLE h, int id, const char *name)
{
    STORAGE_PROPERTY_QUERY q = {0};
    BYTE out[1024] = {0};
    DWORD got = 0, err;
    double ms;
    BOOL ok;

    q.PropertyId = id;
    q.QueryType = PropertyStandardQuery;
    ok = ioctl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), out, sizeof(out), &got, &err, &ms);
    printf("    QUERY_PROPERTY %-16s err %-4lu bytes %-4lu %6.3f ms", name, err, got, ms);
    if (ok && id == StorageDeviceSeekPenaltyProperty && got >= sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR))
        printf("  IncursSeekPenalty=%d", ((DEVICE_SEEK_PENALTY_DESCRIPTOR *)out)->IncursSeekPenalty);
    else if (ok && id == StorageDeviceTrimProperty && got >= sizeof(DEVICE_TRIM_DESCRIPTOR))
        printf("  TrimEnabled=%d", ((DEVICE_TRIM_DESCRIPTOR *)out)->TrimEnabled);
    else if (ok && id == StorageDeviceProperty && got >= sizeof(STORAGE_DEVICE_DESCRIPTOR))
    {
        STORAGE_DEVICE_DESCRIPTOR *d = (STORAGE_DEVICE_DESCRIPTOR *)out;
        printf("  BusType=%d Removable=%d DeviceType=%d", d->BusType, d->RemovableMedia, d->DeviceType);
        if (d->VendorIdOffset && d->VendorIdOffset < got) printf(" vendor='%s'", (char *)out + d->VendorIdOffset);
        if (d->ProductIdOffset && d->ProductIdOffset < got) printf(" product='%s'", (char *)out + d->ProductIdOffset);
    }
    else if (ok && id == StorageAdapterProperty && got >= sizeof(STORAGE_ADAPTER_DESCRIPTOR))
    {
        STORAGE_ADAPTER_DESCRIPTOR *d = (STORAGE_ADAPTER_DESCRIPTOR *)out;
        printf("  BusType=%d MaxTransfer=%lu AlignMask=%lu", d->BusType, d->MaximumTransferLength, d->AlignmentMask);
    }
    else if (ok && id == StorageDeviceMediumProductType_ && got >= 12)
        printf("  MediumProductType=%lu", ((DWORD *)out)[2]);
    printf("\n");
}

static void query(const char *target)
{
    DWORD attrs = GetFileAttributesA(target);
    DWORD flags = (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) ? FILE_FLAG_BACKUP_SEMANTICS : 0;
    HANDLE h = CreateFileA(target, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, flags, NULL);
    BYTE out[1024];
    DWORD got, err;
    double ms;

    printf("%s\n", target);
    if (h == INVALID_HANDLE_VALUE)
    {
        printf("    open failed, error %lu\n", GetLastError());
        return;
    }
    query_property(h, StorageDeviceProperty, "Device");
    query_property(h, StorageAdapterProperty, "Adapter");
    query_property(h, StorageDeviceSeekPenaltyProperty, "SeekPenalty");
    query_property(h, StorageDeviceTrimProperty, "Trim");
    query_property(h, StorageDeviceMediumProductType_, "MediumProduct");

    got = 0;
    if (ioctl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, out, sizeof(out), &got, &err, &ms))
    {
        VOLUME_DISK_EXTENTS *e = (VOLUME_DISK_EXTENTS *)out;
        printf("    VOLUME_DISK_EXTENTS          err 0    bytes %-4lu %6.3f ms  extents=%lu", got, ms, e->NumberOfDiskExtents);
        if (e->NumberOfDiskExtents)
            printf(" disk=%lu offset=%lld length=%lld", e->Extents[0].DiskNumber,
                   e->Extents[0].StartingOffset.QuadPart, e->Extents[0].ExtentLength.QuadPart);
        printf("\n");
    }
    else printf("    VOLUME_DISK_EXTENTS          err %-4lu bytes %-4lu %6.3f ms\n", err, got, ms);

    got = 0;
    if (ioctl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, out, sizeof(out), &got, &err, &ms))
    {
        STORAGE_DEVICE_NUMBER *n = (STORAGE_DEVICE_NUMBER *)out;
        printf("    GET_DEVICE_NUMBER            err 0    bytes %-4lu %6.3f ms  type=%lu number=%lu partition=%lu\n",
               got, ms, n->DeviceType, n->DeviceNumber, n->PartitionNumber);
    }
    else printf("    GET_DEVICE_NUMBER            err %-4lu bytes %-4lu %6.3f ms\n", err, got, ms);

    got = 0;
    if (ioctl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, out, sizeof(out), &got, &err, &ms))
    {
        DISK_GEOMETRY_EX *g = (DISK_GEOMETRY_EX *)out;
        printf("    DISK_GET_DRIVE_GEOMETRY_EX   err 0    bytes %-4lu %6.3f ms  media=%d bytes/sector=%lu size=%lld\n",
               got, ms, g->Geometry.MediaType, g->Geometry.BytesPerSector, g->DiskSize.QuadPart);
    }
    else printf("    DISK_GET_DRIVE_GEOMETRY_EX   err %-4lu bytes %-4lu %6.3f ms\n", err, got, ms);
    CloseHandle(h);
}

int main(int argc, char **argv)
{
    static const char *defaults[] = { "\\\\.\\D:", "\\\\.\\C:", "\\\\.\\Z:", "\\\\.\\PhysicalDrive0",
                                      "D:\\", "D:\\Games\\wwm" };
    static const char *roots[] = { "C:\\", "D:\\", "Z:\\" };
    char fs[64], vol[MAX_PATH];
    int i;

    for (i = 0; i < 3; i++)
    {
        const char *root = roots[i];
        ULARGE_INTEGER freeb = {{0}}, total = {{0}};
        fs[0] = vol[0] = 0;
        GetVolumeInformationA(root, vol, sizeof(vol), NULL, NULL, NULL, fs, sizeof(fs));
        GetDiskFreeSpaceExA(root, &freeb, &total, NULL);
        printf("%s drive type %u, fs '%s', label '%s', %llu of %llu MB free\n", root, GetDriveTypeA(root), fs, vol,
               freeb.QuadPart >> 20, total.QuadPart >> 20);
    }
    /* --log <file>: write there instead of stdout (proton run does not pass stdout through) */
    if (argc > 2 && !strcmp(argv[1], "--log"))
    {
        if (!freopen(argv[2], "w", stdout)) return 1;
        argv += 2;
        argc -= 2;
    }
    if (argc > 1)
        for (i = 1; i < argc; i++) query(argv[i]);
    else
        for (i = 0; i < (int)(sizeof(defaults) / sizeof(defaults[0])); i++) query(defaults[i]);
    return 0;
}
