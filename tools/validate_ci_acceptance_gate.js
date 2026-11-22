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
 *   CI_GATE_RUN_JSON='...' CI_GATE_JOBS_JSON='...' \
 *   CI_GATE_EXPECTED_HEAD_SHA='<40-hex>' \
 *   CI_GATE_EXPECTED_BASE_SHA='<40-hex-for-PR>' \
 *   CI_GATE_EXPECTED_PR_NUMBER='<PR-number-for-PR>' \
 *   CI_GATE_EXPECTED_RUN_ID='<run-id>' CI_GATE_EXPECTED_RUN_ATTEMPT='1' \
 *     node tools/validate_ci_acceptance_gate.js --verify-run
 *   --verify-run is ONLY valid after the whole GitHub run completes.
 *   Use read-only GitHub REST run and run-attempt jobs JSON as inputs.
 *   --check verifies prerequisites only, NOT the final workflow result.
 * The --check input is GitHub's needs context, not job logs.
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
                failures.push('dependency-review unexpectedly ran outside a PR');
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


const CI_JOB_NAMES = Object.freeze([
    'Host Automated Validators',
    'Build Firmware Binary & Validate OTA',
    'Dependency Review',
    'Conductor Acceptance Gate',
]);

// This must run AFTER GitHub marks the whole workflow complete.
// A job inside the same run cannot know that run's final conclusion.
function verifyCompletedRun(run, page, expected) {
    const failures = [];
    const isObject = (value) => value !== null &&
        typeof value === 'object' && !Array.isArray(value);
    const isSha = (value) => typeof value === 'string' &&
        /^[0-9a-f]{40}$/.test(value);

    if (!isObject(expected) || !isSha(expected.head) ||
            !Number.isSafeInteger(expected.id) || expected.id <= 0 ||
            !Number.isSafeInteger(expected.attempt) || expected.attempt <= 0) {
        failures.push('Invalid expected run identity');
    }
    if (!isObject(run)) {
        failures.push('Missing workflow run');
        return { passed: false, failures };
    }
    if (!EVENTS.includes(run.event) || run.name !== 'CI' ||
            run.path !== '.github/workflows/ci.yml') {
        failures.push('Wrong CI workflow or event');
    }
    if (run.status !== 'completed' || run.conclusion !== 'success') {
        failures.push('Whole workflow run did not complete successfully');
    }
    if (isObject(expected) && (run.id !== expected.id ||
            run.run_attempt !== expected.attempt ||
            run.head_sha !== expected.head)) {
        failures.push('Run ID, attempt or HEAD mismatch');
    }
    if (run.event === 'pull_request' &&
            (!isObject(expected) || !isSha(expected.base) ||
            !Number.isSafeInteger(expected.prNumber) || expected.prNumber <= 0 ||
            !Array.isArray(run.pull_requests) ||
            !run.pull_requests.some((pr) => isObject(pr) &&
                pr.number === expected.prNumber &&
                isObject(pr.head) && isObject(pr.base) &&
                pr.head.sha === expected.head && pr.base.sha === expected.base))) {
        failures.push('PR number/head/base association missing or stale');
    }
    if (run.event !== 'pull_request' && isObject(expected) &&
            expected.prNumber !== null) {
        failures.push('Unexpected PR number for non-PR event');
    }
    if (!isObject(page) || !Array.isArray(page.jobs) ||
            page.total_count !== CI_JOB_NAMES.length ||
            page.jobs.length !== CI_JOB_NAMES.length) {
        failures.push('Incomplete or paginated workflow jobs');
        return { passed: false, failures };
    }

    const seen = new Set();
    const seenJobIds = new Set();
    for (const job of page.jobs) {
        if (!isObject(job) || !CI_JOB_NAMES.includes(job.name) ||
                seen.has(job.name)) {
            failures.push('Unexpected or duplicated workflow job');
            continue;
        }
        seen.add(job.name);
        if (!Number.isSafeInteger(job.id) || job.id <= 0 ||
                seenJobIds.has(job.id)) {
            failures.push('Missing, invalid or duplicated numeric job ID: ' +
                job.name);
        } else {
            seenJobIds.add(job.id);
        }
        if (!isObject(expected) || job.run_id !== expected.id ||
                job.run_attempt !== expected.attempt ||
                job.head_sha !== expected.head || job.workflow_name !== 'CI') {
            failures.push('Job identity mismatch: ' + job.name);
        }
        const nonPrDependency = job.name === 'Dependency Review' &&
            run.event !== 'pull_request' && EVENTS.includes(run.event);
        if (nonPrDependency && job.conclusion !== 'skipped') {
            failures.push('dependency-review must be skipped (N/A) outside PR');
        } else if (job.status !== 'completed' ||
                (!nonPrDependency && job.conclusion !== 'success')) {
            failures.push('Job not successful: ' + job.name);
        }
    }
    for (const name of CI_JOB_NAMES) {
        if (!seen.has(name)) {
            failures.push('Required workflow job missing: ' + name);
        }
    }
    return { passed: failures.length === 0, failures };
}

