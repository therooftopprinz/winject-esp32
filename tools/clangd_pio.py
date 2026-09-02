# Expose the Xtensa toolchain and a stable compile database so host clangd
# can parse firmware sources (Ubuntu clangd has no Xtensa backend).
#
# CMake rewrites .pio/build/<env>/compile_commands.json in place (tmp +
# rename), which leaves clangd with a missing or truncated database during
# every configure. Copy a complete snapshot to .pio/clangd/ instead.
Import("env")  # pylint: disable=undefined-variable

import json
from pathlib import Path


def _replace_symlink(link: Path, target: Path) -> None:
    link.parent.mkdir(parents=True, exist_ok=True)
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(target)


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_bytes(data)
    tmp.replace(path)


def _load_compile_commands(path: Path) -> list:
    if not path.is_file():
        return []
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return []
    return data if isinstance(data, list) else []


def _install_clangd_db() -> None:
    by_file = {}
    # test first so the default env wins for shared sources.
    for env_name in ("test", "wt32-eth01"):
        src = project / ".pio" / "build" / env_name / "compile_commands.json"
        for entry in _load_compile_commands(src):
            f = entry.get("file")
            if f:
                by_file[f] = entry
    if not by_file:
        return
    dest_dir = project / ".pio" / "clangd"
    dest = dest_dir / "compile_commands.json"
    payload = json.dumps(list(by_file.values()), separators=(",", ":")).encode()
    _atomic_write(dest, payload)
    _replace_symlink(project / "compile_commands.json", dest)


project = Path(env.subst("$PROJECT_DIR"))
platform = env.PioPlatform()
toolchain = Path(platform.get_package_dir("toolchain-xtensa-esp-elf"))
idf = Path(platform.get_package_dir("framework-espidf"))

_replace_symlink(project / ".pio" / "clangd-toolchain", toolchain)
_replace_symlink(project / ".pio" / "clangd-idf", idf)

cxx_root = toolchain / "xtensa-esp-elf" / "include" / "c++"
if cxx_root.is_dir():
    versions = sorted(p for p in cxx_root.iterdir() if p.is_dir())
    if versions:
        _replace_symlink(project / ".pio" / "clangd-cxx", versions[-1])

_install_clangd_db()
env.AddPostAction("buildprog", lambda *args, **kwargs: _install_clangd_db())
