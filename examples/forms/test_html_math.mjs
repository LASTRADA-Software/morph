// SPDX-License-Identifier: Apache-2.0
//
// Unit-tests the demo page's *actual shipped* JS math: emits the page via the
// demo binary, extracts the marked pure-helper block verbatim, and drives it.
// Usage: node test_html_math.mjs <path-to-morph_forms_demo>

import { execFileSync } from 'node:child_process';
import { readFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const demo = process.argv[2];
if (!demo) {
    console.error('usage: node test_html_math.mjs <morph_forms_demo>');
    process.exit(2);
}

const page = join(mkdtempSync(join(tmpdir(), 'morph-forms-')), 'page.html');
execFileSync(demo, ['--emit-html', page]);
const html = readFileSync(page, 'utf8');

const schemas = JSON.parse(html.match(/const SCHEMAS = (\{[\s\S]*?\});\nconst OPTIONS/)[1]);
const options = JSON.parse(html.match(/const OPTIONS = (\{[\s\S]*?\});\n/)[1]);
const math = html.match(/\/\/ __MORPH_MATH_BEGIN__[^\n]*\n([\s\S]*?)\/\/ __MORPH_MATH_END__/)[1];

// eslint-disable-next-line no-new-func — evaluating our own emitted page code.
const api = new Function(
    math + '; return { resolve, typeSet, scaledBig, rationalJson, divRoundBig, formatScaled, convertText };')();

let failures = 0;
const eq = (actual, expected, what) => {
    if (actual !== expected) {
        console.error(`FAIL ${what}: ${JSON.stringify(actual)} !== ${JSON.stringify(expected)}`);
        failures += 1;
    }
};

// --- exact payload assembly (canonical, ratio-folded, beyond 2^53) ---------
eq(api.rationalJson('2650.5', { decimals: 1, num: 1, den: 1 }, 1), '{"num":26505,"den":10,"dp":1}',
   'canonical payload');
eq(api.rationalJson('2650500.0', { decimals: 1, num: 1, den: 1000 }, 3), '{"num":26505000,"den":10000,"dp":3}',
   'grams payload folds the ratio');
eq(api.rationalJson('123456789012345678.9', { decimals: 1, num: 1, den: 1 }, 1),
   '{"num":1234567890123456789,"den":10,"dp":1}', 'digit-exact beyond 2^53');
eq(api.rationalJson('-0.5', { decimals: 1, num: 1, den: 1 }, 1), '{"num":-5,"den":10,"dp":1}', 'negative payload');

// --- unit switching: exact recalculation with half-up rounding -------------
const kg = { decimals: 3, num: 1, den: 1 };
const g = { decimals: 1, num: 1, den: 1000 };
const tonne = { decimals: 4, num: 1000, den: 1 };
eq(api.convertText('2650.5', kg, g), '2650500.0', 'kg -> g');
eq(api.convertText('2650500.0', g, kg), '2650.500', 'g -> kg');
eq(api.convertText('2650.5', kg, tonne), '2.6505', 'kg -> t');
eq(api.convertText('2.6505', tonne, g), '2650500.0', 't -> g');
eq(api.convertText('-0.001', kg, g), '-1.0', 'negative conversion');
eq(api.convertText('0.05', { decimals: 2, num: 1, den: 1 }, { decimals: 1, num: 1, den: 1 }), '0.1',
   'rounds half up');
eq(api.convertText('-0.05', { decimals: 2, num: 1, den: 1 }, { decimals: 1, num: 1, den: 1 }), '-0.1',
   'rounds half away from zero');
eq(api.convertText('junk', kg, g), '', 'malformed input yields empty');

// --- primitive helpers ------------------------------------------------------
eq(api.formatScaled(-5n, 1), '-0.5', 'formatScaled negative');
eq(api.formatScaled(26505n, 0), '26505', 'formatScaled integer');
eq(api.divRoundBig(-5n, 2n), -3n, 'divRoundBig half away from zero');
eq(api.scaledBig('2650.5', 3), 2650500n, 'scaledBig pads');

// --- schema helpers + embedded data sanity ----------------------------------
const resolved = api.resolve({ $defs: { d: { type: 'integer', minimum: 1 } } }, { $ref: '#/$defs/d', minimum: 5 });
eq(resolved.minimum, 5, 'field attributes win over $def');
eq(api.typeSet(resolved).has('integer'), true, 'typeSet');
eq(options.ListSamples.length, 3, 'options resolved at emit time');
eq(schemas.RecordMeasurement.required.join(','), 'sampleId,measuredAt,density', 'required array');
eq(schemas.ComputeDryDensity.properties.massDry['x-unitAlternatives'].length, 2, 'unit alternatives embedded');

if (failures === 0) {
    console.log('html math: all assertions passed');
}
process.exit(failures === 0 ? 0 : 1);