function runCompletedWorkflowTests() {
    let count = 0;
    const head = 'a'.repeat(40);
    const base = 'b'.repeat(40);
    function fixture(eventName = 'pull_request') {
        const expected = {
            head, base, id: 123, attempt: 2,
            prNumber: eventName === 'pull_request' ? 21 : null,
        };
        const run = {
            id: 123, run_attempt: 2, head_sha: head,
            name: 'CI', path: '.github/workflows/ci.yml',
            status: 'completed', conclusion: 'success', event: eventName,
            pull_requests: [{ number: 21, head: { sha: head }, base: { sha: base } }],
        };
        const page = {
            total_count: 4,
            jobs: CI_JOB_NAMES.map((name, index) => ({
                id: 1001 + index, name, status: 'completed',
                conclusion: name === 'Dependency Review' &&
                    eventName !== 'pull_request' ? 'skipped' : 'success',
                run_id: 123, run_attempt: 2, head_sha: head,
                workflow_name: 'CI',
            })),
        };
        return { run, page, expected };
    }
    function expectCase(label, mutate, shouldPass, message = '',
            eventName = 'pull_request') {
        const data = fixture(eventName);
        mutate(data);
        const actual = verifyCompletedRun(data.run, data.page, data.expected);
        if (actual.passed !== shouldPass ||
                (message && !actual.failures.some((x) => x.includes(message)))) {
            throw new Error('Workflow-run fixture failed: ' + label +
                ' / ' + JSON.stringify(actual));
        }
        count += 1;
    }
    for (const eventName of EVENTS) {
        const { run, page, expected } = fixture(eventName);
        if (!verifyCompletedRun(run, page, expected).passed) {
            throw new Error('Valid completed ' + eventName + ' run rejected');
        }
        count += 1;
    }
    for (const status of ['queued', 'in_progress', 'pending', 'waiting']) {
        expectCase('unfinished ' + status, (d) => { d.run.status = status; },
            false, 'Whole workflow');
    }
    for (const conclusion of ['cancelled', 'failure', 'timed_out', 'skipped',
        'neutral', null]) {
        expectCase('run ' + conclusion, (d) => {
            d.run.conclusion = conclusion;
        }, false, 'Whole workflow');
    }
    // Regression: every individual job is green, but the whole run was cancelled.
    expectCase('green gate in cancelled run', (d) => {
        d.run.conclusion = 'cancelled';
    }, false, 'Whole workflow');
    for (const [field, value] of [
        ['head_sha', 'c'.repeat(40)], ['id', 999],
        ['run_attempt', 1], ['name', 'Other CI'],
        ['path', '.github/workflows/other.yml'], ['event', 'workflow_run'],
    ]) {
        expectCase('bad run ' + field, (d) => { d.run[field] = value; },
            false, ['name', 'path', 'event'].includes(field) ?
                'Wrong CI' : 'mismatch');
    }
    for (const [field, value] of [
        ['head', 'c'.repeat(40)], ['id', 999],
        ['attempt', 1], ['base', 'c'.repeat(40)],
    ]) {
        expectCase('stale expected ' + field, (d) => {
            d.expected[field] = value;
        }, false, field === 'base' ? 'association' : 'mismatch');
    }
    expectCase('lost PR association', (d) => {
        d.run.pull_requests = [];
    }, false, 'association');
    expectCase('invalid run object', (d) => { d.run = null; },
        false, 'Missing workflow');
    for (const name of CI_JOB_NAMES) {
        for (const result of ['failure', 'cancelled', 'skipped', 'neutral',
            'timed_out', null]) {
            expectCase(name + ' result ' + result, (d) => {
                d.page.jobs.find((j) => j.name === name).conclusion = result;
            }, false, 'Job not successful');
        }
        expectCase(name + ' still running', (d) => {
            d.page.jobs.find((j) => j.name === name).status = 'in_progress';
        }, false, 'Job not successful');
    }
    for (const [field, value] of [
        ['head_sha', 'c'.repeat(40)], ['run_id', 7],
        ['run_attempt', 1], ['workflow_name', 'Other'],
    ]) {
        expectCase('job identity ' + field, (d) => {
            d.page.jobs[0][field] = value;
        }, false, 'Job identity');
    }
    expectCase('jobs missing', (d) => { d.page.jobs.pop(); },
        false, 'Incomplete');
    expectCase('wrong total', (d) => { d.page.total_count = 100; },
        false, 'Incomplete');
    expectCase('duplicate job', (d) => {
        d.page.jobs[0].name = d.page.jobs[1].name;
    }, false, 'duplicated');
    expectCase('unknown job', (d) => {
        d.page.jobs[0].name = 'Unknown';
    }, false, 'Unexpected');
    expectCase('missing page', (d) => { d.page = null; },
        false, 'Incomplete');
    expectCase('malformed expected', (d) => { d.expected.id = 0; },
        false, 'Invalid expected');

    // Match post-run behavior to the earlier in-job event contract.
    for (const eventName of ['push', 'workflow_dispatch']) {
        for (const result of ['success', 'failure', 'cancelled', 'neutral',
            'timed_out', null]) {
            expectCase(eventName + ' invalid dependency-review ' + result,
                (d) => {
                    d.page.jobs.find((j) => j.name === 'Dependency Review')
                        .conclusion = result;
                }, false, 'must be skipped (N/A)', eventName);
        }
        expectCase(eventName + ' unexpected PR number', (d) => {
            d.expected.prNumber = 21;
        }, false, 'Unexpected PR number', eventName);
    }

    expectCase('wrong PR number in run', (d) => {
        d.run.pull_requests[0].number = 22;
    }, false, 'PR number/head/base');
    expectCase('wrong expected PR number', (d) => {
        d.expected.prNumber = 22;
    }, false, 'PR number/head/base');
    for (const invalid of [undefined, null, 0, -1, 1.5, '21']) {
        expectCase('invalid expected PR number ' + String(invalid), (d) => {
            d.expected.prNumber = invalid;
        }, false, 'PR number/head/base');
    }
    expectCase('disjoint multi-PR records', (d) => {
        d.run.pull_requests = [
            { number: 21, head: { sha: 'c'.repeat(40) },
                base: { sha: base } },
            { number: 22, head: { sha: head }, base: { sha: base } },
        ];
    }, false, 'PR number/head/base');
    const matchedMultiple = fixture();
    matchedMultiple.run.pull_requests.unshift({
        number: 999, head: { sha: head }, base: { sha: base },
    });
    if (!verifyCompletedRun(matchedMultiple.run, matchedMultiple.page,
            matchedMultiple.expected).passed) {
        throw new Error('Matching PR rejected in multi-PR association');
    }
    count += 1;

    for (const value of [undefined, null, 0, -2, 1.5, '1001',
        Number.MAX_SAFE_INTEGER + 1]) {
        expectCase('invalid numeric job ID ' + String(value), (d) => {
            d.page.jobs[0].id = value;
        }, false, 'numeric job ID');
    }
    expectCase('duplicate numeric job ID', (d) => {
        d.page.jobs[1].id = d.page.jobs[0].id;
    }, false, 'numeric job ID');

    // Exercise the production --verify-run input handling, not a mock
    // implementation of its decision policy.
    function checkRecorded(label, mutate, shouldPass, diagnostic) {
        const d = fixture();
        mutate(d);
        const logs = [];
        const errors = [];
        const output = {
            log(message) { logs.push(message); },
            error(message) { errors.push(message); },
        };
        const env = {
            CI_GATE_RUN_JSON: JSON.stringify(d.run),
            CI_GATE_JOBS_JSON: JSON.stringify(d.page),
            CI_GATE_EXPECTED_HEAD_SHA: d.expected.head,
            CI_GATE_EXPECTED_BASE_SHA: d.expected.base,
            CI_GATE_EXPECTED_PR_NUMBER: String(d.expected.prNumber),
            CI_GATE_EXPECTED_RUN_ID: String(d.expected.id),
            CI_GATE_EXPECTED_RUN_ATTEMPT: String(d.expected.attempt),
        };
        let thrown = false;
        try {
            verifyRecordedRun(env, output);
        } catch (error) {
            thrown = true;
            if (shouldPass ||
                    !error.message.includes('Whole workflow acceptance rejected')) {
                throw new Error('Incorrect --verify-run error: ' + label +
                    ' / ' + error.message);
            }
        }
        if (thrown === shouldPass ||
                (shouldPass && (logs.length !== 1 ||
                logs[0] !== 'PASS: completed successful CI workflow, exact HEAD and jobs')) ||
                (!shouldPass && !errors.some((x) => x.includes(diagnostic)))) {
            throw new Error('Wrong --verify-run result: ' + label);
        }
        count += 1;
    }
    checkRecorded('real CLI path PR success', () => {}, true, '');
    checkRecorded('real CLI path wrong PR number', (d) => {
        d.run.pull_requests[0].number = 22;
    }, false, 'PR number/head/base');
    checkRecorded('real CLI path duplicate ID', (d) => {
        d.page.jobs[1].id = d.page.jobs[0].id;
    }, false, 'numeric job ID');
    checkRecorded('real CLI path missing ID', (d) => {
        delete d.page.jobs[0].id;
    }, false, 'numeric job ID');
    checkRecorded('real CLI path cancelled overall', (d) => {
        d.run.conclusion = 'cancelled';
    }, false, 'Whole workflow run did not complete successfully');
    checkRecorded('real CLI path wrong expected PR', (d) => {
        d.expected.prNumber = 22;
    }, false, 'PR number/head/base');
    return count;
}

