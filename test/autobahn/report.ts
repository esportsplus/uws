import { readFileSync } from 'node:fs';


type CaseResult = {
    behavior: string;
    behaviorClose: string;
};


const BAD = new Set(['FAILED', 'WRONG CODE', 'UNCLEAN']);

const PATH = process.argv[2] ?? '';


function main(): void {
    let report = JSON.parse(readFileSync(PATH, 'utf8')) as Record<string, Record<string, CaseResult>>,
        agent = Object.keys(report)[0],
        cases = report[agent] ?? {},
        failures: string[] = [],
        passed = 0;

    for (let id of Object.keys(cases).sort()) {
        let result = cases[id];

        if (BAD.has(result.behavior) || BAD.has(result.behaviorClose)) {
            failures.push(`${id}: behavior=${result.behavior} close=${result.behaviorClose}`);
        }
        else {
            passed++;
        }
    }

    console.log(`Autobahn (${agent}): ${passed} passed, ${failures.length} failed of ${passed + failures.length}`);

    for (let failure of failures) {
        console.log(`  FAIL ${failure}`);
    }

    process.exit(failures.length ? 1 : 0);
}


main();
