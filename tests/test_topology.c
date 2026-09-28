/* CPU topology as a game sees it when it sizes and pins its thread pools:
 * GetLogicalProcessorInformationEx (all relations), GetLogicalProcessorInformation,
 * GetSystemCpuSetInformation, active processor counts and the process affinity mask.
 * Output to C:\test_topology.log; run under each runner by tests/run_topology.sh. */
#include <windows.h>
#include <stdio.h>

static void mask_str(char *buf, KAFFINITY m)
{
    int i, n = 0;
    buf[0] = 0;
    for (i = 0; i < 64; i++)
        if (m & ((KAFFINITY)1 << i)) n += sprintf(buf + n, "%s%d", n ? "," : "", i);
}

int main(void)
{
    FILE *f = fopen("C:\\test_topology.log", "w");
    DWORD len = 0, off;
    char *buf, ms[256];
    DWORD_PTR pmask, smask;
    SYSTEM_INFO si;
    if (!f) return 1;

    GetSystemInfo(&si);
    GetProcessAffinityMask(GetCurrentProcess(), &pmask, &smask);
    mask_str(ms, pmask);
    fprintf(f, "dwNumberOfProcessors %lu  ActiveProcessorCount %lu  process mask {%s}", si.dwNumberOfProcessors,
            GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), ms);
    mask_str(ms, smask);
    fprintf(f, "  system mask {%s}\n", ms);

    GetLogicalProcessorInformationEx(RelationAll, NULL, &len);
    buf = malloc(len);
    if (!GetLogicalProcessorInformationEx(RelationAll, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len))
        fprintf(f, "GetLogicalProcessorInformationEx failed %lu\n", GetLastError());
    else
    {
        for (off = 0; off < len; )
        {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX p = (void *)(buf + off);
            switch (p->Relationship)
            {
            case RelationProcessorCore:
                mask_str(ms, p->Processor.GroupMask[0].Mask);
                fprintf(f, "core     smt=%d eff=%d {%s}\n", p->Processor.Flags, p->Processor.EfficiencyClass, ms);
                break;
            case RelationProcessorPackage:
                mask_str(ms, p->Processor.GroupMask[0].Mask);
                fprintf(f, "package  {%s}\n", ms);
                break;
            case RelationCache:
                mask_str(ms, p->Cache.GroupMask.Mask);
                fprintf(f, "cache    L%d type %d size %lu KB line %d assoc %d {%s}\n", p->Cache.Level, p->Cache.Type,
                        p->Cache.CacheSize / 1024, p->Cache.LineSize, p->Cache.Associativity, ms);
                break;
            case RelationNumaNode:
                mask_str(ms, p->NumaNode.GroupMask.Mask);
                fprintf(f, "numa     node %lu {%s}\n", p->NumaNode.NodeNumber, ms);
                break;
            case RelationGroup:
                mask_str(ms, p->Group.GroupInfo[0].ActiveProcessorMask);
                fprintf(f, "group    max %d active %d {%s}\n", p->Group.MaximumGroupCount,
                        p->Group.GroupInfo[0].ActiveProcessorCount, ms);
                break;
            default:
                fprintf(f, "relation %d size %lu\n", p->Relationship, p->Size);
            }
            off += p->Size;
        }
    }
    free(buf);

    len = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    buf = malloc(len ? len : 1);
    if (len && GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0))
    {
        for (off = 0; off < len; )
        {
            PSYSTEM_CPU_SET_INFORMATION c = (void *)(buf + off);
            fprintf(f, "cpuset   id %lu lp %d core %d llc %d numa %d eff %d parked %d alloc %d\n", c->CpuSet.Id,
                    c->CpuSet.LogicalProcessorIndex, c->CpuSet.CoreIndex, c->CpuSet.LastLevelCacheIndex,
                    c->CpuSet.NumaNodeIndex, c->CpuSet.EfficiencyClass, c->CpuSet.Parked, c->CpuSet.Allocated);
            off += c->Size;
        }
    }
    else fprintf(f, "GetSystemCpuSetInformation: len %lu err %lu\n", len, GetLastError());
    free(buf);
    fclose(f);
    return 0;
}
