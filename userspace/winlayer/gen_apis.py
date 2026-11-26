#!/usr/bin/env python3
"""Generates apis_gen.h: what every Windows API function returns, so a
function the layer does not have fails the way its callers expect, and the
names behind DLL ordinals.

  python3 gen_apis.py <spec-dir> > apis_gen.h

Run where libclang's Python bindings and MinGW-w64's headers and import
libraries are (Alpine: py3-clang clang17 mingw-w64-headers mingw-w64-crt
binutils).  The functions are those the system DLLs' import libraries
export; their return types come from MinGW-w64's headers, read by clang as
a compiler reads them.  <spec-dir> holds export lists with ordinals
(<dll>.spec, "<ordinal> <convention> [-flags] <name>" lines) for the DLLs
programs import by number: Wine's dlls/<dll>/<dll>.spec files list them,
and only their numbers and names are taken (the DLLs' export numbers).

Kinds (what a missing function returns):
  v  nothing                 b  FALSE                 p  NULL
  h  E_NOTIMPL               n  STATUS_NOT_IMPLEMENTED
  e  ERROR_CALL_NOT_IMPLEMENTED (functions returning a Win32 error code)
  m  MMSYSERR_NOTSUPPORTED   c  CR_CALL_NOT_IMPLEMENTED
  s  SEC_E_UNSUPPORTED_FUNCTION
  H  INVALID_HANDLE_VALUE    i  0, or what its DLL's functions return
"""
import os
import re
import subprocess
import sys

import clang.cindex as ci

# libclang under a versioned name (Alpine: libclang.so.17)
if not os.environ.get("LIBCLANG"):
    import glob
    found = sorted(glob.glob("/usr/lib/libclang.so*") + glob.glob("/usr/lib/llvm*/lib/libclang.so*"))
    if found:
        ci.Config.set_library_file(found[0])
else:
    ci.Config.set_library_file(os.environ["LIBCLANG"])

MINGW = "/usr/x86_64-w64-mingw32"

# the DLLs programs link to that the layer answers for (builtin or stubbed)
DLLS = """kernel32 kernelbase ntdll user32 gdi32 advapi32 shell32 shlwapi ole32 oleaut32 combase comctl32 comdlg32
version winmm ws2_32 wsock32 mswsock iphlpapi dnsapi winhttp wininet urlmon crypt32 bcrypt ncrypt secur32 wintrust
setupapi cfgmgr32 hid dbghelp imagehlp psapi powrprof userenv wtsapi32 netapi32 mpr imm32 uxtheme dwmapi shcore
propsys d3d9 d3d11 d3d12 dxgi d3dcompiler_47 dsound dinput8 xinput1_4 xinput9_1_0 opengl32 glu32 msimg32 winspool
usp10 dwrite d2d1 mfplat mf mfreadwrite avrt ole32 rpcrt4 wldap32 normaliz credui wevtapi taskschd ntdsapi
rstrtmgr dhcpcsvc fwpuclnt wlanapi winusb bluetoothapis cabinet msi msvcrt ucrtbase""".split()

HEADERS = """winsock2.h ws2tcpip.h windows.h winternl.h ole2.h oleauto.h objbase.h combaseapi.h shlobj.h shlwapi.h
shellapi.h commctrl.h commdlg.h mmsystem.h mmreg.h winver.h iphlpapi.h wininet.h winhttp.h urlmon.h wincrypt.h
bcrypt.h ncrypt.h security.h sspi.h wintrust.h softpub.h setupapi.h cfgmgr32.h dbghelp.h psapi.h powrprof.h
userenv.h wtsapi32.h lm.h winnetwk.h imm.h uxtheme.h dwmapi.h shellscalingapi.h propsys.h propvarutil.h d3d9.h
d3d11.h d3d12.h dxgi1_6.h d3dcompiler.h dsound.h dinput.h xinput.h GL/gl.h GL/glu.h winspool.h usp10.h dwrite_3.h
d2d1.h mfapi.h mfidl.h mfreadwrite.h avrt.h rpc.h winldap.h wincred.h winevt.h taskschd.h ntdsapi.h
restartmanager.h dhcpcsdk.h fwpmu.h wlanapi.h winusb.h bluetoothapis.h fdi.h fci.h msi.h roapi.h winstring.h
aclapi.h sddl.h ntsecapi.h tlhelp32.h winioctl.h processthreadsapi.h synchapi.h hidsdi.h windns.h""".split()

