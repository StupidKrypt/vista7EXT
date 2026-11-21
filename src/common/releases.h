/* releases.h - Windows 10 and 11 releases, as those Windows show them in
 * HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion: DisplayVersion,
 * ReleaseId, the build lab (BuildLab/BuildLabEx) and an update revision
 * (UBR) of that release. One table for everything that tells a program
 * another version: common.c (the tools, the DLLs) and vxload (ntdll only, so
 * this is data in a header rather than a function). lab NULL: not known for
 * that release; a generic one is made up. */
#ifndef VX_RELEASES_H
#define VX_RELEASES_H

typedef struct { ULONG build; const WCHAR *disp, *rel, *lab; ULONG ubr; } vx_release;

static const vx_release vx_releases[] = {
    { 10240, L"1507", L"1507", NULL, 0 },
    { 10586, L"1511", L"1511", NULL, 0 },
    { 14393, L"1607", L"1607", NULL, 0 },
    { 15063, L"1703", L"1703", NULL, 0 },
    { 16299, L"1709", L"1709", NULL, 0 },
    { 17134, L"1803", L"1803", NULL, 0 },
    { 17763, L"1809", L"1809", NULL, 0 },
    { 18362, L"1903", L"1903", NULL, 0 },
    { 18363, L"1909", L"1909", NULL, 0 },
    { 19041, L"2004", L"2004", L"19041.vb_release.191206-1406", 1415 },
    { 19042, L"20H2", L"2009", L"19041.vb_release.191206-1406", 2965 },
    { 19043, L"21H1", L"2009", L"19041.vb_release.191206-1406", 2364 },
    { 19044, L"21H2", L"2009", L"19041.vb_release.191206-1406", 5131 },
    { 19045, L"22H2", L"2009", L"19041.vb_release.191206-1406", 6456 },
    { 22000, L"21H2", L"2009", L"22000.co_release.210604-1628", 3260 },
    { 22621, L"22H2", L"2009", L"22621.ni_release.220506-1250", 6060 },
    { 22631, L"23H2", L"2009", L"22621.ni_release.220506-1250", 6060 },
    { 26100, L"24H2", L"2009", L"26100.ge_release.240331-1435", 7171 },
    { 26200, L"25H2", L"2009", L"26100.ge_release.240331-1435", 6584 },   /* 24H2's code, switched on */
};
#define VX_RELEASE_COUNT (sizeof(vx_releases) / sizeof(vx_releases[0]))

static __inline const vx_release *vx_find_release(ULONG build)
{
    unsigned i;
    for (i = 0; i < VX_RELEASE_COUNT; i++) if (vx_releases[i].build == build) return &vx_releases[i];
    return 0;
}

#endif
