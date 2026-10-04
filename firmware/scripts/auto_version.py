"""PlatformIO pre-hook: stamp the env's deterministic version.

Runs for the versioned firmware envs (DAS_v0_1, Tamu_v2_0A); bootloaders are unversioned, so
nothing is injected for them. The manual -D VERSION_* flags are gone from platformio.ini.
"""
import os
import sys

Import("env")  # noqa: F821 - provided by PlatformIO's SCons environment

_ROOT = os.path.abspath(os.path.join(env.subst("$PROJECT_DIR"), ".."))
sys.path.insert(0, os.path.join(_ROOT, "scripts"))
import version  # noqa: E402

_env_name = env.subst("$PIOENV")
if _env_name in version.TARGETS:
    _v = version.ensure_version(_env_name)
    env.Append(
        CPPDEFINES=[
            ("VERSION_YEAR", _v["year"]),
            ("VERSION_MONTH", _v["month"]),
            ("VERSION_DAY", _v["day"]),
            ("VERSION_ITERATION", _v["iteration"]),
        ]
    )
    print("Tamu version %s: %d.%d.%d.%d" % (
        _env_name, _v["year"], _v["month"], _v["day"], _v["iteration"]))