def resource_dir():
    """clang's own headers (x86intrin.h ...), which libclang does not always find"""
    import glob
    for d in sorted(glob.glob("/usr/lib/llvm*/lib/clang/*/include") + glob.glob("/usr/lib/clang/*/include")):
        if os.path.exists(os.path.join(d, "x86intrin.h")):
            return ["-isystem", d]
    return []


ARGS = resource_dir() + ["-target", "x86_64-w64-windows-gnu", "-isystem", MINGW + "/include", "-D_WIN32_WINNT=0x0A00",
        "-DNTDDI_VERSION=0x0A000008", "-DWINVER=0x0A00", "-DSECURITY_WIN32", "-DCOBJMACROS", "-DUNICODE",
        "-D_UNICODE", "-fms-extensions", "-ferror-limit=0", "-w", "-Wno-everything"]

# handles whose "no handle" is -1
INVALID_IS_MINUS_ONE = re.compile(r"^(CreateFile|CreateFile2|CreateFileTransacted|CreateNamedPipe|CreateMailslot|"
                                  r"FindFirst|CreateToolhelp32Snapshot|CreateConsoleScreenBuffer|OpenBackupEventLog|"
                                  r"FindFirstChangeNotification|FindFirstVolume|FindFirstVolumeMountPoint|"
                                  r"SetupDiGetClassDevs|SetupDiCreateDeviceInfoList|SetupOpenInfFile)")

STATUS = {"HRESULT": "h", "NTSTATUS": "n", "BOOL": "b", "WINBOOL": "b", "BOOLEAN": "b", "LSTATUS": "e",
          "NET_API_STATUS": "e", "RPC_STATUS": "e", "WIN32_ERROR": "e", "MMRESULT": "m", "MCIERROR": "m",
          "CONFIGRET": "c", "SECURITY_STATUS": "s"}


PER_DLL = {}          # dll -> every function it exports


def exports():
    """function name -> dll, from the import libraries"""
    found = {}
    for dll in DLLS:
        lib = "%s/lib/lib%s.a" % (MINGW, dll)
        if not os.path.exists(lib):
            continue
        out = subprocess.run(["nm", "-g", lib], capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = re.search(r" I __imp_(\w+)$", line)
            if m:
                found.setdefault(m.group(1), dll)
                PER_DLL.setdefault(dll, set()).add(m.group(1))
    return found


def tkind(t):
    """the type's kind id (newer libclangs have kinds these bindings do not name)"""
    return t._kind_id


def kind_of(t):
    seen = 0
    while seen < 32:
        seen += 1
        name = t.spelling.replace("const ", "").replace("volatile ", "").strip()
        if name in STATUS:
            return STATUS[name]
        if tkind(t) == ci.TypeKind.TYPEDEF.value:
            t = t.get_declaration().underlying_typedef_type
            continue
        if tkind(t) == ci.TypeKind.ELABORATED.value:
            t = t.get_named_type()
            continue
        break
    c = t.get_canonical()
    if tkind(c) == ci.TypeKind.VOID.value:
        return "v"
    if tkind(c) in (ci.TypeKind.POINTER.value, ci.TypeKind.BLOCKPOINTER.value):
        return "p"
    return "i"


def declarations():
    kinds = {}
    index = ci.Index.create()
    units = [HEADERS, ["windows.h", "ddk/ntifs.h"]]
    for headers in units:
        src = "".join("#include <%s>\n" % h for h in headers)
        args = ARGS + (["-isystem", MINGW + "/include/ddk"] if headers is units[1] else [])
        tu = index.parse("apis.c", args=args, unsaved_files=[("apis.c", src)],
                         options=ci.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES)
        errors = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error]
        for d in errors[:5]:
            print("/* clang: %s */" % d.spelling.replace("*/", "* /"), file=sys.stderr)
        for c in tu.cursor.get_children():
            if c.kind == ci.CursorKind.FUNCTION_DECL and c.spelling not in kinds:
                kinds[c.spelling] = kind_of(c.result_type)
    return kinds


