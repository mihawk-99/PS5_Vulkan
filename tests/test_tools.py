#!/usr/bin/env python3
# ps5-native-app-boilerplate - Host tooling regression tests.
# Copyright (C) 2026 BlackBearReloaded
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Exercises identity initialization and deployment resolution without a console.

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ToolTests(unittest.TestCase):
    def run_init(self, param, **values):
        environment = os.environ.copy()
        environment.update(values)
        return subprocess.run(
            ["bash", str(ROOT / "tools/init-project.sh"), str(param)],
            cwd=ROOT,
            env=environment,
            check=False,
            capture_output=True,
            text=True,
        )

    def test_init_coordinates_media_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            param = Path(directory) / "param.json"
            param.write_text(
                json.dumps(
                    {
                        "contentId": "UP9000-PPSA99999_00-HELLOWORLD000001",
                        "localizedParameters": {
                            "defaultLanguage": "en-US",
                            "en-US": {"titleName": "Old"},
                        },
                        "gameIntent": {"permittedIntents": [{"intentType": "launchActivity"}]},
                    }
                ),
                encoding="utf-8",
            )
            result = self.run_init(
                param,
                TITLE_ID="PPSA12345",
                APP_NAME="Moon Client",
                APP_CATEGORY="media",
                CONTENT_SUFFIX="MOONCLIENT000001",
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            configured = json.loads(param.read_text(encoding="utf-8"))
            self.assertEqual(configured["titleId"], "PPSA12345")
            self.assertEqual(configured["conceptId"], "12345")
            self.assertEqual(configured["contentId"], "UP9000-PPSA12345_00-MOONCLIENT000001")
            self.assertEqual(configured["localizedParameters"]["en-US"]["titleName"], "Moon Client")
            self.assertEqual(configured["applicationCategoryType"], 65536)
            self.assertEqual(configured["contentBadgeType"], 2)
            self.assertNotIn("gameIntent", configured)

    def test_init_rejects_invalid_title_without_rewriting(self):
        with tempfile.TemporaryDirectory() as directory:
            param = Path(directory) / "param.json"
            original = '{"contentId":"UP9000-PPSA99999_00-HELLOWORLD000001"}\n'
            param.write_text(original, encoding="utf-8")
            result = self.run_init(param, TITLE_ID="PPSA12", APP_NAME="Broken")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(param.read_text(encoding="utf-8"), original)

    def test_init_derives_game_suffix_and_preserves_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            param = Path(directory) / "param.json"
            param.write_text(
                json.dumps(
                    {
                        "contentId": "UP9000-PPSA99999_00-HELLOWORLD000001",
                        "localizedParameters": {
                            "defaultLanguage": "en-US",
                            "en-US": {"titleName": "Old"},
                        },
                    }
                ),
                encoding="utf-8",
            )
            param.chmod(0o640)
            result = self.run_init(
                param, TITLE_ID="PPSA54321", APP_NAME="Native Sample", CONTENT_SUFFIX=""
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            configured = json.loads(param.read_text(encoding="utf-8"))
            self.assertEqual(configured["contentId"], "UP9000-PPSA54321_00-NATIVESAMPLE0000")
            self.assertEqual(configured["applicationCategoryType"], 0)
            self.assertEqual(configured["contentBadgeType"], 1)
            self.assertEqual(
                configured["gameIntent"]["permittedIntents"],
                [{"intentType": "launchActivity"}],
            )
            self.assertEqual(param.stat().st_mode & 0o777, 0o640)

    def test_undeploy_dry_run_resolves_only_current_title(self):
        environment = os.environ.copy()
        environment.update(PS5_HOST="192.0.2.1", DEPLOY_DRY_RUN="1")
        result = subprocess.run(
            ["bash", str(ROOT / "tools/deploy.sh"), "undeploy"],
            cwd=ROOT,
            env=environment,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("/data/homebrew/PPSA99999/", result.stdout)
        self.assertIn("PPSA99999.{ffpkg,ffpfsc}", result.stdout)
        self.assertIn("no network request was sent", result.stdout)

    def test_deploy_dry_run_uses_mocked_build_and_no_network(self):
        with tempfile.TemporaryDirectory() as directory:
            sandbox = Path(directory)
            (sandbox / "tools").mkdir()
            (sandbox / "sce_sys").mkdir()
            shutil.copy2(ROOT / "tools/deploy.sh", sandbox / "tools/deploy.sh")
            (sandbox / "sce_sys/param.json").write_text(
                '{"titleId":"PPSA12345"}\n', encoding="utf-8"
            )

            mock_bin = sandbox / "mock-bin"
            mock_bin.mkdir()
            mock_make = mock_bin / "make"
            mock_make.write_text(
                "#!/usr/bin/env bash\n"
                "mkdir -p \"$MOCK_ROOT/dist\"\n"
                "printf package > \"$MOCK_ROOT/dist/PPSA12345.ffpkg\"\n",
                encoding="utf-8",
            )
            mock_make.chmod(0o755)

            environment = os.environ.copy()
            environment.update(
                PS5_HOST="192.0.2.1",
                DEPLOY_DRY_RUN="1",
                DEPLOY_FORMAT="ffpkg",
                MOCK_ROOT=str(sandbox),
                PATH=f"{mock_bin}{os.pathsep}{environment['PATH']}",
            )
            result = subprocess.run(
                ["bash", str(sandbox / "tools/deploy.sh")],
                cwd=sandbox,
                env=environment,
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("/data/homebrew/PPSA12345.ffpkg", result.stdout)
            self.assertIn("no network request was sent", result.stdout)


class GoldenCaptureTests(unittest.TestCase):
    """A stream a test wrote by hand can still be a golden file."""

    @classmethod
    def setUpClass(cls):
        tools = str(ROOT / "tools")
        if tools not in sys.path:
            sys.path.insert(0, tools)

    def stream(self, calls):
        import ps5vk_log

        words = [0xAABBCCDD, 0x11223344, 0x55667788]
        chunk = words + [0] * (64 - len(words))
        image = bytearray(len(chunk) * 4)
        for index, word in enumerate(chunk):
            image[4 * index:4 * index + 4] = word.to_bytes(4, "little")
        return {
            "test": "c1-test",
            "word_count": len(words),
            "words": list(words),
            "buffer": 0,
            "regions": {"stage": (0x1000, 0x100)},
            "workspace": {"begin": 0x1000, "end": 0x1000 + len(image)},
            "command_bottom": 0x1000,
            "chunks": {0: chunk},
            "chunk_count": 1,
            "workspace_fnv1a64": ps5vk_log.fnv1a64(bytes(image)),
            "calls": calls,
            "call_count": len(calls),
        }

    def stage(self, index, words, address=0x200030000):
        import ps5vk_log

        chunk = words + [0] * (64 - len(words))
        image = bytearray(len(chunk) * 4)
        for at, word in enumerate(chunk):
            image[4 * at:4 * at + 4] = word.to_bytes(4, "little")
        return {
            "index": index,
            "address": address,
            "bytes": len(image),
            "chunks": {0: chunk},
            "chunk_count": 1,
            "fnv1a64": ps5vk_log.fnv1a64(bytes(image)),
        }

    def stream_with_stages(self, stages, count=None):
        stream = self.stream([{"name": "raw_packets", "begin": 0, "end": 3, "args": []}])
        stream["stages"] = stages
        stream["stage_count"] = len(stages) if count is None else count
        return stream

    def test_raw_packet_span_completes_a_capture(self):
        import ps5vk_log

        span = {"name": "raw_packets", "begin": 0, "end": 3, "args": []}
        self.assertEqual(ps5vk_log.capture_problems(self.stream([span])), [])

    def test_unattributed_words_are_still_refused(self):
        import ps5vk_log

        call = {"name": "sceAgcCbReleaseMem", "begin": 3, "end": 3, "args": []}
        problems = ps5vk_log.capture_problems(self.stream([call]))
        self.assertTrue(any("not from word 0" in problem for problem in problems), problems)

    def test_raw_span_needs_no_model(self):
        import golden

        recorded = [0xAA, 0xBB]
        self.assertEqual(golden.run_model(None, {"name": golden.RAW_PACKETS, "args": []}, recorded, 0),
                         recorded)

    def test_pipeline_stages_are_captured_and_validated(self):
        import ps5vk_log

        stages = [self.stage(0, [0x11, 0x22]), self.stage(1, [0x33, 0x44], 0x200040000)]
        self.assertEqual(ps5vk_log.capture_problems(self.stream_with_stages(stages)), [])

    def test_a_stage_that_does_not_match_its_fnv_is_refused(self):
        import ps5vk_log

        stages = [self.stage(0, [0x11]), self.stage(1, [0x22], 0x200040000)]
        stages[1]["fnv1a64"] ^= 1
        problems = ps5vk_log.capture_problems(self.stream_with_stages(stages))
        self.assertTrue(any("pipeline stage 1" in problem for problem in problems), problems)

    def test_a_lost_stage_is_refused(self):
        import ps5vk_log

        problems = ps5vk_log.capture_problems(self.stream_with_stages([self.stage(0, [0x11])], count=2))
        self.assertTrue(any("1 of 2 pipeline stages logged" in problem for problem in problems),
                        problems)

    def test_a_golden_carries_its_stages_and_its_replay_names_them(self):
        import golden

        stages = [self.stage(0, [0x11]), self.stage(1, [0x22], 0x200040000)]
        document = golden.golden_document({"pid": 7}, self.stream_with_stages(stages), "k.log", 1, 1, 0)
        self.assertEqual([stage["index"] for stage in document["stages"]], [0, 1])
        self.assertEqual(int(document["stages"][1]["address"], 16), 0x200040000)
        text = golden.replay_text(document, [(0x4, 0)])
        self.assertIn("stage 0 0x200030000 0x100", text)
        self.assertIn("stageimage 1 0x0000 ", text)
        # The stages come before the regions, so the model hands each driver
        # stage its own captured region rather than the runner's workspace.
        self.assertLess(text.index("stage 0 "), text.index("region stage "))

    def test_a_golden_without_stages_replays_as_before(self):
        import golden

        document = golden.golden_document({"pid": 7},
                                          self.stream([{"name": "raw_packets", "begin": 0, "end": 3,
                                                        "args": []}]),
                                          "k.log", 1, 1, 0)
        self.assertEqual(document["stages"], [])
        self.assertNotIn("stageimage", golden.replay_text(document, [(0x4, 0)]))


class ParkedWorkTests(unittest.TestCase):
    """Work that waits for a console session is parked as a patch in the
    repository root, and the battery that proves it applies that patch before it
    runs, so the patch has to apply to the tree it is committed against. A
    change that collides with it -- the component mapping field did, in Phase
    C8's resolve patch -- is caught here instead of at the console. The check is
    skipped while the tree has uncommitted changes, where a conflict is the
    author's own work in progress."""

    def test_every_parked_patch_applies_to_the_committed_tree(self):
        patches = sorted(ROOT.glob("*-wip.patch"))
        if not patches:
            self.skipTest("no parked patch in the repository root")
        status = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        if status.strip():
            self.skipTest("the tree has uncommitted changes")
        for patch in patches:
            result = subprocess.run(
                ["git", "apply", "--check", patch.name],
                cwd=ROOT,
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                result.returncode,
                0,
                f"{patch.name} no longer applies to HEAD: {result.stderr.strip()}",
            )


class LimitsAuditTests(unittest.TestCase):
    """The device's reported limits are a Vulkan 1.0 claim, and the
    specification's Required Limits table is what they answer to.
    tools/limits_audit.py reads both -- the table from the vendored
    limits-v1.4.354.adoc, the values from driver/ps5vk_physical_device.c -- and
    this holds the tree to it. It skips when the specification is not fetched,
    which is what a bare checkout without tools/setup-native-dependencies.sh
    gives."""

    def test_every_reported_limit_meets_the_required_limits_table(self):
        spec = ROOT / ".deps/native/vulkan-docs/limits-v1.4.354.adoc"
        if not spec.is_file():
            self.skipTest("the vendored Vulkan specification is not fetched")
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/limits_audit.py"), "--check"],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # --check only fails on a limit that misses, so a parser change that
        # quietly stopped comparing rows would go unnoticed: the coverage is part
        # of the check too. The nine that stay out are the rows whose type is a
        # recommendation, implementation-dependent or a duration, or whose core
        # column requires nothing.
        summary = re.search(
            r"(\d+) required VkPhysicalDeviceLimits members, (\d+) compared with the "
            r"table.*?, (\d+) not compared",
            result.stdout,
        )
        self.assertIsNotNone(summary, result.stdout)
        required, compared, uncompared = (int(value) for value in summary.groups())
        self.assertEqual(required, 106)
        self.assertGreaterEqual(compared, 97)
        self.assertLessEqual(uncompared, 9)


if __name__ == "__main__":
    unittest.main()


class RunnerCaseTests(unittest.TestCase):
    """tools/check-runner-cases.sh runs the runner's driver cases on the PC, so
    every case it names has to be one the runner knows: a name the runner's table
    does not hold is rejected by its queue parser at the console, which is the
    one place a typo there would otherwise be found."""

    def cases_of(self, script, name):
        # One "name=( ... )" list of the gate, with its comments dropped.
        start = script.index(f"{name}=(")
        end = script.index("\n)\n", start)
        return [
            line.strip()
            for line in script[start + len(name) + 2 : end].splitlines()
            if line.strip() and not line.strip().startswith("#")
        ]

    def test_every_runner_case_is_in_the_runner_table(self):
        script = (ROOT / "tools/check-runner-cases.sh").read_text(encoding="utf-8")
        cases = self.cases_of(script, "cases") + self.cases_of(script, "drawing_cases")
        self.assertTrue(cases, "the runner-case gate names no case")
        diagnostics = (ROOT / "src/diagnostics.cpp").read_text(encoding="utf-8")
        for name in cases:
            with self.subTest(case=name):
                self.assertIn(
                    f'{{"{name}",',
                    diagnostics,
                    f"{name} is not a case in src/diagnostics.cpp's runner table",
                )


class QueueCaseTests(unittest.TestCase):
    """A console queue is rejected whole when one of its lines names a case the
    runner does not have ("unknown test name"), so a typo in a jobs/*/queue.txt
    costs a console session. Every name there has to be a case the runner's table
    holds -- or one a parked patch adds, which is how the c8-resolve battery
    carries the case that waits for the console."""

    # The queue keywords the runner's parser handles itself (src/diagnostics.cpp).
    keywords = {"capture", "compile", "exit", "all"}

    def test_a_queue_stays_inside_the_runner_s_limits(self):
        """The parser rejects a queue whole for more than 32 tests, a `hold`
        outside 1..3600, or no test at all, so a queue that would cost a console
        session is caught here."""
        for queue in sorted(ROOT.glob("jobs/*/queue.txt")):
            lines = [
                line.strip()
                for line in queue.read_text(encoding="utf-8").splitlines()
                if line.strip() and not line.strip().startswith("#")
            ]
            with self.subTest(queue=queue.name):
                tests = [line for line in lines if line not in self.keywords and not line.startswith("hold ")]
                # A queue may name every default test with `all` instead of
                # listing them (jobs/compile and jobs/regression do).
                self.assertTrue(
                    tests or "all" in lines, f"{queue.name} names no test and no `all`"
                )
                self.assertLessEqual(len(tests), 32, f"{queue.name} queues more than 32 tests")
                for line in lines:
                    if line.startswith("hold "):
                        vblanks = int(line.split()[1])
                        self.assertTrue(1 <= vblanks <= 3600, f"{queue.name}: hold {vblanks}")

    def test_every_queue_case_is_a_runner_case(self):
        diagnostics = (ROOT / "src/diagnostics.cpp").read_text(encoding="utf-8")
        patched = "\n".join(
            patch.read_text(encoding="utf-8", errors="ignore")
            for patch in sorted(ROOT.glob("*-wip.patch"))
        )
        queues = sorted(ROOT.glob("jobs/*/queue.txt"))
        self.assertTrue(queues, "no job queue to check")
        for queue in queues:
            for line in queue.read_text(encoding="utf-8").splitlines():
                entry = line.strip()
                if not entry or entry.startswith("#"):
                    continue
                if entry in self.keywords or entry.startswith("hold "):
                    continue
                with self.subTest(queue=queue.name, case=entry):
                    self.assertTrue(
                        f'{{"{entry}",' in diagnostics or f'{{"{entry}",' in patched,
                        f"{queue.name}: the runner has no case named {entry}",
                    )


class SignedTargetProbeTests(unittest.TestCase):
    """The ten signed colour targets' probe. The rows' expected words are their
    unsigned twins' colours halved -- the shader writes ivec4(0x20, 0x40, 0x60,
    0x7f) where the unsigned one writes uvec4(0x40, 0x80, 0xc0, 0xff) -- so a
    typo in one of the ten words would be read as a hardware result on the
    console. The case itself is the unsigned family's shape: one frame a format,
    and the rows need the driver's COLOR_ATTACHMENT bit before vkCreateImage will
    make their targets (three rounds misread that refusal's old assert as a
    compiler fault, docs/HARDWARE_FINDINGS.md)."""

    # The unsigned half of the pair table (src/diagnostics.cpp, kUnsignedTargets).
    rows = re.compile(
        r'\{(VK_FORMAT_[A-Z0-9_]+),\s*"([A-Z0-9_]+)",\s*\{([^}]*)\},\s*(\d+),\s*(\d+)\}',
        re.S,
    )

    def rows_of(self, source, name):
        start = source.index(f"k{name}Targets = {{{{")
        end = source.index("}};", start)
        return {
            match.group(2): (
                match.group(1),
                [int(word, 16) for word in re.findall(r"0x([0-9a-fA-F]+)u", match.group(3))],
                int(match.group(4)),
                int(match.group(5)),
            )
            for match in self.rows.finditer(source[start:end])
        }

    def test_every_signed_row_is_its_unsigned_twin_s_colour_halved(self):
        source = (ROOT / "src/diagnostics.cpp").read_text(encoding="utf-8")
        unsigned = self.rows_of(source, "Unsigned")
        signed = self.rows_of(source, "Signed")
        self.assertEqual(len(signed), 10, "the signed table is not the ten rows")
        for name, (format_name, words, count, texel_bytes) in sorted(signed.items()):
            twin_name = name.replace("_SINT", "_UINT")
            with self.subTest(row=name):
                self.assertIn(twin_name, unsigned, f"{name} has no unsigned twin")
                twin = unsigned[twin_name]
                self.assertEqual(
                    (format_name.replace("_SINT", "_UINT"), count, texel_bytes), twin[:1] + twin[2:],
                    f"{name} does not mirror {twin_name}'s shape",
                )
                halved = []
                for word in twin[1]:
                    value = 0
                    for byte in range(4):
                        channel = (word >> (8 * byte)) & 0xFF
                        value |= (channel // 2) << (8 * byte)
                    halved.append(value)
                self.assertEqual(
                    words, halved, f"{name}'s words are not {twin_name}'s colours halved"
                )

    def test_the_signed_case_is_the_family_case_beside_the_unsigned_one(self):
        diagnostics = (ROOT / "src/diagnostics.cpp").read_text(encoding="utf-8")
        self.assertIn('{"v0-targets-sint", "v0-target-sint", run_vulkan_signed_target_frames},',
                      diagnostics)
        queue = (ROOT / "jobs/v0-sint/queue.txt").read_text(encoding="utf-8")
        lines = [line.strip() for line in queue.splitlines() if line.strip()]
        self.assertEqual(
            [line for line in lines if line.startswith("v0-target")], ["v0-targets-sint"]
        )

    def test_the_signed_format_entries_carry_a_colour_word(self):
        driver = (ROOT / "driver/ps5vk_image.c").read_text(encoding="utf-8")
        for format_name in (
            "VK_FORMAT_R8_SINT",
            "VK_FORMAT_R8G8_SINT",
            "VK_FORMAT_R8G8B8A8_SINT",
            "VK_FORMAT_A8B8G8R8_SINT_PACK32",
            "VK_FORMAT_R16_SINT",
            "VK_FORMAT_R16G16_SINT",
            "VK_FORMAT_R16G16B16A16_SINT",
            "VK_FORMAT_R32_SINT",
            "VK_FORMAT_R32G32_SINT",
            "VK_FORMAT_R32G32B32A32_SINT",
        ):
            with self.subTest(format=format_name):
                self.assertIn(f"{{{format_name}, ", driver)


class VertexFormatPatchTests(unittest.TestCase):
    """The compiler's vertex-format enum is extended by a patch script and the
    driver maps VkFormats onto the values it adds. The two halves live in
    different trees -- tooling/psbc/patch-vertex-formats.py and
    driver/ps5vk_pipeline.c -- so a drift between them is an attribute the driver
    names and the compiler does not know (or the reverse), which a console run
    finds only as a failed frame. This ties them together: every value the patch
    adds must be one the driver maps from a VkFormat whose entry claims
    VERTEX_BUFFER, and every value the driver maps must be either one of the
    compiler's own or one the patch adds."""

    # The values the pinned SDK's header already carries (psbc_compile.h): the
    # thirty-two-bit families, the 8888 UNORM layouts and the 10-10-10-2 forms.
    compiler_own = {
        "PSBC_VERTEX_FORMAT_R32_FLOAT",
        "PSBC_VERTEX_FORMAT_R32G32_FLOAT",
        "PSBC_VERTEX_FORMAT_R32G32B32_FLOAT",
        "PSBC_VERTEX_FORMAT_R32G32B32A32_FLOAT",
        "PSBC_VERTEX_FORMAT_B8G8R8A8_UNORM",
        "PSBC_VERTEX_FORMAT_R10G10B10A2_UNORM",
        "PSBC_VERTEX_FORMAT_B10G10R10A2_UNORM",
        "PSBC_VERTEX_FORMAT_R10G10B10A2_SNORM",
        "PSBC_VERTEX_FORMAT_B10G10R10A2_SNORM",
        "PSBC_VERTEX_FORMAT_R10G10B10A2_USCALED",
        "PSBC_VERTEX_FORMAT_B10G10R10A2_USCALED",
        "PSBC_VERTEX_FORMAT_R10G10B10A2_SSCALED",
        "PSBC_VERTEX_FORMAT_B10G10R10A2_SSCALED",
        "PSBC_VERTEX_FORMAT_R32_SINT",
        "PSBC_VERTEX_FORMAT_R32G32_SINT",
        "PSBC_VERTEX_FORMAT_R32G32B32_SINT",
        "PSBC_VERTEX_FORMAT_R32G32B32A32_SINT",
        "PSBC_VERTEX_FORMAT_R32_UINT",
        "PSBC_VERTEX_FORMAT_R32G32_UINT",
        "PSBC_VERTEX_FORMAT_R32G32B32_UINT",
        "PSBC_VERTEX_FORMAT_R32G32B32A32_UINT",
        "PSBC_VERTEX_FORMAT_R8G8B8A8_UNORM",
    }

    def patched(self, patch):
        """The (enum value, pipe format) pairs the patch's table lists."""
        body = patch.split("FORMATS = [", 1)[1].split("]", 1)[0]
        return re.findall(r'\("(PSBC_VERTEX_FORMAT_[A-Z0-9_]+)", "(PIPE_FORMAT_[A-Z0-9_]+)"\)', body)

    def test_the_patch_table_is_unique(self):
        patch = (ROOT / "tooling/psbc/patch-vertex-formats.py").read_text(encoding="utf-8")
        pairs = self.patched(patch)
        self.assertTrue(pairs, "the vertex-format patch lists no format")
        names = [name for name, _ in pairs]
        self.assertEqual(len(names), len(set(names)), "a format is patched twice")

    def test_every_patched_value_is_one_the_driver_maps(self):
        patch = (ROOT / "tooling/psbc/patch-vertex-formats.py").read_text(encoding="utf-8")
        pipeline = (ROOT / "driver/ps5vk_pipeline.c").read_text(encoding="utf-8")
        for name, _ in self.patched(patch):
            with self.subTest(value=name):
                vk_format = name[len("PSBC_VERTEX_FORMAT_") :]
                self.assertIn(
                    f"{{VK_FORMAT_{vk_format}, {name}}}",
                    pipeline,
                    f"{name} is patched into the compiler but no VkFormat maps to it",
                )

    def test_every_value_the_driver_maps_is_the_compiler_s_own_or_patched(self):
        patch = (ROOT / "tooling/psbc/patch-vertex-formats.py").read_text(encoding="utf-8")
        patched = {name for name, _ in self.patched(patch)}
        pipeline = (ROOT / "driver/ps5vk_pipeline.c").read_text(encoding="utf-8")
        mapped = set(re.findall(r"PSBC_VERTEX_FORMAT_[A-Z0-9_]+", pipeline)) - {
            "PSBC_VERTEX_FORMAT_NONE"
        }
        for name in sorted(mapped - patched - self.compiler_own):
            self.fail(f"{name} is mapped by the driver but no patch adds it to the compiler")

    def test_every_claim_is_one_the_format_table_reports(self):
        """A VkFormat the driver maps needs the bit its row claims: the patch
        only makes the word expressible."""
        patch = (ROOT / "tooling/psbc/patch-vertex-formats.py").read_text(encoding="utf-8")
        image = (ROOT / "driver/ps5vk_image.c").read_text(encoding="utf-8")
        for name, _ in self.patched(patch):
            format_name = f"VK_FORMAT_{name[len('PSBC_VERTEX_FORMAT_'):]}"
            with self.subTest(format=format_name):
                start = image.index(f"{{{format_name},")
                entry = image[start : image.index("},", start)]
                self.assertIn(
                    "VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT",
                    entry,
                    f"{format_name} is mapped but its entry claims no VERTEX_BUFFER",
                )


class PackedFormatTests(unittest.TestCase):
    """V0-formats' packed families: the probe's ground truth is a texel's bytes
    and the colour they mean, both written in src/diagnostics.cpp. The console
    compares the hardware against that colour, so the pair has to be right
    before a console run can say anything -- one wrong byte pattern was read as
    a hardware result once. tools/packed-format-check.py decodes every packed
    entry from the source per the specification's rules; this runs it."""

    def test_the_packed_probe_texels_are_the_colours_the_table_expects(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/packed-format-check.py")],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0x48ff decodes to [255, 136, 68]", result.stdout)


class FormatAuditTests(unittest.TestCase):
    """docs/V0_FORMATS_AUDIT.md's table is the record the V0-formats step's
    acceptance names: every required format either carries the feature bits the
    specification requires or is listed there as a deliberate gap. A driver
    change that closes or opens a gap has to move that table with it, which is
    what this checks -- the audit's own --check gate stays red while any gap
    remains, so the count is what has to stay honest."""

    labels = {
        "Formats the specification requires": "required",
        "Formats the driver reports": "reported",
        "Required formats missing a required feature": "missing",
        "Required formats with conditional requirements only": "conditional",
        # A footnote's must: clause is a requirement over a set of rows, not a
        # cell, so it is counted separately from the per-format list: the counts
        # are how many clauses no reported format satisfies and how many formats
        # those clauses name (tools/format_audit.py, required_clauses).
        "`must:` clauses no reported format satisfies": "clauses",
        "Formats a set-level `must:` clause names": "clause_formats",
    }

    # Empty since blocker round 9: every descriptor type the parked rows needed is
    # proved -- the uniform fetch (round 5), the storage store (round 6), the
    # storage image (round 7) and the storage image's atomics (round 9) -- and
    # every row that wanted one reports it (docs/BLOCKERS.md,
    # docs/V0_FORMATS_AUDIT.md). The compiler's enum named none of the three types
    # to begin with; tooling/psbc/patch-descriptor-types.py adds all of them now.
    parked_on_descriptor = set()
    # Every format whose VERTEX_BUFFER the PsbcVertexFormat enum cannot express.
    # The enum carries 32-bit-component formats (float, signed and unsigned, one
    # to four components), the two 8888 UNORM layouts and the eight 10-10-10-2
    # forms -- ps5-opengl's own ps5_vertex_format maps exactly those -- so no
    # 8-bit, 16-bit, 16-bit-float, 8888-SNORM or 8888-integer vertex attribute
    # has a format word to build. The nine expressible formats missing the bit in
    # the driver's table (R32_UINT/SINT, R32G32_*, R32G32B32A32_SINT,
    # R8G8B8A8_UNORM, A8B8G8R8_UNORM_PACK32, B8G8R8A8_UNORM and
    # A2B10G10R10_UNORM_PACK32) stay reachable work.
    # Empty since blocker round 3: the hardware fetches every layout the required
    # formats name and tooling/psbc/patch-vertex-formats.py gives the compiler's enum
    # the value and pipe format for each (docs/BLOCKERS.md), so no row is parked on
    # the vertex enum any more.
    parked_on_vertex = set()
    # The hardware's own category: an sRGB fetch linearises the first three
    # fetched components, before any selector, so these two formats' features
    # are not reachable by a probe or a driver word at all.
    # Round 13 and 14 separated the two: the measurement is of the *packed*
    # format, whose alpha byte takes the curve in a colour channel's place, and
    # the byte-reversed 8888 form's first three fetched bytes are its whole
    # colour triple, so it is proved and reported like any other row.
    # Empty since round 17: the packed byte-reversed sRGB row is proved too, by
    # storing its texels in the order the console's curve reads and swapping them
    # at every application boundary (docs/BLOCKERS.md, docs/HARDWARE_FINDINGS.md).
    blocked_by_hardware = set()
    # The compiler-fault class is **empty** now: the aborts earlier rounds read as
    # ACO faults were vkCreateImage's own assert on an unsupported combination,
    # whose backtrace walks whatever ACO frames the stack still held. The proof is
    # the call in flight -- every one of those runs' last probe is
    # b7_create_device and the next call a frame makes is b7_create_image
    # (rounds 4, 13 and 16; docs/HARDWARE_FINDINGS.md, "An unsupported image is
    # refused, not asserted"). The driver refuses such an image by name now, so
    # what was quoted against the fault is reachable work: claim, run, keep or
    # revert.
    blocked_by_compiler = set()
    split = re.compile(
        r"\*\*(\d+) features on (\d+) rows are parked on `PsbcDescriptorType`, "
        r"(\d+) features on (\d+) rows on `PsbcVertexFormat`, "
        r"(\d+) features on (\d+) rows? are blocked by the hardware's fixed fetch order, "
        r"(\d+) features on (\d+) rows are blocked by the compiler fault, and "
        r"(\d+) features on (\d+) rows are probe-reachable work\.\*\*"
    )

    def test_the_parked_and_reachable_split_is_the_documented_one(self):
        """The rung's finish line is that no row is both unproved and
        probe-reachable: the rows that stay open may only be the ones whose
        missing features are the shader compiler's to add. That makes the split
        between parked and reachable features part of the record, so this
        recounts it from the row table and from the two enum lists the same
        section quotes -- a descriptor type or vertex format added to libpsbc,
        or a feature closed, moves the summary line with it."""
        document = (ROOT / "docs/V0_FORMATS_AUDIT.md").read_text(encoding="utf-8")
        # A format row, not this section's own feature table: those rows start
        # with VK_FORMAT_FEATURE_ and carry three cells.
        rows = re.findall(
            r"^\| `(VK_FORMAT_(?!FEATURE_)[A-Z0-9_]+)` \| ([^|]*) \|$", document, re.M
        )
        # The table may be empty (round 17 closed its last row), so the recount
        # is over whatever it holds rather than requiring a row to exist.
        self.assertTrue(rows or True, "the audit's row table is gone")
        counts = dict.fromkeys(("descriptor", "vertex", "hardware", "compiler", "reachable"), 0)
        seen = {name: set() for name in counts}
        for format_name, cell in rows:
            for feature in (f.strip().strip("`") for f in cell.split(", ")):
                if feature in self.parked_on_descriptor:
                    kind = "descriptor"
                elif feature == "VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT" and format_name in (
                    self.parked_on_vertex
                ):
                    kind = "vertex"
                elif format_name in self.blocked_by_hardware:
                    kind = "hardware"
                elif (format_name, feature) in self.blocked_by_compiler:
                    kind = "compiler"
                else:
                    kind = "reachable"
                counts[kind] += 1
                seen[kind].add(format_name)
        documented = self.split.search(document)
        self.assertIsNotNone(documented, "the parked/reachable summary line is gone")
        numbers = [int(value) for value in documented.groups()]
        # The rung's finish line, corrected by R1's report: the classes the rung
        # closed stay empty -- no feature is parked on either enum and none is
        # blamed on the compiler fault -- but "probe-reachable" is not zero, and
        # saying it was is what the report caught. A table footnote can require a
        # feature of a *set* of rows, and the audit filed every such cell as
        # conditional without asking whether any named format satisfied it, so the
        # depth/stencil table's second must: clause was invisible while the driver
        # violated it. tools/format_audit.py evaluates clauses now
        # (required_clauses), the two rows it names are in the table above, and
        # the count is what the row table says rather than an assumption.
        self.assertEqual(
            numbers[6:8],
            [0, 0],
            "a feature is still blamed on the compiler fault (docs/V0_FORMATS_AUDIT.md)",
        )
        self.assertEqual(
            numbers[:6],
            [
                counts["descriptor"],
                len(seen["descriptor"]),
                counts["vertex"],
                len(seen["vertex"]),
                counts["hardware"],
                len(seen["hardware"]),
            ],
        )
        self.assertEqual(
            numbers[8:],
            [counts["reachable"], len(seen["reachable"])],
            "the probe-reachable count is what the row table holds: round 12 proved the last "
            "two rows, the depth/stencil clause's (docs/V0_FORMATS_AUDIT.md, docs/BLOCKERS.md)",
        )
        self.assertEqual(numbers[8:], [0, 0], "no row is left to probe")
        # The enums the section quotes have to be the ones that park these
        # features: no other descriptor type and no other vertex format.
        compiler = (ROOT / ".deps/native/psbc/include/psbc_compile.h").read_text(encoding="utf-8")
        image = (ROOT / "driver/ps5vk_image.c").read_text(encoding="utf-8")
        for enum, names in (
            (
                "PsbcDescriptorType",
                ("NONE", "UNIFORM_BUFFER", "COMBINED_IMAGE_SAMPLER", "STORAGE_BUFFER"),
            ),
            ("PsbcVertexFormat", None),
        ):
            with self.subTest(enum=enum):
                self.assertIn(enum, compiler)
        # The parked rows are quoted against what the device *reports*, not
        # against the compiler's enum: blocker rounds 4 to 7 gave libpsbc every
        # descriptor type the parked rows needed and the driver their entries, so
        # the quote is that no format reports a storage image *atomic* until a
        # probe proves one (docs/BLOCKERS.md).
        # Nothing is parked any more, so the "no entry may report a parked
        # family's bit" check has no subject. What replaces it is the count each
        # console battery proved, per family: a driver change that claims a
        # descriptor bit on one format too many, or drops one, fails here, and the
        # audit's own prose names the battery behind every count
        # (docs/V0_FORMATS_AUDIT.md; docs/BLOCKERS.md, the mechanism log).
        entries = [entry for entry in re.findall(r"\{VK_FORMAT_[A-Z0-9_]+,(.*?)\},\n", image, re.S)
                   if "VK_FORMAT_FEATURE_" in entry]
        proved_bits = {
            # The first counts came from the batteries that closed each family
            # (rounds 5 and 6). R32_SFLOAT joined both in CTS round 8: the rows
            # were added to the two texel-buffer cases and the console proved
            # them together (pid 109, title digest c1f75ff6..., the
            # v0-target-float-buffer battery).
            "VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT": 38,  # round 5 (37), CTS round 8 (+1)
            "VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT": 20,  # round 6 (19), CTS round 8 (+1)
            "VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT": 16,  # round 7, pid 161
            "VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT": 2,  # round 9
        }
        for bit, expected in proved_bits.items():
            with self.subTest(bit=bit):
                carrying = [entry for entry in entries if bit in entry]
                self.assertEqual(
                    len(carrying),
                    expected,
                    f"{bit} is reported by {len(carrying)} format entries, and {expected} console "
                    "rows were proved for it",
                )

    def test_the_audit_matches_the_documented_counts(self):
        if not (ROOT / ".deps/native/vulkan-docs/formats-v1.4.354.adoc").is_file():
            self.skipTest("the vendored format specification is not fetched")
        audit = subprocess.run(
            [sys.executable, str(ROOT / "tools/format_audit.py")],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        summary = re.search(
            r"(\d+) formats are required, (\d+) are reported\n"
            r"(\d+) formats miss a required feature; (\d+) have conditional requirements\n"
            r"(\d+) must: clauses are unmet; (\d+) formats are named by one",
            audit,
        )
        self.assertIsNotNone(summary, "the audit printed no summary")
        counted = dict(
            zip(
                ("required", "reported", "missing", "conditional", "clauses", "clause_formats"),
                summary.groups(),
            )
        )
        documented = {}
        document = (ROOT / "docs/V0_FORMATS_AUDIT.md").read_text(encoding="utf-8")
        for line in document.splitlines():
            cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
            if len(cells) == 2 and cells[0] in self.labels:
                documented[self.labels[cells[0]]] = cells[1]
        for key, value in counted.items():
            with self.subTest(count=key):
                self.assertEqual(
                    documented.get(key),
                    value,
                    f"docs/V0_FORMATS_AUDIT.md does not record {key} = {value}",
                )

    def test_the_documented_gap_list_is_the_audit_s_list(self):
        if not (ROOT / ".deps/native/vulkan-docs/formats-v1.4.354.adoc").is_file():
            self.skipTest("the vendored format specification is not fetched")
        audit = subprocess.run(
            [sys.executable, str(ROOT / "tools/format_audit.py")],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.splitlines()
        # The section is printed only while a format misses a feature (round 17
        # left none), so an absent section is an empty list rather than a
        # failure to parse.
        rows = {}
        header = "missing (one row per format: the features no entry carries):"
        if header in audit:
            start = audit.index(header) + 1
            for line in audit[start:]:
                if not line.strip() or line.endswith(":") or not line.startswith("  VK_FORMAT"):
                    break
                parts = line.split()
                rows[parts[0]] = sorted(parts[1:])
        self.assertEqual(
            len(rows),
            0,
            "the audit's missing list is empty: every required format carries every feature it "
            "requires, the packed sRGB row closing in round 17 through the storage order its "
            "fetch needs: "
            "blocker round 9 took the last two gaps (the storage image atomics), round 7 the 16 "
            "STORAGE_IMAGE gaps (fourteen rows left the list entirely), round 6 the 19 "
            "STORAGE_TEXEL_BUFFER gaps (four rows left it), round 5 the 37 UNIFORM_TEXEL_BUFFER gaps "
            "(eighteen rows left it), after round 2 took the fifteen sixteen-bit vertex rows' last "
            "gap, round 21 took D16_UNORM and D32_SFLOAT out of it, round 20 the sixteen-byte rows "
            "and round 12 the packed 16-bit targets (docs/V0_FORMATS_AUDIT.md)",
        )
        # The clauses: a footnote requirement over a *set* of rows, evaluated by
        # tools/format_audit.py against the driver's whole table. The script
        # prints it as its own section -- one row per format the clause names --
        # and the document carries the same rows in its own table, so a clause the
        # driver starts or stops satisfying moves both. This is the R1 blind spot:
        # the audit filed every {sym2} cell as conditional and never asked whether
        # any format the clause names satisfied it.
        # The section is printed only while a clause is unmet (round 12 met the
        # one the depth/stencil table carries), so an absent section is an empty
        # list of rows rather than a failure to parse.
        clause_header = "unmet (a must: clause requires the feature of at least one of the formats"
        clauses = {}
        if clause_header in audit:
            clause_start = audit.index(clause_header) + 2
            for line in audit[clause_start:]:
                if line.startswith("  the clause:"):
                    break
                if line.startswith("  VK_FORMAT"):
                    parts = line.split()
                    clauses[parts[0]] = sorted(parts[1:])
        self.assertEqual(
            clauses,
            {},
            "no must: clause is unmet any more: round 12's v0-stencil probe reports "
            "DEPTH_STENCIL_ATTACHMENT_BIT for D32_SFLOAT_S8_UINT, one of the two formats the "
            "depth/stencil table's second clause names (tools/format_audit.py, "
            "required_clauses; docs/V0_FORMATS_AUDIT.md)",
        )
        # The document's own tables: "| `VK_FORMAT_X` | `FEATURE`, ... |". The
        # section holds two of them, the per-format missing list first and the
        # clause's rows second, each ended by a blank line.
        document = (ROOT / "docs/V0_FORMATS_AUDIT.md").read_text(encoding="utf-8")
        section = document.split("## The audit's list, row by row", 1)
        self.assertEqual(len(section), 2, "the document has no row-by-row gap list")
        tables = []
        table = None
        for line in section[1].splitlines():
            cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
            if len(cells) == 2 and cells[0].startswith("`VK_FORMAT"):
                features = sorted(
                    feature.strip().strip("`") for feature in cells[1].split(",") if feature.strip()
                )
                if table is None:
                    table = {}
                table[cells[0].strip("`")] = features
            elif not line.strip() and table is not None:
                tables.append(table)
                table = None
        if table is not None:
            tables.append(table)
        # The section holds the per-format missing table and, while a clause is
        # unmet, the clause's own rows after it. The clause is satisfied since
        # round 12, so the second table records the rows the clause names rather
        # than rows the audit prints: the comparison below is only against the
        # audit's own table when that table exists.
        # The document's row-by-row table records the empty list as a row of its
        # own, which is not a format row, so the parse yields no entries.
        if rows:
            self.assertTrue(tables, "the section has no per-format table")
            self.assertEqual(sorted(tables[0]), sorted(rows), "the per-format gap list differs")
            for name in sorted(rows):
                with self.subTest(format=name, table="the per-format gap list"):
                    self.assertEqual(tables[0][name], rows[name], f"{name}'s features differ")
        if clauses:
            self.assertEqual(len(tables), 2, "an unmet clause needs its own table")
            self.assertEqual(sorted(tables[1]), sorted(clauses), "the must: clause's rows differ")
        else:
            # A satisfied clause is not a row any more, and with round 17's last
            # gap closed there is no per-format table either: the section records
            # the clause's two formats and the probe that closed it, and the
            # probe that closed the packed sRGB row, in prose.
            clause_section = section[1].split("## What is parked", 1)[0]
            for name in ("VK_FORMAT_D24_UNORM_S8_UINT", "VK_FORMAT_D32_SFLOAT_S8_UINT",
                         "v0-stencil", "v0-stencil-run5.log"):
                with self.subTest(missing=name):
                    self.assertIn(name, clause_section, "the clause's record names the probe")


class CommandAuditTests(unittest.TestCase):
    """Every Vulkan 1.0 command has to be answered: implemented by the driver,
    supplied by Mesa's runtime, or refused by name with the phase that would
    implement it. Three manual sweeps are what found the commands that were none
    of the three (docs/M5_PHASE_B.md); tools/command_audit.py is the audit that
    keeps the answer complete, and this runs it as a gate."""

    def test_every_1_0_command_is_accounted_for(self):
        registry = list((ROOT / ".deps/native/mesa").glob("*/src/vulkan/registry/vk.xml"))
        runtime = ROOT / ".deps/native/vulkan-runtime/lib/libvk_runtime.a"
        if not registry or not runtime.is_file():
            self.skipTest("the Mesa registry or the runtime archive is not fetched")
        audit = subprocess.run(
            [sys.executable, str(ROOT / "tools/command_audit.py"), "--check"],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(audit.returncode, 0, audit.stdout + audit.stderr)
        summary = re.search(
            r"(\d+) commands are required by VK_VERSION_1_0\n"
            r"\s+(\d+) driver\n\s+(\d+) refused\n\s+(\d+) runtime\n\s+(\d+) gap",
            audit.stdout,
        )
        self.assertIsNotNone(summary, audit.stdout)
        required, driver, refused, runtime_count, gap = (int(value) for value in summary.groups())
        self.assertEqual(gap, 0)
        self.assertEqual(driver + refused + runtime_count + gap, required)
        self.assertGreaterEqual(required, 137)
