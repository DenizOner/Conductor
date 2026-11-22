#!/usr/bin/env node
'use strict';

/*
 * Fail-closed CI acceptance for all Conductor pull requests.
 *
 * Usage:
 *   node tools/validate_ci_acceptance_gate.js --test
 *   CI_GATE_EVENT=pull_request CI_GATE_NEEDS_JSON='...' \
 *     node tools/validate_ci_acceptance_gate.js --check
 *
 * The input is the GitHub Actions needs context, not untrusted job logs.
 * Dependency Review is intentionally not applicable to push/dispatch runs.
 * A skipped job on a pull request is NEVER treated as successful.
 * No external packages, network requests or state changes are required.
 */

const JOBS = Object.freeze([
    'host-validators',
    'build-firmware',
    'dependency-review',
]);
const EVENTS = Object.freeze([
    'pull_request',
    'push',
    'workflow_dispatch',
]);

function evaluate(eventName, needs) {
    const messages = [];
    const failures = [];

    if (!EVENTS.includes(eventName)) {
        failures.push('Unknown or missing CI event');
    }
    if (needs === null || typeof needs !== 'object' || Array.isArray(needs)) {
        failures.push('Missing or invalid needs object');
        return { passed: false, messages, failures };
    }

    const actualJobs = Object.keys(needs);
    for (const id of JOBS) {
        if (!Object.prototype.hasOwnProperty.call(needs, id)) {
            failures.push('Missing required job: ' + id);
        }
    }
    for (const id of actualJobs) {
        if (!JOBS.includes(id)) {
            failures.push('Unexpected dependency: ' + id);
        }
    }

    for (const id of JOBS) {
        if (!Object.prototype.hasOwnProperty.call(needs, id)) {
            continue;
        }
        const job = needs[id];
        const result = job !== null && typeof job === 'object' &&
            !Array.isArray(job) ? job.result : undefined;

        if (id === 'dependency-review' && eventName !== 'pull_request' &&
                EVENTS.includes(eventName) && result === 'skipped') {
            messages.push('[N/A] ' + id + ': only runs for pull requests');
        } else if (result === 'success') {
            if (id === 'dependency-review' && eventName !== 'pull_request') {
                failures.push('Dependency Review unexpectedly ran outside a PR');
            } else {
                messages.push('[PASS] ' + id);
            }
        } else {
            failures.push('Job ' + id + ' must succeed, got ' +
                (typeof result === 'string' ? result : 'missing/invalid'));
        }
    }

    return { passed: failures.length === 0, messages, failures };
}

function parseNeeds(raw) {
    if (typeof raw !== 'string' || raw.trim() === '') {
        throw new Error('CI_GATE_NEEDS_JSON is missing');
    }
    const needs = JSON.parse(raw);
    if (needs === null || typeof needs !== 'object' || Array.isArray(needs)) {
        throw new Error('CI_GATE_NEEDS_JSON must contain an object');
    }
    return needs;
}

function runTests() {
    let cases = 0;
    function fixture() {
        return {
            'host-validators': { result: 'success' },
            'build-firmware': { result: 'success' },
            'dependency-review': { result: 'success' },
        };
    }
    function expectCase(label, eventName, needs, expected, fragment) {
        const actual = evaluate(eventName, needs);
        if (actual.passed !== expected ||
                (fragment && !(actual.failures.concat(actual.messages)
                    .some((value) => value.includes(fragment))))) {
            throw new Error('Fixture failed: ' + label + ' / ' +
                JSON.stringify(actual));
        }
        cases += 1;
    }

    expectCase('PR success', 'pull_request', fixture(), true, '[PASS]');
    for (const eventName of ['push', 'workflow_dispatch']) {
        const needs = fixture();
        needs['dependency-review'].result = 'skipped';
        expectCase(eventName + ' explicit N/A', eventName, needs, true, '[N/A]');
    }

    const invalidResults = [
        'failure', 'cancelled', 'skipped', 'neutral',
        'timed_out', 'pending', 'queued', '',
    ];
    for (const id of JOBS) {
        for (const result of invalidResults) {
            const needs = fixture();
            needs[id].result = result;
            expectCase('PR ' + id + ' ' + result, 'pull_request',
                needs, false, 'Job ' + id);
        }
    }
    for (const id of ['host-validators', 'build-firmware']) {
        for (const result of invalidResults) {
            const needs = fixture();
            needs[id].result = result;
            needs['dependency-review'].result = 'skipped';
            expectCase('push ' + id + ' ' + result, 'push',
                needs, false, 'Job ' + id);
        }
    }
    for (const eventName of ['push', 'workflow_dispatch']) {
        for (const result of ['success', 'failure', 'cancelled', 'neutral']) {
            const needs = fixture();
            needs['dependency-review'].result = result;
            expectCase(eventName + ' non-PR Dependency Review ' + result,
                eventName, needs, false, 'Dependency Review');
        }
    }

    for (const id of JOBS) {
        const missing = fixture();
        delete missing[id];
        expectCase('missing ' + id, 'pull_request', missing, false, id);

        const invalid = fixture();
        invalid[id] = null;
        expectCase('invalid ' + id, 'pull_request', invalid, false, id);

        const noResult = fixture();
        noResult[id] = {};
        expectCase('undefined result ' + id, 'pull_request',
            noResult, false, id);
    }
    const unexpected = fixture();
    unexpected['unreviewed-job'] = { result: 'success' };
    expectCase('unexpected dependency', 'pull_request', unexpected,
        false, 'Unexpected dependency');
    expectCase('null needs', 'pull_request', null, false, 'needs object');
    expectCase('array needs', 'pull_request', [], false, 'needs object');
    expectCase('unknown event', 'workflow_run', fixture(), false, 'Unknown');
    expectCase('empty event', '', fixture(), false, 'Unknown');

    const valid = JSON.stringify(fixture());
    if (!evaluate('pull_request', parseNeeds(valid)).passed) {
        throw new Error('Valid JSON integration fixture was rejected');
    }
    cases += 1;
    for (const raw of ['', '{', 'null', '[]', '""']) {
        let failed = false;
        try {
            parseNeeds(raw);
        } catch (error) {
            failed = true;
        }
        if (!failed) {
            throw new Error('Malformed JSON was accepted: ' + raw);
        }
        cases += 1;
    }

    console.log('PASS: CI acceptance gate positive/negative tests (' +
        cases + ' cases)');
}

function checkLive() {
    const needs = parseNeeds(process.env.CI_GATE_NEEDS_JSON);
    const assessment = evaluate(process.env.CI_GATE_EVENT, needs);
    for (const message of assessment.messages) {
        console.log(message);
    }
    if (!assessment.passed) {
        for (const failure of assessment.failures) {
            console.error('[FAIL] ' + failure);
        }
        throw new Error('Conductor Acceptance Gate is blocked');
    }
    console.log('PASS: Conductor Acceptance Gate');
}

try {
    if (process.argv.length !== 3) {
        throw new Error('Use --test or --check');
    }
    if (process.argv[2] === '--test') {
        runTests();
    } else if (process.argv[2] === '--check') {
        checkLive();
    } else {
        throw new Error('Unsupported mode: ' + process.argv[2]);
    }
} catch (error) {
    console.error('[FAIL] CI acceptance validation: ' + error.message);
    process.exitCode = 1;
}
