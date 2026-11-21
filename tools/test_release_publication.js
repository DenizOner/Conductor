'use strict';

/**
 * @file test_release_publication.js
 * @brief Read-only mock GitHub API tests for P20-S2 publication and provenance.
 * NO network, credentials, GitHub Release, tag, or real file mutation occurs.
 */
const assert = require('node:assert/strict');
const {
    createGuard, requirePatchedGh, attestationArgs, verifyAttestations
} = require('./release_publication_guard.js');

const REPO = 'DenizOner/Conductor';
const TAG = 'v1.4.0';
const COMMIT = 'a'.repeat(40);
const OTHER = 'b'.repeat(40);
const REF = 'refs/tags/' + TAG;
const NAMES = ['firmware.bin', '1000-0906-010400aa-LogitechZ906.ota',
    'index.json', 'conductor_converter.mjs'];
const VERIFIED = NAMES.map((name, index) => ({
    name, size: 100 + index, sha256: String(index + 1).repeat(64)
}));
let passed = 0;

function clone(value) {
    return JSON.parse(JSON.stringify(value));
}
function make() {
    const context = { directory: 'synthetic-only', repository: REPO, tag: TAG, commit: COMMIT };
    const state = {
        tagCommit: COMMIT, annotated: false, draft: true,
        releaseId: 12345, patchCalls: 0, patchPath: null, patchPayload: null,
        getFailure: null, patchFailure: null, afterPublishReadFailure: null,
        afterPatchMutate: null
    };
    state.release = {
        id: 12345, tag_name: TAG, draft: true,
        assets: VERIFIED.map((file, i) => ({
            id: 200 + i, name: file.name, size: file.size,
            state: 'uploaded', digest: 'sha256:' + file.sha256
        }))
    };

    const api = {
        async get(route) {
            if (state.getFailure && route.includes(state.getFailure.route)) {
                throw new Error(state.getFailure.message);
            }
            if (state.afterPublishReadFailure && !state.release.draft &&
                route.includes(state.afterPublishReadFailure.route)) {
                throw new Error(state.afterPublishReadFailure.message);
            }
            if (route === 'repos/' + REPO + '/git/ref/tags/' + TAG) {
                return {
                    ref: REF, object: state.annotated ?
                        { type: 'tag', sha: 'c'.repeat(40) } :
                        { type: 'commit', sha: state.tagCommit }
                };
            }
            if (route === 'repos/' + REPO + '/git/tags/' + 'c'.repeat(40)) {
                return { object: { type: 'commit', sha: state.tagCommit } };
            }
            if (route === 'repos/' + REPO + '/releases/tags/' + TAG ||
                route === 'repos/' + REPO + '/releases/' + state.releaseId) {
                return clone(state.release);
            }
            throw new Error('HTTP 404: ' + route);
        },
        async patch(route, body) {
            state.patchCalls++;
            state.patchPath = route;
            state.patchPayload = body;
            if (route !== 'repos/' + REPO + '/releases/12345' ||
                !body || body.draft !== false) {
                throw new Error('Unexpected PATCH target or content');
            }
            if (state.patchFailure === 'before') throw new Error('HTTP 429');
            state.release.draft = false;
            if (typeof state.afterPatchMutate === 'function') {
                state.afterPatchMutate(state.release);
            }
            if (state.patchFailure === 'after') throw new Error('Remote disconnected');
            return clone(state.release);
        }
    };

    /* Real production calls verifyReleaseAssets; this mock exercises the
     * orchestrator's independent asset-ID/source-SHA/state invariants. */
    const verifyAssets = (dir, repo, tag, release) => {
        assert.equal(dir, context.directory);
        assert.equal(repo, REPO);
        assert.equal(tag, TAG);
        assert.equal(release.draft, true);
        assert.equal(release.assets.length, VERIFIED.length);
        for (const file of VERIFIED) {
            const asset = release.assets.find(x => x.name === file.name);
            assert.ok(asset, 'Missing ' + file.name);
            assert.equal(asset.size, file.size);
            assert.equal(asset.digest, 'sha256:' + file.sha256);
            assert.equal(asset.state, 'uploaded');
        }
        return clone(VERIFIED);
    };
    return { context, state, api, guard: createGuard(api, verifyAssets) };
}
async function test(name, run) {
    await run();
    passed++;
    console.log('[PASS] ' + name);
}
async function expectNoPatch(name, mutate, pattern, beforePrepare = false) {
    await test(name, async () => {
        const f = make();
        if (beforePrepare) {
            mutate(f);
            await assert.rejects(f.guard.prepare(f.context), pattern);
        } else {
            const snapshot = await f.guard.prepare(f.context);
            mutate(f);
            await assert.rejects(f.guard.publish(f.context, snapshot), pattern);
        }
        assert.equal(f.state.patchCalls, 0, 'Never PATCH on prepublication failure');
        assert.equal(f.state.release.draft, true);
    });
}

