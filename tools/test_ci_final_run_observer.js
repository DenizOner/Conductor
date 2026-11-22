#!/usr/bin/env node
'use strict';

const { observe } = require('./ci_final_run_observer.js');
const REPO = 'DenizOner/Conductor';
const HEAD = 'a'.repeat(40);
const BASE = 'b'.repeat(40);

function fixture() {
    const run = {
        id: 700, run_attempt: 2, head_sha: HEAD, head_branch: 'fix/example',
        status: 'completed', conclusion: 'success', event: 'pull_request',
        name: 'CI', path: '.github/workflows/ci.yml',
        repository: { full_name: REPO }, head_repository: { full_name: REPO },
        pull_requests: [{ number: 21, head: { sha: HEAD },
            base: { sha: BASE } }],
    };
    return {
        event: { repository: { full_name: REPO }, action: 'completed',
            workflow_run: { id: 700, run_attempt: 2, head_sha: HEAD } },
        run,
        pr: { number: 21, state: 'open', draft: false,
            head: { sha: HEAD, ref: 'fix/example',
                repo: { full_name: REPO } },
            base: { sha: BASE, ref: 'main',
                repo: { full_name: REPO } } },
        main: { object: { sha: BASE } },
        history: { total_count: 1, workflow_runs: [{
            id: 700, run_attempt: 2, head_sha: HEAD, event: 'pull_request',
            name: 'CI', path: '.github/workflows/ci.yml',
        }] },
        jobs: { total_count: 4, jobs: [
            'Host Automated Validators',
            'Build Firmware Binary & Validate OTA',
            'Dependency Review', 'Conductor Acceptance Gate',
        ].map((name, i) => ({
            id: 800 + i, name, run_id: 700, run_attempt: 2,
            head_sha: HEAD, workflow_name: 'CI', status: 'completed',
            conclusion: 'success',
        })) },
    };
}

function getter(d, broken) {
    return async (path) => {
        if (path === broken) throw new Error('Simulated GitHub API failure');
        if (path === 'actions/runs/700') return d.run;
        if (path === 'pulls/21') return d.pr;
        if (path === 'git/ref/heads/main') return d.main;
        if (path === 'actions/runs?event=pull_request&head_sha=' +
                HEAD + '&per_page=100') return d.history;
        if (path === 'actions/runs/700/attempts/2/jobs?per_page=100') {
            return d.jobs;
        }
        throw new Error('Unexpected API path: ' + path);
    };
}

