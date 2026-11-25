import argparse
import gzip
import io
import subprocess
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PACKAGE = "com.itouch.remote"
VERSION = "1.3.0"
ARCHITECTURE = "iphoneos-arm"


def archive(files):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for name, mode, data in files:
            entry = tarfile.TarInfo("./" + name)
            entry.mode = mode
            entry.uname = "root"
            entry.gname = "wheel"
            if isinstance(data, str):
                entry.type = tarfile.SYMTYPE
                entry.linkname = data
                tar.addfile(entry)
            else:
                entry.size = len(data)
                tar.addfile(entry, io.BytesIO(data))
    return gzip.compress(output.getvalue(), mtime=0)


def deb_member(name, data):
    header = f"{name + '/':<16}{0:<12}{0:<6}{0:<6}{'100644':<8}{len(data):<10}`\n"
    return header.encode("ascii") + data + (b"\n" if len(data) % 2 else b"")


def package(executable):
    control = (
        f"Package: {PACKAGE}\nVersion: {VERSION}\nArchitecture: {ARCHITECTURE}\n"
        "Name: iTouch\nMaintainer: iTouch\nSection: Utilities\n"
        "Depends: firmware (>= 10.0), firmware (<< 11.0)\n"
        "Description: System touch input over SSH for arm64 iOS 10.\n"
    ).encode("ascii")
    maintainer = [
        (name, 0o755, (ROOT / "package" / name).read_bytes().replace(b"\r\n", b"\n"))
        for name in ("postinst", "prerm", "postrm")
    ]
    payload = archive([
        ("usr/local/bin/itouch", 0o755, executable),
        ("usr/bin/itouch", 0o777, "../local/bin/itouch"),
        ("Library/LaunchDaemons/" + PACKAGE + ".plist", 0o644,
         (ROOT / (PACKAGE + ".plist")).read_bytes()),
    ])
    deb = ROOT / "dist" / f"{PACKAGE}_{VERSION}_{ARCHITECTURE}.deb"
    deb.write_bytes(
        b"!<arch>\n" + deb_member("debian-binary", b"2.0\n")
        + deb_member("control.tar.gz", archive([("control", 0o644, control), *maintainer]))
        + deb_member("data.tar.gz", payload)
    )
    return deb


def stub(directory, name, install_name, symbols):
    path = directory / name
    path.write_text(
        "--- !tapi-tbd-v3\n"
        "archs: [ arm64 ]\n"
        "platform: ios\n"
        f"install-name: '{install_name}'\n"
        "current-version: 1.0.0\n"
        "compatibility-version: 1.0.0\n"
        "exports:\n"
        "  - archs: [ arm64 ]\n"
        f"    symbols: [ {', '.join(sorted(symbols))} ]\n"
        "...\n",
        encoding="ascii",
    )
    return path


def main():
    parser = argparse.ArgumentParser(description="Build iTouch for arm64 iOS 10")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--lld", default="ld64.lld")
    parser.add_argument("--llvm-nm", default="llvm-nm")
    parser.add_argument("--headers", type=Path, required=True,
                        help="Darwin C headers, e.g. Zig lib/libc/include/any-darwin-any")
    parser.add_argument("--signer", default="rcodesign")
    args = parser.parse_args()
    (ROOT / "dist").mkdir(exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="itouch-build-") as tmp:
        work = Path(tmp)
        obj = work / "itouch.o"
        subprocess.run([
            args.clang, "-target", "arm64-apple-ios10.0", "-isystem",
            str(args.headers.resolve()), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-O2", "-c", "itouch.c", "-o", str(obj),
        ], cwd=ROOT, check=True)
        undefined = set(subprocess.check_output(
            [args.llvm_nm, "--undefined-only", "--just-symbol-name", str(obj)],
            text=True,
        ).split())
        hid = {symbol for symbol in undefined if symbol.startswith("_IOHID")}
        cf = {"_CFRelease"}
        springboard = {"_SBSSpringBoardServerPort", "_SBGetInterfaceOrientation"}
        system = (undefined - hid - cf - springboard) | {"dyld_stub_binder"}
        signed = work / "itouch"
        linker = [args.lld]
        if Path(args.lld).name.lower() in {"lld", "lld.exe", "ld.lld", "ld.lld.exe"}:
            linker += ["-flavor", "darwin"]
        subprocess.run(linker + [
            "-arch", "arm64", "-platform_version", "ios", "10.0", "10.3",
            "-no_fixup_chains", "-no_adhoc_codesign", "-o", str(signed), str(obj),
            str(stub(work, "libSystem.tbd", "/usr/lib/libSystem.B.dylib", system)),
            str(stub(work, "IOKit.tbd", "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit", hid)),
            str(stub(work, "CoreFoundation.tbd", "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", cf)),
            str(stub(work, "SpringBoardServices.tbd", "/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", springboard)),
        ], check=True)
        subprocess.run([
            args.signer, "sign", "--digest", "sha1", "--timestamp-url", "none",
            "--entitlements-xml-file", "entitlements.plist", str(signed),
        ], cwd=ROOT, check=True)
        executable = signed.read_bytes()
    (ROOT / "dist" / "itouch").write_bytes(executable)
    print(package(executable))


if __name__ == "__main__":
    main()
