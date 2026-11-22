'use strict';

/**
 * @file test_release_publication.js
 * @brief Read-only mock GitHub API tests for P20-S2 publication and provenance.
 * NO network, credentials, GitHub Release, tag, or real file mutation occurs.
 */
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const path = require('node:path');
const {
    createGuard, requirePatchedGh, attestationArgs, verifyAttestations,
    uploadEndpoint, persistSnapshot
} = require('./release_publication_guard.js');

const REPO = 'DenizOner/Conductor';
const TAG = 'v1.4.0';
const COMMIT = 'a'.repeat(40);
const OTHER = 'b'.repeat(40);
const REF = 'refs/tags/' + TAG;
const NAMES = ['firmware.bin', '1000-0906-010400aa-LogitechZ906.ota',
    'index.json', 'conductor_converter.mjs'];
const FIXTURES = NAMES.map((name, index) => Buffer.alloc(100 + index, index + 1));
const VERIFIED = NAMES.map((name, index) => ({
    name, size: FIXTURES[index].length,
    sha256: crypto.createHash('sha256').update(FIXTURES[index]).digest('hex')
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
        graphqlResponse: null, graphqlCalls: 0, restTagCalls: 0,
        createCalls: 0, uploadCalls: 0, createFailure: null,
        uploadFailure: null, afterCreateMutation: null, afterUploadMutation: null,
        createResponseOverride: null, uploadResponseOverride: null,
        foreignRelease: null, graphqlPickForeign: false, graphqlLagCount: 0,
        finalTagFailure: null, tagReadsAfterFour: 0
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
            if (route === 'repos/' + REPO + '/git/ref/tags/' + TAG &&
                state.uploadCalls === NAMES.length) {
                state.tagReadsAfterFour++;
                /* Two guarded reads after upload four precede final tag readback. */
                if (state.finalTagFailure && state.tagReadsAfterFour === 3) {
                    throw new Error(state.finalTagFailure);
                }
            }
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
            if (state.graphqlLagCount > 0) {
                state.graphqlLagCount--;
                return { data: { repository: { release: null } } };
            }
            const selected = state.graphqlPickForeign ? state.foreignRelease : state.release;
            const release = (state.graphqlNull || !selected || selected.tag_name !== tag) ?
                null : {
                    databaseId: state.graphqlPickForeign ? selected.id : state.releaseId,
                    isDraft: selected.draft,
                    tagName: selected.tag_name
                };
            return { data: { repository: { release } } };
        },
        async create(route, body) {
            state.createCalls++;
            assert.equal(route, 'repos/' + REPO + '/releases');
            assert.equal(body.tag_name, TAG);
            assert.equal(body.target_commitish, COMMIT);
            assert.equal(body.draft, true);
            assert.equal(body.prerelease, false);
            const collision = state.createFailure &&
                /^(collision-(draft|public)|collision-409-(draft|public))$/.test(state.createFailure);
            if (collision) {
                state.graphqlNull = false;
                state.release.draft = state.createFailure.endsWith('draft');
                const status = state.createFailure.includes('409') ? 409 : 422;
                throw new Error('HTTP ' + status + ': Release already exists');
            }
            if (state.createFailure === 'before') throw new Error('HTTP 429');
            state.graphqlNull = false;
            state.release = { id: state.releaseId, tag_name: TAG, draft: true, assets: [] };
            if (state.createFailure === 'duplicate-201-foreign' ||
                state.createFailure === 'duplicate-201-own') {
                /* Simulated server alternative: concurrent draft X and new Y
                 * can exist; which one GraphQL selects is NOT guaranteed. */
                state.foreignRelease = {
                    id: 55555, tag_name: TAG, draft: true,
                    assets: [{ id: 67890, name: 'foreign.bin', size: 16,
                        state: 'uploaded', digest: 'sha256:' + '1'.repeat(64) }]
                };
                state.graphqlPickForeign = state.createFailure === 'duplicate-201-foreign';
            }
            if (state.createFailure === 'delayed-graphql') state.graphqlLagCount = 1;
            const createdResponse = clone(state.release);
            if (state.afterCreateMutation) state.afterCreateMutation(state);
            if (state.createFailure === 'after') throw new Error('Remote connection lost');
            return state.createResponseOverride || createdResponse;
        },
        async upload(endpoint, filename) {
            state.uploadCalls++;
            assert.ok(endpoint.startsWith('https://uploads.github.com/repos/' + REPO +
                '/releases/' + state.releaseId + '/assets?name='));
            const name = decodeURIComponent(endpoint.split('?name=')[1]);
            assert.equal(path.basename(filename), name);
            if (state.uploadFailure === 'before') throw new Error('HTTP 409');
            const file = VERIFIED.find(f => f.name === name);
            assert.ok(file, 'No unexpected asset names');
            const asset = {
                id: 200 + NAMES.indexOf(name), name, size: file.size,
                digest: 'sha256:' + file.sha256, state: 'uploaded'
            };
            state.release.assets.push(asset);
            const uploadResponse = clone(asset);
            if (state.afterUploadMutation) state.afterUploadMutation(state);
            if (state.uploadFailure === 'after') throw new Error('Upload response lost');
            return state.uploadResponseOverride || uploadResponse;
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
    const readAsset = filename => {
        const index = NAMES.indexOf(path.basename(filename));
        assert.notEqual(index, -1, 'Unexpected synthetic fixture');
        return FIXTURES[index];
    };
    return { context, state, api, guard: createGuard(api, verifyAssets, readAsset) };
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
    await test('P20-S2c: concurrent existing DRAFT + HTTP 409 => UNKNOWN and zero uploads', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.createFailure = 'collision-409-draft';
        await assert.rejects(f.guard.stage(f.context),
            /CREATE_OUTCOME_UNKNOWN:.*HTTP 409: Release already exists/);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 0);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('P20-S2c: concurrent existing PUBLIC + HTTP 409 => UNKNOWN and zero uploads', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.createFailure = 'collision-409-public';
        await assert.rejects(f.guard.stage(f.context),
            /CREATE_OUTCOME_UNKNOWN:.*HTTP 409: Release already exists/);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 0);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('P20-S2c: duplicate HTTP 201, GraphQL selects foreign draft X; reject Y', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.createFailure = 'duplicate-201-foreign';
        await assert.rejects(f.guard.stage(f.context),
            /CREATE_OUTCOME_UNKNOWN:.*Created draft GraphQL ID\/state mismatch/);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 0);
        assert.equal(f.state.patchCalls, 0);
        assert.equal(f.state.foreignRelease.id, 55555);
        assert.deepEqual(f.state.foreignRelease.assets.map(a => a.name), ['foreign.bin']);
        assert.equal(f.state.release.id, 12345);
        assert.deepEqual(f.state.release.assets, []);
    });
    await test('P20-S2c: duplicate HTTP 201, GraphQL selects own Y; X remains untouched', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.createFailure = 'duplicate-201-own';
        const snapshot = await f.guard.stage(f.context);
        assert.equal(snapshot.releaseId, 12345);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 4);
        assert.equal(f.state.patchCalls, 0);
        assert.equal(f.state.foreignRelease.id, 55555);
        assert.deepEqual(f.state.foreignRelease.assets.map(a => a.name), ['foreign.bin']);
        /* This hypothetical server behavior is NOT a uniqueness proof:
         * GraphQL selected Y despite an existing same-tag draft X. */
    });
    await test('P20-S2c: HTTP 201 but delayed GraphQL discovery => CREATE UNKNOWN', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.createFailure = 'delayed-graphql';
        await assert.rejects(f.guard.stage(f.context),
            /CREATE_OUTCOME_UNKNOWN:.*Created draft GraphQL ID\/state mismatch/);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 0);
        assert.equal(f.state.patchCalls, 0);
        assert.equal(f.state.release.id, 12345);
    });
    await test('P20-S2c: POST 201 then numeric REST GET 500 => CREATE UNKNOWN', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.afterCreateMutation = state => {
            state.getFailure = { route: '/releases/12345', message: 'HTTP 500' };
        };
        await assert.rejects(f.guard.stage(f.context),
            /CREATE_OUTCOME_UNKNOWN: post-create draft\/asset readback failed;.*HTTP 500/);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 0);
        assert.equal(f.state.patchCalls, 0);
        assert.equal(f.state.release.draft, true);
    });
    await test('P20-S2c: POST 201 then GraphQL HTTP 429 => CREATE UNKNOWN', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.afterCreateMutation = state => { state.graphqlFailure = 'HTTP 429'; };
        await assert.rejects(f.guard.stage(f.context),
            /CREATE_OUTCOME_UNKNOWN: post-create draft\/asset readback failed;.*HTTP 429/);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 0);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('P20-S2c: asset uploaded then numeric REST GET 500 => ASSET UNKNOWN', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.afterUploadMutation = state => {
            state.getFailure = { route: '/releases/12345', message: 'HTTP 500' };
        };
        await assert.rejects(f.guard.stage(f.context),
            /ASSET_UPLOAD_OUTCOME_UNKNOWN: post-create draft\/asset readback failed;.*HTTP 500/);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 1);
        assert.equal(f.state.release.assets.length, 1);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('P20-S2c: second uploaded asset then GraphQL HTTP 429 => ASSET UNKNOWN', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.afterUploadMutation = state => {
            if (state.uploadCalls === 2) state.graphqlFailure = 'HTTP 429';
        };
        await assert.rejects(f.guard.stage(f.context),
            /ASSET_UPLOAD_OUTCOME_UNKNOWN: post-create draft\/asset readback failed;.*HTTP 429/);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 2);
        assert.equal(f.state.release.assets.length, 2);
        assert.equal(f.state.patchCalls, 0);
    });
    for (const status of [429, 500]) {
        await test('P20-S2d: final tag HTTP ' + status +
            ' after four uploads => ASSET UNKNOWN, zero PATCH', async () => {
            const f = make();
            f.state.graphqlNull = true;
            f.state.finalTagFailure = 'HTTP ' + status;
            await assert.rejects(f.guard.stage(f.context),
                new RegExp('ASSET_UPLOAD_OUTCOME_UNKNOWN: final tag readback failed;.*HTTP ' +
                    status));
            assert.equal(f.state.tagReadsAfterFour, 3);
            assert.equal(f.state.createCalls, 1);
            assert.equal(f.state.uploadCalls, 4);
            assert.equal(f.state.release.assets.length, 4);
            assert.equal(f.state.patchCalls, 0);
        });
    }
    await test('P20-S2d: snapshot write failure after four uploads => ASSET UNKNOWN', async () => {
        const f = make();
        f.state.graphqlNull = true;
        const snapshot = await f.guard.stage(f.context);
        let writeCalls = 0;
        assert.throws(() => persistSnapshot('snapshot.json', snapshot, 'stage',
            (file, contents, options) => {
                writeCalls++;
                assert.equal(file, 'snapshot.json');
                assert.equal(JSON.parse(contents).releaseId, snapshot.releaseId);
                assert.equal(options.flag, 'wx');
                throw new Error('ENOSPC');
            }), /ASSET_UPLOAD_OUTCOME_UNKNOWN: snapshot write failed;.*ENOSPC/);
        assert.equal(writeCalls, 1);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 4);
        assert.equal(f.state.release.assets.length, 4);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('P20-S2d: prepare snapshot failure retains original error', () => {
        assert.throws(() => persistSnapshot('snapshot.json', { releaseId: 12345 },
            'prepare', () => { throw new Error('EACCES'); }), /EACCES/);
    });
    await test('P20-S2b: create-only stage uploads exactly four files to NEW numeric ID', async () => {
        const f = make();
        f.state.graphqlNull = true;
        const snapshot = await f.guard.stage(f.context);
        assert.equal(snapshot.releaseId, 12345);
        assert.equal(snapshot.assets.length, 4);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 4);
        assert.equal(f.state.patchCalls, 0);
        assert.equal(f.state.restTagCalls, 0);
        assert.equal(f.state.release.draft, true);
        assert.deepEqual(snapshot.assets.map(a => a.name).sort(), [...NAMES].sort());
    });
    await test('P20-S2b: stage snapshot supports guarded numeric-ID publication', async () => {
        const f = make();
        f.state.graphqlNull = true;
        const snapshot = await f.guard.stage(f.context);
        const result = await f.guard.publish(f.context, snapshot);
        assert.equal(result.published, true);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 4);
        assert.equal(f.state.patchCalls, 1);
        assert.equal(f.state.patchPath, 'repos/' + REPO + '/releases/12345');
    });
    await test('P20-S2b: uploadEndpoint requires canonical name and numeric ID', async () => {
        assert.equal(uploadEndpoint({ repository: REPO }, 12345, NAMES[0]),
            'https://uploads.github.com/repos/' + REPO +
            '/releases/12345/assets?name=firmware.bin');
        assert.throws(() => uploadEndpoint({ repository: REPO }, -2, NAMES[0]),
            /Invalid new release asset target/);
        assert.throws(() => uploadEndpoint({ repository: REPO }, 12345, 'evil.bin'),
            /Invalid new release asset target/);
    });
    await test('P20-S2b: existing DRAFT => zero create/upload/PATCH', async () => {
        const f = make();
        await assert.rejects(f.guard.stage(f.context),
            /Release already exists \(draft or published\); refusing create-only POST/);
        assert.equal(f.state.createCalls, 0);
        assert.equal(f.state.uploadCalls, 0);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('P20-S2b: existing published Release => zero create/upload/PATCH', async () => {
        const f = make();
        f.state.release.draft = false;
        await assert.rejects(f.guard.stage(f.context),
            /Release already exists \(draft or published\); refusing create-only POST/);
        assert.equal(f.state.createCalls, 0);
        assert.equal(f.state.uploadCalls, 0);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('P20-S2b: race to create other DRAFT => POST 422; zero upload', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.createFailure = 'collision-draft';
        await assert.rejects(f.guard.stage(f.context), /CREATE_OUTCOME_UNKNOWN:.*HTTP 422/);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 0);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('P20-S2b: race to create other PUBLIC Release => POST 422; zero upload', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.createFailure = 'collision-public';
        await assert.rejects(f.guard.stage(f.context), /CREATE_OUTCOME_UNKNOWN:.*HTTP 422/);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 0);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('P20-S2b: POST HTTP429 before create never retried', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.createFailure = 'before';
        await assert.rejects(f.guard.stage(f.context), /CREATE_OUTCOME_UNKNOWN:.*HTTP 429/);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 0);
    });
    await test('P20-S2b: lost POST response after new draft leaves it untouched', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.createFailure = 'after';
        await assert.rejects(f.guard.stage(f.context),
            /CREATE_OUTCOME_UNKNOWN:.*Remote connection lost/);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 0);
        assert.equal(f.state.release.draft, true);
        assert.deepEqual(f.state.release.assets, []);
    });
    await test('P20-S2b: malformed create response forbids asset upload', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.createResponseOverride = { id: '12345', tag_name: TAG, draft: true, assets: [] };
        await assert.rejects(f.guard.stage(f.context),
            /CREATE_OUTCOME_UNKNOWN: Unexpected create response/);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 0);
    });
    await test('P20-S2b: externally published draft before upload => zero upload', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.afterCreateMutation = state => { state.release.draft = false; };
        await assert.rejects(f.guard.stage(f.context),
            /Created draft GraphQL ID\/state mismatch/);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 0);
    });
    await test('P20-S2b: foreign pre-existing asset after create => no upload', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.afterCreateMutation = state => {
            state.release.assets.push({
                id: 987, name: 'foreign-file', size: 10,
                state: 'uploaded', digest: 'sha256:' + 'a'.repeat(64)
            });
        };
        await assert.rejects(f.guard.stage(f.context),
            /Created draft ID\/tag\/state\/assets changed/);
        assert.equal(f.state.uploadCalls, 0);
    });
    await test('P20-S2b: HTTP 409 on asset upload => no retry/delete', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.uploadFailure = 'before';
        await assert.rejects(f.guard.stage(f.context),
            /ASSET_UPLOAD_OUTCOME_UNKNOWN:.*HTTP 409/);
        assert.equal(f.state.createCalls, 1);
        assert.equal(f.state.uploadCalls, 1);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('P20-S2b: lost upload response => asset may exist, never auto retry', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.uploadFailure = 'after';
        await assert.rejects(f.guard.stage(f.context),
            /ASSET_UPLOAD_OUTCOME_UNKNOWN:.*Upload response lost/);
        assert.equal(f.state.uploadCalls, 1);
        assert.equal(f.state.release.assets.length, 1);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('P20-S2b: post-upload external asset mutation fails, never public', async () => {
        const f = make();
        f.state.graphqlNull = true;
        f.state.afterUploadMutation = state => {
            state.release.assets[0].digest = 'sha256:' + '0'.repeat(64);
        };
        await assert.rejects(f.guard.stage(f.context),
            /Created draft asset identity changed or unexpected asset present/);
        assert.equal(f.state.uploadCalls, 1);
        assert.equal(f.state.patchCalls, 0);
    });
    await test('P20-S2a-002: GraphQL ID 99999 REST 200 JSON ID 12345 is rejected', async () => {
        const f = make();
        f.state.releaseId = 99999; // GraphQL resolves 99999, mocked REST route 200 returns id=12345.
        await assert.rejects(f.guard.prepare(f.context),
            /Draft Release ID lookup inconsistent/);
        assert.equal(f.state.patchCalls, 0);
    });
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