async function runTests() {
    let count = 0;
    const check = async (name, mutate, expected) => {
        const d = fixture();
        mutate(d);
        const result = await observe(d.event, REPO, getter(d));
        if (result.state !== expected) {
            throw new Error(name + ' expected ' + expected + ': ' +
                JSON.stringify(result));
        }
        count += 1;
    };
    await check('valid', () => {}, 'SUCCESS');
    for (const outcome of ['cancelled', 'failure', 'timed_out',
        'skipped', 'neutral', null]) {
        await check('run outcome ' + outcome,
            (d) => { d.run.conclusion = outcome; }, 'FAILURE');
    }
    for (const status of ['queued', 'in_progress', 'pending', 'waiting']) {
        await check('run status ' + status,
            (d) => { d.run.status = status; }, 'PENDING');
    }
    for (const action of ['requested', 'in_progress']) {
        await check('delayed event ' + action,
            (d) => { d.event.action = action; }, 'SUCCESS');
    }
    await check('webhook old attempt', (d) => {
        d.event.workflow_run.run_attempt = 1;
    }, 'STALE');
    await check('webhook newer attempt', (d) => {
        d.event.workflow_run.run_attempt = 3;
    }, 'BLOCKED');
    await check('new CI run', (d) => {
        d.history.total_count = 2;
        d.history.workflow_runs.push({
            id: 701, run_attempt: 1, head_sha: HEAD,
            name: 'CI', path: '.github/workflows/ci.yml',
            event: 'pull_request',
        });
    }, 'STALE');
    await check('new CI attempt', (d) => {
        d.history.workflow_runs[0].run_attempt = 3;
    }, 'STALE');
    await check('empty history', (d) => {
        d.history.total_count = 0;
        d.history.workflow_runs = [];
    }, 'BLOCKED');
    await check('incomplete history', (d) => {
        d.history.total_count = 101;
    }, 'BLOCKED');
    const wrong = [
        ['event repo', (d) => { d.event.repository.full_name = 'other/repo'; }],
        ['event SHA', (d) => { d.event.workflow_run.head_sha = 'c'.repeat(40); }],
        ['event missing', (d) => { d.event.workflow_run = null; }],
        ['unknown webhook action', (d) => { d.event.action = 'deleted'; }],
        ['run SHA', (d) => { d.run.head_sha = 'c'.repeat(40); }],
        ['run repository', (d) => { d.run.repository.full_name = 'other/repo'; }],
        ['fork runner', (d) => { d.run.head_repository.full_name = 'other/repo'; }],
        ['workflow name', (d) => { d.run.name = 'Other'; }],
        ['workflow path', (d) => { d.run.path = 'other.yml'; }],
        ['missing PR association', (d) => { d.run.pull_requests = []; }],
        ['multiple PR associations', (d) => {
            d.run.pull_requests.push(d.run.pull_requests[0]);
        }],
        ['PR number', (d) => { d.pr.number = 999; }],
        ['PR closed', (d) => { d.pr.state = 'closed'; }],
        ['PR draft', (d) => { d.pr.draft = true; }],
        ['PR HEAD', (d) => { d.pr.head.sha = 'c'.repeat(40); }],
        ['PR base', (d) => { d.pr.base.sha = 'c'.repeat(40); }],
        ['PR fork', (d) => { d.pr.head.repo.full_name = 'other/repo'; }],
        ['PR target', (d) => { d.pr.base.ref = 'release'; }],
        ['PR branch', (d) => { d.pr.head.ref = 'other'; }],
        ['main changed', (d) => { d.main.object.sha = 'c'.repeat(40); }],
        ['wrong history SHA', (d) => {
            d.history.workflow_runs[0].head_sha = 'c'.repeat(40);
        }],
    ];
    for (const [name, mutate] of wrong) {
        await check(name, mutate, 'BLOCKED');
    }
    const failed = [
        ['missing job ID', (d) => { delete d.jobs.jobs[0].id; }],
        ['duplicate job ID', (d) => {
            d.jobs.jobs[1].id = d.jobs.jobs[0].id;
        }],
        ['incomplete jobs', (d) => { d.jobs.jobs.pop(); }],
        ['wrong job attempt', (d) => { d.jobs.jobs[0].run_attempt = 1; }],
        ['wrong job SHA', (d) => {
            d.jobs.jobs[0].head_sha = 'c'.repeat(40);
        }],
        ['skipped host', (d) => { d.jobs.jobs[0].conclusion = 'skipped'; }],
        ['skipped Dependency Review on PR', (d) => {
            d.jobs.jobs[2].conclusion = 'skipped';
        }],
    ];
    for (const [name, mutate] of failed) {
        await check(name, mutate, 'FAILURE');
    }
    await check('push ignored', (d) => { d.run.event = 'push'; }, 'IGNORED');
    const endpoints = [
        'actions/runs/700', 'pulls/21', 'git/ref/heads/main',
        'actions/runs?event=pull_request&head_sha=' +
            HEAD + '&per_page=100',
        'actions/runs/700/attempts/2/jobs?per_page=100',
    ];
    for (const endpoint of endpoints) {
        let rejected = false;
        try {
            const d = fixture();
            await observe(d.event, REPO, getter(d, endpoint));
        } catch (error) {
            rejected = error.message === 'Simulated GitHub API failure';
        }
        if (!rejected) throw new Error('API failure accepted: ' + endpoint);
        count += 1;
    }
    console.log('PASS: CI final-run read-only observer (' + count + ' cases)');
}

if (require.main === module) {
    runTests().catch((error) => {
        console.error('[FAIL] CI final-run observer: ' + error.message);
        process.exitCode = 1;
    });
}
module.exports = Object.freeze({ runTests });