def ordinals(spec_dir):
    table = []
    for f in sorted(os.listdir(spec_dir)):
        if not f.endswith(".spec"):
            continue
        dll = f[:-5] + ".dll"
        for line in open(os.path.join(spec_dir, f), encoding="utf-8", errors="replace"):
            m = re.match(r"^\s*(\d+)\s+(?:stdcall|cdecl|varargs|stub|thiscall|fastcall|extern)\s+(?:-\S+\s+)*([A-Za-z_]\w*)",
                         line)
            if m and int(m.group(1)) < 65536:
                table.append((dll, int(m.group(1)), m.group(2)))
    return table


def main():
    spec_dir = sys.argv[1] if len(sys.argv) > 1 else "spec"
    exp = exports()
    decl = declarations()
    rows = []
    for name in sorted(exp):
        if name in decl:
            k = decl[name]
        elif exp[name] == "ntdll" and re.match(r"^(Nt|Zw)[A-Z]", name):
            k = "n"                                   # system calls: an NTSTATUS
        else:
            continue
        if k == "p" and INVALID_IS_MINUS_ONE.match(name):
            k = "H"
        if k == "i" and re.match(r"^Reg[A-Z]", name) and exp[name] in ("advapi32", "kernel32", "kernelbase"):
            k = "e"                                   # the registry's LONGs are error codes
        rows.append((name, k))
    ords = ordinals(spec_dir)
    out = sys.stdout
    out.write("/* Generated by gen_apis.py from MinGW-w64's headers and import libraries\n"
              " * (and export ordinals): do not edit.  %d functions, %d ordinals. */\n" % (len(rows), len(ords)))
    out.write("static const char api_table[] =\n")
    line = '    "'
    for name, k in rows:
        entry = "%s\\0%s\\0" % (name, k)          # name, NUL, kind, NUL
        if len(line) + len(entry) > 116:
            out.write(line + '"\n')
            line = '    "'
        line += entry
    out.write(line + '";\n')
    out.write("#define API_COUNT %d\n" % len(rows))
    # each DLL's exports: the DLL's name, its functions, then an empty name
    out.write("static const char api_exports[] =\n")
    line = '    "'
    count = 0
    for dll in sorted(PER_DLL):
        names = ["%s.dll" % dll] + sorted(PER_DLL[dll]) + [""]
        for n in names:
            entry = "%s\\0" % n
            if len(line) + len(entry) > 116:
                out.write(line + '"\n')
                line = '    "'
            line += entry
        count += len(PER_DLL[dll])
    out.write(line + '";\n')
    out.write("#define API_EXPORTS %d\n" % count)
    out.write("static const struct { const char *dll; uint16_t ord; const char *name; } api_ordinals[] = {\n")
    for dll, o, name in ords:
        out.write('    { "%s", %d, "%s" },\n' % (dll, o, name))
    out.write("};\n")
    print("%d of %d exports declared" % (len(rows), len(exp)), file=sys.stderr)
    print("%d functions (%s), %d ordinals" % (len(rows), " ".join(
        "%s=%d" % (k, sum(1 for _, x in rows if x == k)) for k in "vbphnemcsHi"), len(ords)), file=sys.stderr)


main()
