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
        afterPatchMutate: null, graphqlFailure: null, graphqlNull: false,
        graphqlResponse: null, graphqlCalls: 0, restTagCalls: 0
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
            if (route === 'repos/' + REPO + '/releases/tags/' + TAG) {
                state.restTagCalls++;
                /* Real GitHub REST by-tag endpoint exposes PUBLISHED only. */
                if (state.release.draft) throw new Error('HTTP 404: ' + route);
                return clone(state.release);
            }
            if (route === 'repos/' + REPO + '/releases/' + state.releaseId) {
                return clone(state.release);
            }
            throw new Error('HTTP 404: ' + route);
        },
        async graphql(repository, tag) {
            state.graphqlCalls++;
            if (state.graphqlFailure) throw new Error(state.graphqlFailure);
            if (state.graphqlResponse) return clone(state.graphqlResponse);
            assert.equal(repository, REPO);
            assert.equal(tag, TAG);
            const release = (state.graphqlNull || state.release.tag_name !== tag) ?
                null : {
                    databaseId: state.releaseId,
                    isDraft: state.release.draft,
                    tagName: state.release.tag_name
                };
            return { data: { repository: { release } } };
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

    /* Deliberately return canonical expected files WITHOUT assertions.
     * The production publication guard must detect remote digest/size errors
     * itself. A mock AssertionError must never masquerade as a security PASS. */
    const verifyAssets = () => clone(VERIFIED);
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
        /* The external mutation may itself have changed draft state. The
         * guarantee here is no new publication PATCH by this guard. */
    });
}

async function main() {
    await test('draft REST-by-tag 404; GraphQL plus numeric ID prepare succeeds', async () => {
        const f = make();
        await assert.rejects(f.api.get('repos/' + REPO + '/releases/tags/' + TAG), /HTTP 404/);
        const snapshot = await f.guard.prepare(f.context);
        assert.equal(snapshot.releaseId, 12345);
        assert.equal(f.state.patchCalls, 0);
        assert.equal(f.state.restTagCalls, 1); // Only the test's explicit probe.
        assert.ok(f.state.graphqlCalls >= 1);
    });
    await test('no Release: GraphQL absence preflight PASS without mutation', async () => {
        const f = make();
        f.state.graphqlNull = true;
        assert.equal(await f.guard.absent({ repository: REPO, tag: TAG }), true);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('existing DRAFT blocks release creation', async () => {
        const f = make();
        await assert.rejects(f.guard.absent({ repository: REPO, tag: TAG }),
            /Release already exists \(draft or published\)/);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('existing PUBLISHED release blocks release creation', async () => {
        const f = make();
        f.state.release.draft = false;
        await assert.rejects(f.guard.absent({ repository: REPO, tag: TAG }),
            /Release already exists \(draft or published\)/);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('GraphQL null cannot be used to prepare a DRAFT', async () => {
        const f = make();
        f.state.graphqlNull = true;
        await assert.rejects(f.guard.prepare(f.context),
            /No unambiguous draft Release for tag/);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('GraphQL errors inside HTTP 200 fail closed', async () => {
        const f = make();
        f.state.graphqlResponse = {
            data: { repository: { release: null } },
            errors: [{ message: 'Insufficient release read access' }]
        };
        await assert.rejects(f.guard.absent({ repository: REPO, tag: TAG }),
            /GraphQL release lookup incomplete or failed/);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('GraphQL inaccessible repository is not absence', async () => {
        const f = make();
        f.state.graphqlResponse = { data: { repository: null } };
        await assert.rejects(f.guard.absent({ repository: REPO, tag: TAG }),
            /GraphQL release lookup incomplete or failed/);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('GraphQL invalid databaseId type fails closed', async () => {
        const f = make();
        f.state.graphqlResponse = { data: { repository: { release: {
            databaseId: '12345', isDraft: true, tagName: TAG
        } } } };
        await assert.rejects(f.guard.prepare(f.context),
            /GraphQL release identity malformed or ambiguous/);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('GraphQL wrong databaseId fails before publication', async () => {
        const f = make();
        f.state.graphqlResponse = { data: { repository: { release: {
            databaseId: 99999, isDraft: true, tagName: TAG
        } } } };
        await assert.rejects(f.guard.prepare(f.context), /HTTP 404/);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('GraphQL wrong tag rejected', async () => {
        const f = make();
        f.state.graphqlResponse = { data: { repository: { release: {
            databaseId: 12345, isDraft: true, tagName: 'v2.0.0'
        } } } };
        await assert.rejects(f.guard.prepare(f.context),
            /GraphQL release identity malformed or ambiguous/);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('GraphQL 403/429/500 failures all abort preflight', async () => {
        for (const status of [403, 429, 500]) {
            const f = make();
            f.state.graphqlFailure = 'HTTP ' + status;
            await assert.rejects(f.guard.absent({ repository: REPO, tag: TAG }),
                new RegExp('HTTP ' + status));
            assert.equal(f.state.patchCalls, 0);
        }
    });
    await test('GraphQL draft disappearance before PATCH => zero PATCH', async () => {
        const f = make();
        const snapshot = await f.guard.prepare(f.context);
        f.state.graphqlNull = true;
        await assert.rejects(f.guard.publish(f.context, snapshot),
            /No unambiguous draft Release for tag/);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('GraphQL published tag change after PATCH => UNKNOWN', async () => {
        const f = make();
        const snapshot = await f.guard.prepare(f.context);
        f.state.afterPatchMutate = release => { release.tag_name = 'v2.0.0'; };
        await assert.rejects(f.guard.publish(f.context, snapshot),
            /PUBLICATION_OUTCOME_UNKNOWN/);
        assert.equal(f.state.patchCalls, 1);
        assert.equal(f.state.release.draft, false);
    });
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
        /Draft asset identity\/size\/digest mismatch/);
    await expectNoPatch('changed remote size rejected', f =>
        { f.state.release.assets[0].size++; },
        /Draft asset identity\/size\/digest mismatch/);
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
        /No unambiguous draft Release for tag/);
    await expectNoPatch('GET 404 aborts before publication', f => {
        f.state.getFailure = { route: '/releases/12345', message: 'HTTP 404' };
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
            /Draft Release ID lookup inconsistent/);
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
            /Cannot determine stable GitHub CLI security patch version/);
    });
    await test('patched github CLI 2.102.0 and 2.103.3 accepted', () => {
        requirePatchedGh('gh version 2.102.0 (2026-09-30)');
        requirePatchedGh('gh version 2.102.1 (2026-10-02)');
        requirePatchedGh('gh version 2.103.3 (2026-11-01)');
    });
    await test('GH CLI pre-release and suffixed versions cannot claim stable patch', () => {
        assert.throws(() => requirePatchedGh('gh version 2.102.0-rc.1 (2026-09-28)'),
            /Cannot determine stable GitHub CLI/);
        assert.throws(() => requirePatchedGh('gh version 2.103.0-dev'),
            /Cannot determine stable GitHub CLI/);
        assert.throws(() => requirePatchedGh('gh version 2.101.99 (2026-11-01)'),
            /GitHub CLI 2.102.0\+ required/);
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
