"""Keep StackChan-BSP 1.1.0 from treating a failed position read as full left.

Runs after PlatformIO has resolved lib_deps, before compilation. Checks the
actual source on every build, so a clean checkout or dependency reinstall gets
the same fix. Unexpected upstream code fails the build instead of skipping it.
"""

from pathlib import Path

ORIGINAL = """        int current_pos = _scs_bus.ReadPos(_config.id);
        int angle       = (current_pos - _zero_pos) * 5 * 10 / 16;
"""

PATCHED = """        int current_pos = _scs_bus.ReadPos(_config.id);
        // stackchan-mqtt: invalid feedback must never become a physical angle.
        // ReadPos returns -1 on timeout; converting it used to yield -1280.
        if (current_pos < _config.rawPosLimit.x || current_pos > _config.rawPosLimit.y) {
            const uint32_t now = millis();
            if (!_feedback_error_logged || now - _feedback_error_at >= 5000) {
                ESP_LOGW(TAG, "Servo ID: %d invalid position %d; retaining motion estimate", _config.id, current_pos);
                _feedback_error_at = now;
                _feedback_error_logged = true;
            }
            return Servo::getCurrentAngle();
        }
        int angle       = (current_pos - _zero_pos) * 5 * 10 / 16;
"""

MEMBERS_ORIGINAL = """    int _zero_pos      = 0;
    Mode _current_mode = Mode::Position;
"""

MEMBERS_PATCHED = """    int _zero_pos      = 0;
    Mode _current_mode = Mode::Position;
    uint32_t _feedback_error_at = 0;
    bool _feedback_error_logged = false;
"""


def patch_source(source: str) -> str:
    for before, after in [(ORIGINAL, PATCHED), (MEMBERS_ORIGINAL, MEMBERS_PATCHED)]:
        if source.count(after) == 1:
            continue
        if source.count(before) != 1:
            raise RuntimeError("Unsupported StackChan-BSP source: review the servo feedback patch")
        source = source.replace(before, after, 1)
    return source


def apply_patch(path: Path) -> None:
    source = path.read_text(encoding="utf-8")
    patched = patch_source(source)
    if patched != source:
        path.write_text(patched, encoding="utf-8", newline="\n")
        print("StackChan-BSP: applied servo feedback error fix")


try:
    Import("env")  # noqa: F821 (provided by PlatformIO / SCons)
except NameError:
    pass  # Also importable by the host regression test.
else:
    apply_patch(Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") /  # noqa: F821
                "StackChan-BSP" / "src" / "M5StackChan.cpp")