async function main() {
    await test('lightweight tag and exact-ID publication PASS', async () => {
        const f = make();
        const snapshot = await f.guard.prepare(f.context);
        assert.equal(snapshot.releaseId, 12345);
        assert.equal(snapshot.assets.length, 4);
        const result = await f.guard.publish(f.context, snapshot);
        assert.deepEqual(result, { releaseId: 12345, tag: TAG, published: true });
        assert.equal(f.state.patchCalls, 1);
        assert.equal(f.state.patchPath, 'repos/' + REPO + '/releases/12345');
        assert.deepEqual(f.state.patchPayload, { draft: false });
    });
    await test('annotated tag resolves to the exact pinned commit PASS', async () => {
        const f = make();
        f.state.annotated = true;
        const snap = await f.guard.prepare(f.context);
        await f.guard.publish(f.context, snap);
        assert.equal(f.state.patchCalls, 1);
    });
    await expectNoPatch('incorrect original tag SHA rejected', f =>
        { f.state.tagCommit = OTHER; },
        /Release tag does not resolve to the pinned build commit/, true);
    await expectNoPatch('tag moved after draft readback rejected', f =>
        { f.state.tagCommit = OTHER; },
        /Release tag does not resolve to the pinned build commit/);
    await expectNoPatch('changed Release ID rejected', f => {
        f.state.releaseId = 99999;
        f.state.release.id = 99999;
    }, /Draft Release ID replaced since prepared snapshot/);
    await expectNoPatch('changed remote asset ID rejected', f =>
        { f.state.release.assets[0].id = 9191; },
        /Draft Release or asset identity changed since prepared snapshot/);
    await expectNoPatch('changed remote SHA-256 rejected', f =>
        { f.state.release.assets[0].digest = 'sha256:' + 'f'.repeat(64); },
        /Expected values to be strictly equal|Draft asset identity\/size\/digest mismatch/);
    await expectNoPatch('changed remote size rejected', f =>
        { f.state.release.assets[0].size++; },
        /Expected values to be strictly equal|Draft asset identity\/size\/digest mismatch/);
    await expectNoPatch('missing asset ID rejected before publication', f =>
        { delete f.state.release.assets[0].id; },
        /Missing remote asset ID/, true);
    await expectNoPatch('duplicate asset IDs rejected before publication', f =>
        { f.state.release.assets[0].id = f.state.release.assets[1].id; },
        /Duplicate remote asset ID/, true);
    await expectNoPatch('draft unexpectedly made public rejected', f =>
        { f.state.release.draft = false; },
        /No unambiguous draft Release for tag/);
    await expectNoPatch('release tag changed rejected', f =>
        { f.state.release.tag_name = 'v2.0.0'; },
        /Draft Release ID\/tag\/state mismatch/);
    await expectNoPatch('GET 404 aborts before publication', f => {
        f.state.getFailure = { route: '/releases/tags/', message: 'HTTP 404' };
    }, /HTTP 404/);
    await expectNoPatch('GET 429 aborts before publication', f => {
        f.state.getFailure = { route: '/releases/12345', message: 'HTTP 429' };
    }, /HTTP 429/);
    await test('wrong pinned source snapshot rejected before PATCH', async () => {
        const f = make();
        const snapshot = await f.guard.prepare(f.context);
        snapshot.commit = OTHER;
        await assert.rejects(f.guard.publish(f.context, snapshot),
            /Missing or wrong release snapshot/);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('missing original Release ID rejected', async () => {
        const f = make();
        delete f.state.release.id;
        await assert.rejects(f.guard.prepare(f.context),
            /No unambiguous draft Release for tag/);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('PATCH error before publication reports UNKNOWN and does not retry', async () => {
        const f = make();
        const snapshot = await f.guard.prepare(f.context);
        f.state.patchFailure = 'before';
        await assert.rejects(f.guard.publish(f.context, snapshot),
            /PUBLICATION_OUTCOME_UNKNOWN/);
        assert.equal(f.state.patchCalls, 1);
        assert.equal(f.state.release.draft, true);
    });
    await test('PATCH response lost after publication reports UNKNOWN (no retry)', async () => {
        const f = make();
        const snapshot = await f.guard.prepare(f.context);
        f.state.patchFailure = 'after';
        await assert.rejects(f.guard.publish(f.context, snapshot),
            /PUBLICATION_OUTCOME_UNKNOWN/);
        assert.equal(f.state.patchCalls, 1);
        assert.equal(f.state.release.draft, false);
    });
    await test('post-publication GET 500 reports UNKNOWN without retry', async () => {
        const f = make();
        const snapshot = await f.guard.prepare(f.context);
        f.state.afterPublishReadFailure = {
            route: '/releases/12345', message: 'HTTP 500'
        };
        await assert.rejects(f.guard.publish(f.context, snapshot),
            /PUBLICATION_OUTCOME_UNKNOWN/);
        assert.equal(f.state.patchCalls, 1);
        assert.equal(f.state.release.draft, false);
    });
    await test('privileged writer mutation during PATCH detected AFTER publication', async () => {
        const f = make();
        const snapshot = await f.guard.prepare(f.context);
        f.state.afterPatchMutate = release => {
            release.assets[0].digest = 'sha256:' + '0'.repeat(64);
        };
        await assert.rejects(f.guard.publish(f.context, snapshot),
            /PUBLICATION_OUTCOME_UNKNOWN/);
        assert.equal(f.state.patchCalls, 1);
        assert.equal(f.state.release.draft, false);
    });
    await test('unsupported github CLI 2.101.0 rejected for source/signer policy', () => {
        assert.throws(() => requirePatchedGh('gh version 2.101.0 (2026-09-01)'),
            /GitHub CLI 2.102.0\+ required/);
        assert.throws(() => requirePatchedGh('unrecognizable'),
            /Cannot determine GitHub CLI/);
    });
    await test('patched github CLI 2.102.0 and 2.103.3 accepted', () => {
        requirePatchedGh('gh version 2.102.0 (2026-09-30)');
        requirePatchedGh('gh version 2.103.3 (2026-11-01)');
    });
    await test('exact-source signer identity args on four files', () => {
        const args = attestationArgs('dist/release', REPO, REF, COMMIT, 'release_ota.yml');
        assert.equal(args.length, 4);
        for (const cmd of args) {
            assert.deepEqual(cmd.slice(0, 2), ['attestation', 'verify']);
            assert.ok(NAMES.some(n => cmd[2].endsWith(n)));
            assert.ok(cmd.includes('--repo') && cmd.includes(REPO));
            assert.ok(cmd.includes('--source-digest') && cmd.includes(COMMIT));
            assert.ok(cmd.includes('--source-ref') && cmd.includes(REF));
            assert.ok(cmd.includes('--signer-workflow') &&
                cmd.includes(REPO + '/.github/workflows/release_ota.yml'));
        }
    });
    await test('wrong source ref/commit or signer workflow blocked before execution', () => {
        assert.throws(() => attestationArgs('dist/release', REPO,
            'refs/heads/evil', COMMIT, 'release_ota.yml'), /Invalid attestation source ref/);
        assert.throws(() => attestationArgs('dist/release', REPO,
            REF, OTHER.slice(1), 'release_ota.yml'), /Invalid attestation source commit/);
        assert.throws(() => attestationArgs('dist/release', REPO,
            REF, COMMIT, 'attacker.yml'), /Unapproved signer workflow/);
    });
    await test('attestation check runs four commands with exact policies', () => {
        const invocations = [];
        const runner = args => {
            invocations.push(args);
            return args[0] === '--version' ? 'gh version 2.103.3 (2026-11-01)' : '';
        };
        assert.equal(verifyAttestations('dist/release', REPO, REF,
            COMMIT, 'release_ota.yml', runner), 4);
        assert.equal(invocations.length, 5);
        assert.deepEqual(invocations[1].slice(-2),
            ['--signer-workflow', REPO + '/.github/workflows/release_ota.yml']);
    });
    await test('old GitHub CLI runs zero attestation checks', () => {
        const invocations = [];
        const runner = args => {
            invocations.push(args);
            return 'gh version 2.101.0 (2026-09-01)';
        };
        assert.throws(() => verifyAttestations('dist/release', REPO, REF,
            COMMIT, 'release_ota.yml', runner), /GitHub CLI 2.102.0\+ required/);
        assert.equal(invocations.length, 1);
    });
    await test('GitHub CLI verification errors fail closed', () => {
        let invocation = 0;
        const runner = args => {
            invocation++;
            if (args[0] === '--version') return 'gh version 2.103.3 (2026-11-01)';
            throw new Error('signature does not match source SHA');
        };
        assert.throws(() => verifyAttestations('dist/release', REPO, REF,
            COMMIT, 'release_ota.yml', runner), /signature does not match source SHA/);
        assert.equal(invocation, 2);
    });
    console.log('PASS: publication guard mock tests (' + passed + ' cases)');
}

main().catch(err => {
    console.error('[FAIL] publication mock tests: ' + err.stack);
    process.exitCode = 1;
});
