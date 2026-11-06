# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Asking Muse from a phone (muse_remote.c): the key check and how a question
is read from a request body, cut out of the source and run on the host."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get(
    'CJSON_SOURCE_DIR', ROOT / 'managed_components/espressif__cjson/cJSON'
))


class RemoteAsk(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        source = (ROOT / 'components/muse/muse_remote.c').read_text()
        parsing = source[source.index('static bool blank('):source.index('static esp_err_t answer(')]
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#define PSRAM_CAPS 0
#define heap_caps_malloc(n, caps) malloc(n)
''' + parsing + r'''
static void expect_reply(const char *body, const char *want, bool want_reply) {
    bool reply = !want_reply;
    char *got = question(body, &reply);
    if (!want) {
        assert(!got);
        return;
    }
    assert(got && !strcmp(got, want) && reply == want_reply);
    free(got);
}
static void expect(const char *body, const char *want) {
    expect_reply(body, want, false);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0:
        assert(key_ok("maple-orbit-velvet", "maple-orbit-velvet"));
        assert(!key_ok("maple-orbit-velve", "maple-orbit-velvet"));
        assert(!key_ok("maple-orbit-velvet!", "maple-orbit-velvet"));
        assert(!key_ok("Maple-orbit-velvet", "maple-orbit-velvet"));
        assert(!key_ok("", "maple-orbit-velvet"));
        break;
    case 1:
        expect("{\"text\": \"  What's the weather tomorrow?\\n\"}", "What's the weather tomorrow?");
        expect("  {\"text\":\"Caf\\u00e9 hours?\"}", "Caf\xc3\xa9 hours?");
        expect("  Turn off the kitchen lights \r\n", "Turn off the kitchen lights");
        expect_reply("{\"text\": \"Any traffic?\", \"reply\": true}", "Any traffic?", true);
        expect_reply("{\"text\": \"Any traffic?\", \"reply\": false}", "Any traffic?", false);
        expect_reply("{\"text\": \"Any traffic?\", \"reply\": \"yes\"}", "Any traffic?", false);
        break;
    case 2:
        expect("{\"question\": \"no text member\"}", NULL);
        expect("{\"text\": 42}", NULL);
        expect("{\"text\": \"   \"}", NULL);
        expect("{not json", NULL);
        expect(" \r\n\t ", NULL);
        expect("", NULL);
        break;
    default:
        return 2;
    }
    return 0;
}
'''
        (out / 'remote.c').write_text(code)
        flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(JSON)]
        commands = [
            [*shlex.split(os.environ.get('CC', 'cc')), *flags,
             str(out / 'remote.c'), str(JSON / 'cJSON.c'), '-o', str(out / 'remote')],
        ]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
        cls.binary = out / 'remote'

    def run_case(self, case):
        result = subprocess.run([str(self.binary), str(case)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_key_must_match_exactly(self):
        self.run_case(0)

    def test_question_from_json_or_plain_text_is_trimmed_and_may_want_the_reply(self):
        self.run_case(1)

    def test_no_question_is_refused(self):
        self.run_case(2)


if __name__ == '__main__':
    unittest.main()
