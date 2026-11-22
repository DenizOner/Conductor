#!/usr/bin/env node
'use strict';

// Read-only CI workflow_run observer. It NEVER writes commit statuses.
// A future publisher requires separate Product Owner permission approval.
const fs = require('node:fs');
const { verifyCompletedRun } = require('./validate_ci_acceptance_gate.js');

const CI_PATH = '.github/workflows/ci.yml';
const CI_NAME = 'CI';
const EVENTS = Object.freeze(['requested', 'in_progress', 'completed']);
const SHA = /^[0-9a-f]{40}$/;
const REPO = /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/;

function object(value) {
    return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function positive(value) {
    return Number.isSafeInteger(value) && value > 0;
}

function outcome(state, reason) {
    return { state, reason };
}

// A successful observation needs a current, same-repository PR and base.
// Reuse this guard both before and after the slower CI/jobs reads.
function matchesPr(pr, main, run, repository, number) {
    return object(pr) && pr.number === number && pr.state === 'open' &&
        pr.draft === false &&
        pr.head?.repo?.full_name === repository &&
        pr.base?.repo?.full_name === repository &&
        pr.base?.ref === 'main' &&
        pr.head?.sha === run.head_sha &&
        pr.head?.ref === run.head_branch &&
        SHA.test(pr.base?.sha) && main?.object?.sha === pr.base.sha;
}

// SHA alone does not identify a PR. An unrelated PR with the same HEAD
// must not invalidate this PR's latest CI run. Unknown/multi-PR links
// are not evidence and must fail closed.
function checkPrHistory(history, run, number, baseSha) {
    if (!object(history) || !Array.isArray(history.workflow_runs) ||
            !positive(history.total_count) ||
            history.total_count !== history.workflow_runs.length ||
            history.total_count > 100) {
        return outcome('BLOCKED', 'Incomplete CI run history');
    }
    const ciRuns = history.workflow_runs.filter((item) =>
        object(item) && item.name === CI_NAME &&
        item.path === CI_PATH && item.event === 'pull_request' &&
        item.head_sha === run.head_sha);
    if (ciRuns.length === 0 || !ciRuns.every((item) =>
            positive(item.id) && positive(item.run_attempt) &&
            Array.isArray(item.pull_requests) &&
            item.pull_requests.length === 1 &&
            positive(item.pull_requests[0]?.number) &&
            item.pull_requests[0]?.head?.sha === run.head_sha &&
            SHA.test(item.pull_requests[0]?.base?.sha))) {
        return outcome('BLOCKED', 'Missing or malformed CI/PR history');
    }
    const prRuns = ciRuns.filter((item) =>
        item.pull_requests[0].number === number);
    if (prRuns.length === 0) {
        return outcome('BLOCKED', 'CI run has no matching PR history');
    }
    const newest = prRuns.reduce((a, b) =>
        b.id > a.id ? b :
        b.id === a.id && b.run_attempt > a.run_attempt ? b : a);
    if (newest.id !== run.id || newest.run_attempt > run.run_attempt) {
        return outcome('STALE', 'Newer CI run or attempt for this PR');
    }
    if (newest.run_attempt !== run.run_attempt ||
            !prRuns.some((item) => item.id === run.id &&
                item.run_attempt === run.run_attempt &&
                item.pull_requests[0].base.sha === baseSha)) {
        return outcome('BLOCKED', 'CI history identity/base mismatch');
    }
    return outcome('CURRENT', 'Latest CI run for this PR');
}

// The observer always re-reads authoritative GitHub API records. Webhook
// payloads select a run; they never authorize success or write permissions.
async function observe(event, repository, get) {
    if (!REPO.test(repository) || !object(event) ||
            !object(event.repository) ||
            event.repository.full_name !== repository ||
            !EVENTS.includes(event.action) ||
            !object(event.workflow_run)) {
        return outcome('BLOCKED', 'Invalid webhook identity or event');
    }
    const hint = event.workflow_run;
    if (!positive(hint.id) || !positive(hint.run_attempt) ||
            !SHA.test(hint.head_sha)) {
        return outcome('BLOCKED', 'Invalid workflow-run hint');
    }
    const run = await get('actions/runs/' + hint.id);
    if (!object(run) || run.id !== hint.id ||
            run.repository?.full_name !== repository ||
            run.head_repository?.full_name !== repository ||
            run.name !== CI_NAME || run.path !== CI_PATH ||
            run.head_sha !== hint.head_sha) {
        return outcome('BLOCKED', 'Workflow run identity or repository mismatch');
    }
    // Push jobs are intentionally out of scope: their success is not a PR gate.
    if (run.event !== 'pull_request') {
        return outcome('IGNORED', 'Only pull-request CI runs are observed');
    }
    if (run.run_attempt > hint.run_attempt) {
        return outcome('STALE', 'Webhook refers to a superseded run attempt');
    }
    if (!positive(run.run_attempt) || run.run_attempt !== hint.run_attempt) {
        return outcome('BLOCKED', 'Webhook/run attempt mismatch');
    }
    if (!Array.isArray(run.pull_requests) ||
            run.pull_requests.length !== 1 ||
            !positive(run.pull_requests[0]?.number)) {
        return outcome('BLOCKED', 'Missing or ambiguous PR association');
    }
    const number = run.pull_requests[0].number;
    const pr = await get('pulls/' + number);
    const main = await get('git/ref/heads/main');
    if (!matchesPr(pr, main, run, repository, number)) {
        return outcome('BLOCKED', 'Current PR/base/repository identity mismatch');
    }

    // Never let an older successful run/attempt authorize the current HEAD.
    // A partially returned history is not evidence that this run is latest.
    const history = await get('actions/runs?event=pull_request&head_sha=' +
        run.head_sha + '&per_page=100');
    const historyCheck = checkPrHistory(history, run, number, pr.base.sha);
    if (historyCheck.state !== 'CURRENT') {
        return historyCheck;
    }
    if (run.status !== 'completed') {
        return outcome('PENDING', 'Authoritative CI run not completed');
    }
    const jobs = await get('actions/runs/' + run.id +
        '/attempts/' + run.run_attempt + '/jobs?per_page=100');
    const verdict = verifyCompletedRun(run, jobs, {
        id: run.id,
        attempt: run.run_attempt,
        head: pr.head.sha,
        base: pr.base.sha,
        prNumber: number,
    });
    if (!verdict.passed) {
        return outcome('FAILURE', verdict.failures.join('; '));
    }

    // Narrow the time-of-check/time-of-use window before reporting SUCCESS.
    // These GETs are not an atomic snapshot or a future publisher guarantee.
    const finalPr = await get('pulls/' + number);
    const finalMain = await get('git/ref/heads/main');
    if (!matchesPr(finalPr, finalMain, run, repository, number) ||
            finalPr.head.sha !== pr.head.sha ||
            finalPr.base.sha !== pr.base.sha) {
        return outcome('BLOCKED', 'Final PR/main identity changed');
    }
    const finalRun = await get('actions/runs/' + run.id);
    if (!object(finalRun) || !positive(finalRun.run_attempt)) {
        return outcome('BLOCKED', 'Missing final CI run identity');
    }
    if (finalRun.run_attempt > run.run_attempt) {
        return outcome('STALE', 'CI run restarted during verification');
    }
    if (finalRun.run_attempt !== run.run_attempt ||
            finalRun.id !== run.id || finalRun.event !== run.event ||
            finalRun.head_sha !== run.head_sha ||
            finalRun.head_branch !== run.head_branch ||
            finalRun.name !== CI_NAME || finalRun.path !== CI_PATH ||
            finalRun.repository?.full_name !== repository ||
            finalRun.head_repository?.full_name !== repository) {
        return outcome('BLOCKED', 'Final CI run identity changed');
    }
    const finalHistory = await get('actions/runs?event=pull_request&head_sha=' +
        run.head_sha + '&per_page=100');
    const finalCheck = checkPrHistory(finalHistory, finalRun, number,
        finalPr.base.sha);
    if (finalCheck.state !== 'CURRENT') {
        return finalCheck;
    }
    const finalVerdict = verifyCompletedRun(finalRun, jobs, {
        id: run.id,
        attempt: run.run_attempt,
        head: finalPr.head.sha,
        base: finalPr.base.sha,
        prNumber: number,
    });
    if (!finalVerdict.passed) {
        return outcome('FAILURE', finalVerdict.failures.join('; '));
    }
    return outcome('SUCCESS', 'Whole CI run and final PR/jobs identity verified');
}

async function githubGet(path, repository, token) {
    const url = 'https://api.github.com/repos/' + repository + '/' + path;
    const response = await fetch(url, {
        method: 'GET',
        headers: {
            Authorization: 'Bearer ' + token,
            Accept: 'application/vnd.github+json',
            'X-GitHub-Api-Version': '2022-11-28',
        },
    });
    if (!response.ok) {
        throw new Error('GitHub read failed: HTTP ' + response.status);
    }
    return response.json();
}

async function main() {
    const repository = process.env.GITHUB_REPOSITORY;
    const token = process.env.GITHUB_TOKEN;
    const path = process.env.GITHUB_EVENT_PATH;
    if (!REPO.test(repository) || typeof token !== 'string' || !token ||
            typeof path !== 'string' || !path) {
        throw new Error('Missing observer environment');
    }
    const metadata = fs.statSync(path);
    if (metadata.size < 2 || metadata.size > 1024 * 1024) {
        throw new Error('Invalid workflow event size');
    }
    const event = JSON.parse(fs.readFileSync(path, 'utf8'));
    const result = await observe(event, repository, (suffix) =>
        githubGet(suffix, repository, token));
    // Deliberately do not print the event, token, untrusted fields or API JSON.
    console.log('READ-ONLY CI final-run observer: ' + result.state +
        ' — ' + result.reason);
    if (result.state === 'BLOCKED' || result.state === 'FAILURE') {
        process.exitCode = 1;
    }
}

if (require.main === module) {
    main().catch((error) => {
        console.error('[FAIL] Read-only observer: ' + error.message);
        process.exitCode = 1;
    });
}
module.exports = Object.freeze({ observe });
