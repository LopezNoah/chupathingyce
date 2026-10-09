"""Manifest selection, disabled-source removal, settings and dispatcher contracts."""
import json
import shutil
import subprocess
from pathlib import Path
from types import SimpleNamespace
from unittest import SkipTest, TestCase

import tools.features as features
from tools import linux_build

ROOT = Path(__file__).resolve().parents[1]


def test_selection():
    selected = features.defines(SimpleNamespace(feature_bots=True, feature_infection=True))
    assert "-DHALO_EXTENSION_1=forge" in selected
    assert "-DHALO_EXTENSION_2=bots" in selected
    assert "-DHALO_EXTENSION_3=infection" in selected
    assert "-DHALO_FEATURE_SETTINGS_3=infection" in selected
    assert features.defines(SimpleNamespace(feature_forge=False)) == []
    config = {"game": {"root": "source"}}
    off = linux_build.game_sources(config, SimpleNamespace(feature_forge=False))
    on = linux_build.game_sources(config, SimpleNamespace(feature_bots=True, feature_infection=True))
    assert not any("source/features/" in str(path) for path in off)
    assert Path("source/features/bots/bots.c") in on
    assert Path("source/features/infection/game_engine_infection.c") in on


def test_drop_in(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    module = Path("source/features/experiment")
    module.mkdir(parents=True)
    (module / "feature.json").write_text(json.dumps({"name": "experiment", "settings": "settings.inc"}))
    (module / "settings.inc").write_text("")
    (module / "experiment.c").write_text("")
    assert features.defines(SimpleNamespace(feature_experiment=True)) == [
        "-DHALO_FEATURE_EXPERIMENT", "-DHALO_EXTENSION_1=experiment", "-DHALO_FEATURE_SETTINGS_1=experiment"]
    assert features.excluded_sources(SimpleNamespace()) == {str(module / "experiment.c")}
    assert features.excluded_sources(SimpleNamespace(feature_experiment=True)) == set()
    (module / "feature.json").write_text('{"name": "wrong"}')
    with TestCase().assertRaisesRegex(ValueError, "directory"):
        features.discover()


def test_bad_manifest(tmp_path):
    module = tmp_path / "bad"
    module.mkdir()
    (module / "feature.json").write_text("not json")
    with TestCase().assertRaisesRegex(ValueError, "cannot read"):
        features.discover(tmp_path)
    (module / "feature.json").write_text('{"name": "bad", "default": "false"}')
    with TestCase().assertRaisesRegex(ValueError, "boolean"):
        features.discover(tmp_path)


def compiler():
    cc = shutil.which("clang")
    if not cc:
        raise SkipTest("clang required")
    assert cc is not None
    return cc


def test_dispatcher(tmp_path):
    # Stand-ins for cseries types, not the game's Xbox ABI: only the dispatcher
    # is under test; the macOS matrix separately compiles the real game headers.
    (tmp_path / "cseries.h").write_text(
        "#pragma once\n#include <stddef.h>\n#include <wchar.h>\n"
        "typedef int boolean; typedef float real;\n"
        "#define TRUE 1\n#define FALSE 0\n#define NUMBEROF(a) (sizeof(a)/sizeof((a)[0]))\n")
    executable = tmp_path / "dispatch_test"
    subprocess.run([compiler(), "-std=gnu89", "-Wall", "-Wextra", "-Werror", "-I", str(tmp_path),
                    "-I", str(ROOT / "source"), "-DHALO_EXTENSION_1=first", "-DHALO_EXTENSION_2=second",
                    str(ROOT / "source/extensions/extension_dispatch.c"),
                    str(ROOT / "tools/tests/extension_dispatch_test.c"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
    empty = tmp_path / "empty.c"
    empty.write_text('''
#include "extensions/extension_api.h"
#include <assert.h>
int main(void) {
    boolean result = TRUE, reset = TRUE;
    void *proc = (void *)1;
    assert(halo_extensions_count() == 0);
    assert(!halo_extensions_active_ruleset());
    assert(!halo_extensions_should_end_game(&result) && result);
    assert(!halo_extensions_suppress_network_state());
    assert(!halo_extensions_suppress_game_report());
    assert(halo_extensions_can_collect_items(0));
    assert(halo_extensions_damage_multiplier(0, 0) == 1.f);
    assert(!halo_extensions_input_captured());
    assert(!halo_extensions_editor_director_camera(0, &proc, &reset));
    assert(!proc && !reset);
    halo_extensions_objects_placed();
    halo_extensions_update_player_controllers();
    halo_extensions_end_tick();
    return 0;
}
''')
    subprocess.run([compiler(), "-std=gnu89", "-Wall", "-Wextra", "-Werror", "-I", str(tmp_path),
                    "-I", str(ROOT / "source"), str(ROOT / "source/extensions/extension_dispatch.c"),
                    str(empty), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)


def test_settings_sections(tmp_path):
    # Compile the actual settings table and default writer with small append
    # stand-ins. TOML parsing catches duplicate sections such as [debug].
    source = (ROOT / "port/linux/src/port_config.c").read_text()
    declarations = source[source.index("enum config_type"):source.index("/* macOS starts")]
    table = source[source.index("/* Computed includes"):source.index("#ifdef HALO_ANDROID", source.index("#define NUMBER_OF_CONFIG_SETTINGS"))]
    writer = source[source.index("static char *config_default_text(void)"):source.index("/* whether text has", source.index("static char *config_default_text(void)"))]
    harness = '''
#include <stdio.h>
#include <string.h>
#define DEFAULT_FULLSCREEN "false"
#define DEFAULT_AUDIO_BUFFER_FRAMES "2048"
#define CONFIG_VERSION_TEXT ""
#define CONFIG_PLATFORM _platform_desktop
'''+declarations+table+'''
struct config_text { char *buffer; size_t size, capacity; };
static char output[131072];
static void config_append(struct config_text *text, const char *value) {
    (void)text; strcat(output, value);
}
static void config_append_setting(struct config_text *text, const struct config_setting *setting) {
    config_append(text, strchr(setting->name, '.') + 1);
    config_append(text, " = "); config_append(text, setting->default_value);
    config_append(text, "\\n");
}
'''+writer+'''
int main(void) { config_default_text(); puts(output); return 0; }
'''
    path = tmp_path / "settings_test.c"
    path.write_text(harness)
    executable = tmp_path / "settings_test"
    subprocess.run([compiler(), "-I", str(ROOT / "source"),
                    *features.defines(SimpleNamespace(feature_bots=True, feature_infection=True)),
                    str(path), "-o", str(executable)], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True, check=True)
    import tomllib
    config = tomllib.loads(result.stdout)
    assert config["bots"]["count"] == 0
    assert config["infection"]["local_enabled"] is False
    assert config["forge"]["toggle_key"] == "F7, B"
    assert "nav_probe" in config["debug"]
    assert "forge_test" in config["debug"]
    assert "infection_test_map" in config["debug"]
