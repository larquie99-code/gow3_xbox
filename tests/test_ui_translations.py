"""Check UI catalog completeness and printf safety without a Windows SDK."""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LITERAL = r'"(?:\\.|[^"\\])*"'
GROUP = LITERAL + r'(?:\s*' + LITERAL + r')*'


def strings(source):
    return [''.join(json.loads(s) for s in re.findall(LITERAL, m[0]))
            for m in re.finditer(GROUP, source)]


class UiTranslations(unittest.TestCase):
    def test_catalog_and_format_arguments(self):
        header = (ROOT / 'gpu/shim/spartanport_ui_language.h').read_text()
        entries = [strings(line) for line in header.splitlines() if line.startswith('    {"')]
        self.assertGreater(len(entries), 200)
        keys = [e[0] for e in entries]
        self.assertEqual(keys, sorted(set(keys)))
        spec = r'%(?:[-+#0]*\d*(?:\.\d+)?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcsp]|%)'
        for en, pt, es in entries:
            for translated in (pt, es):
                self.assertTrue(translated.strip(), en)
                self.assertEqual(re.findall(spec, en), re.findall(spec, translated), en)
        # Technical labels, IDs, config tokens and controller glyphs are intentionally invariant.
        invariant = {'xbox', 'nintendo', 'name', 'value', 'FSR 3.1', 'FSR 4', 'FSR 4.1.1',
                     'TAA', 'DLSS', 'native', 'SpartanPort', 's', '  (Insert / R3+L2)',
                     '  (Insert / RS+LT)', '  (Insert / RS+ZL)'}
        source = (ROOT / 'gpu/shim/spartanport_overlay.cpp').read_text()
        ui = source[source.index('constexpr const char* RestartNote'):source.index('s64 NowMs()')]
        missing = {s for s in strings(ui) if re.search('[A-Za-z]{3}', s)
                   and not s.startswith(('##', 'SPARTAN_')) and s not in invariant
                   and not re.fullmatch(r'[\dKpx ×·:./% -]+(?:\(beta\))?', s) and s not in keys}
        self.assertEqual(missing, set())
        profiles = (ROOT / 'gpu/shim/spartanport_xbox_profiles.h').read_text()
        for name in re.findall(r'Profile\{"([^"\n]+)"', profiles):
            self.assertIn(name, keys)
        options = (ROOT / 'gpu/shim/spartanport_xbox_renderer_options.h').read_text()
        for name in ('MemoryNames', 'StagingNames'):
            block = options.split(name + '{', 1)[1].split('};', 1)[0]
            self.assertTrue(set(strings(block)) <= set(keys))


if __name__ == '__main__':
    unittest.main()
