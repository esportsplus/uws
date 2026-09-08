import { appendFileSync, existsSync, readFileSync } from 'node:fs';


const path = process.argv[2];

if (!path || !existsSync(path)) {
    console.log('[uws/test-timings] No test report available.');
    process.exit(0);
}

const report = JSON.parse(readFileSync(path, 'utf8'));
const tests = report.testResults.flatMap((suite) => suite.assertionResults)
    .filter((test) => typeof test.duration === 'number')
    .sort((a, b) => b.duration - a.duration)
    .slice(0, 15);
const summary = [
    'Slowest tests (seconds)',
    '',
    '| Test | Duration |',
    '| --- | ---: |',
    ...tests.map((test) => `| ${test.fullName.replaceAll('|', '\\|').replace(/[\r\n]/g, ' ')} | ${(test.duration / 1000).toFixed(2)} |`),
    ''
].join('\n');

console.log(summary);

if (process.env.GITHUB_STEP_SUMMARY) {
    appendFileSync(process.env.GITHUB_STEP_SUMMARY, summary);
}