// Read-only verification by an operator/reviewer after the entire CI run ends.
function verifyRecordedRun(env = process.env, output = console) {
    const run = parseNeeds(env.CI_GATE_RUN_JSON);
    const jobs = parseNeeds(env.CI_GATE_JOBS_JSON);
    const id = env.CI_GATE_EXPECTED_RUN_ID || '';
    const attempt = env.CI_GATE_EXPECTED_RUN_ATTEMPT || '';
    const prNumber = env.CI_GATE_EXPECTED_PR_NUMBER || '';
    if (!/^[1-9]\d*$/.test(id) || !/^[1-9]\d*$/.test(attempt)) {
        throw new Error('Missing or invalid expected run ID/attempt');
    }
    if ((run.event === 'pull_request' && !/^[1-9]\d*$/.test(prNumber)) ||
            (run.event !== 'pull_request' && prNumber !== '')) {
        throw new Error('Expected PR number invalid or not applicable');
    }
    const expected = {
        head: env.CI_GATE_EXPECTED_HEAD_SHA,
        base: env.CI_GATE_EXPECTED_BASE_SHA,
        prNumber: prNumber === '' ? null : Number(prNumber),
        id: Number(id),
        attempt: Number(attempt),
    };
    const verdict = verifyCompletedRun(run, jobs, expected);
    if (!verdict.passed) {
        for (const reason of verdict.failures) {
            output.error('[FAIL] ' + reason);
        }
        throw new Error('Whole workflow acceptance rejected');
    }
    output.log('PASS: completed successful CI workflow, exact HEAD and jobs');
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
                eventName, needs, false, 'dependency-review');
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

    cases += runCompletedWorkflowTests();
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

// The observer imports this same policy; importing must never execute the CLI.
module.exports = Object.freeze({ verifyCompletedRun });

if (require.main === module) {
try {
    if (process.argv.length !== 3) {
        throw new Error('Use --test or --check');
    }
    if (process.argv[2] === '--test') {
        runTests();
    } else if (process.argv[2] === '--check') {
        checkLive();
    } else if (process.argv[2] === '--verify-run') {
        verifyRecordedRun();
    } else {
        throw new Error('Unsupported mode: ' + process.argv[2]);
    }
} catch (error) {
    console.error('[FAIL] CI acceptance validation: ' + error.message);
    process.exitCode = 1;
}
}
