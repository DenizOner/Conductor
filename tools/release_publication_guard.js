'use strict';

/**
 * @file release_publication_guard.js
 * @brief Verify a pinned GitHub draft and publish ONLY its numeric release ID.
 *
 * Safety: the 'prepare' command performs GET requests only; 'publish' performs
 * one PATCH after an immediately preceding readback. Never call 'publish' in
 * smoke tests. GitHub does NOT offer compare-and-swap publication: another
 * privileged writer can mutate the draft between GET and PATCH.
 *
 * Usage:
 * node tools/release_publication_guard.js absent <owner/repo> <tag>
 * node tools/release_publication_guard.js stage <dir> <owner/repo> <tag> <40hex-commit> <snapshot-json>
 * node tools/release_publication_guard.js prepare <dir> <owner/repo> <tag> <40hex-commit> <snapshot-json>
 * node tools/release_publication_guard.js publish <dir> <owner/repo> <tag> <40hex-commit> <snapshot-json>
 */
const fs = require('node:fs');
const cp = require('node:child_process');
const path = require('node:path');
const crypto = require('node:crypto');
const { verifyReleaseAssets, NAMES } = require('./verify_release_assets.js');

function requireValid(ok, message) {
    if (!ok) throw new Error(message);
}
function positiveId(value) {
    return Number.isSafeInteger(value) && value > 0;
}
function same(a, b) {
    return JSON.stringify(a) === JSON.stringify(b);
}
function pathFor(ctx, suffix) {
    return 'repos/' + ctx.repository + '/' + suffix;
}
function validateRepositoryTag(ctx) {
    requireValid(ctx && typeof ctx === 'object' &&
        /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/.test(ctx.repository) &&
        /^v[0-9]+\.[0-9]+\.[0-9]+$/.test(ctx.tag),
    'Invalid release repository or tag');
}
function validateContext(ctx) {
    validateRepositoryTag(ctx);
    requireValid(/^[0-9a-f]{40}$/.test(ctx.commit) &&
        typeof ctx.directory === 'string' && ctx.directory.length > 0,
    'Invalid release context');
}

/* Resolve annotated tags as well as lightweight tags to an EXACT commit SHA. */
async function assertLiveTag(api, ctx) {
    const expectedRef = 'refs/tags/' + ctx.tag;
    const ref = await api.get(pathFor(ctx, 'git/ref/tags/' + encodeURIComponent(ctx.tag)));
    requireValid(ref && ref.ref === expectedRef && ref.object, 'Release tag ref mismatch');
    let object = ref.object;
    const seen = new Set();
    for (let depth = 0; depth < 5 && object && object.type === 'tag'; depth++) {
        requireValid(/^[0-9a-f]{40}$/.test(object.sha) && !seen.has(object.sha),
            'Invalid or cyclic annotated tag');
        seen.add(object.sha);
        const annotated = await api.get(pathFor(ctx, 'git/tags/' + object.sha));
        object = annotated && annotated.object;
    }
    requireValid(object && object.type === 'commit' && object.sha === ctx.commit,
        'Release tag does not resolve to the pinned build commit');
}

/* The existing local validator independently checks the four local bytes,
 * their Zigbee OTA/index contents and each draft asset's size/SHA-256. */
function captureDraft(release, ctx, verifyAssets) {
    requireValid(release && positiveId(release.id) && release.draft === true &&
        release.tag_name === ctx.tag, 'Draft Release ID/tag/state mismatch');
    const verifiedFiles = verifyAssets(ctx.directory, ctx.repository, ctx.tag, release);
    requireValid(Array.isArray(verifiedFiles) && verifiedFiles.length === NAMES.length,
        'Local verification did not return four assets');
    const assets = release.assets.map(asset => {
        requireValid(asset && positiveId(asset.id), 'Missing remote asset ID');
        const expected = verifiedFiles.find(file => file.name === asset.name);
        requireValid(expected && asset.size === expected.size &&
            asset.digest === 'sha256:' + expected.sha256 && asset.state === 'uploaded',
        'Draft asset identity/size/digest mismatch');
        return { id: asset.id, name: asset.name, size: asset.size, digest: asset.digest };
    }).sort((a, b) => a.name.localeCompare(b.name));
    requireValid(new Set(assets.map(asset => asset.id)).size === NAMES.length,
        'Duplicate remote asset ID');
    return {
        repository: ctx.repository,
        tag: ctx.tag,
        commit: ctx.commit,
        releaseId: release.id,
        assets
    };
}

function compareReleaseToSnapshot(release, snapshot, expectedDraft) {
    requireValid(release && release.id === snapshot.releaseId &&
        release.tag_name === snapshot.tag && release.draft === expectedDraft,
    'Release ID/tag/state changed since verification');
    requireValid(Array.isArray(release.assets) &&
        release.assets.length === snapshot.assets.length, 'Release asset count changed');
    const assets = release.assets.map(asset => ({
        id: asset && asset.id,
        name: asset && asset.name,
        size: asset && asset.size,
        digest: asset && asset.digest,
        state: asset && asset.state
    })).sort((a, b) => String(a.name).localeCompare(String(b.name)));
    requireValid(assets.every(asset => asset.state === 'uploaded') &&
        same(assets.map(({ state, ...rest }) => rest), snapshot.assets),
    'Release asset ID/name/size/SHA-256 changed since verification');
}

/* GitHub REST releases/tags/<tag> returns PUBLISHED releases, not drafts.
 * GitHub CLI discovers drafts with GraphQL repository.release(tagName) and
 * then fetches their canonical data using GET releases/<databaseId>. */
async function lookupReleaseByGraphql(api, ctx) {
    validateRepositoryTag(ctx);
    const response = await api.graphql(ctx.repository, ctx.tag);
    requireValid(response && typeof response === 'object' &&
        (!Object.hasOwn(response, 'errors') ||
            (Array.isArray(response.errors) && response.errors.length === 0)) &&
        response.data && response.data.repository &&
        Object.hasOwn(response.data.repository, 'release'),
    'GraphQL release lookup incomplete or failed');
    const item = response.data.repository.release;
    if (item === null) return null;
    requireValid(item && positiveId(item.databaseId) &&
        typeof item.isDraft === 'boolean' && item.tagName === ctx.tag,
    'GraphQL release identity malformed or ambiguous');
    return { id: item.databaseId, draft: item.isDraft, tag: item.tagName };
}

async function readDraft(api, ctx) {
    const graph = await lookupReleaseByGraphql(api, ctx);
    requireValid(graph && graph.draft === true, 'No unambiguous draft Release for tag');
    const byId = await api.get(pathFor(ctx, 'releases/' + graph.id));
    requireValid(byId && byId.id === graph.id && byId.draft === true &&
        byId.tag_name === graph.tag, 'Draft Release ID lookup inconsistent');
    return { graph, byId };
}

/* IDs and asset paths are derived from trusted, validated create responses.
 * Never take a user-controlled or API-controlled upload URL as an arbitrary
 * network destination; GitHub.com uploads use this exact numeric-ID route. */
function uploadEndpoint(ctx, releaseId, name) {
    requireValid(positiveId(releaseId) && NAMES.includes(name), 'Invalid new release asset target');
    return 'https://uploads.github.com/' + pathFor(ctx,
        'releases/' + releaseId + '/assets') + '?name=' + encodeURIComponent(name);
}
function assertCreatedDraft(release, ctx, releaseId, expectedAssets) {
    requireValid(release && release.id === releaseId && release.draft === true &&
        release.tag_name === ctx.tag && Array.isArray(release.assets) &&
        release.assets.length === expectedAssets.length, 'Created draft ID/tag/state/assets changed');
    const canonical = release.assets.map(asset => ({
        id: asset && asset.id, name: asset && asset.name,
        size: asset && asset.size, digest: asset && asset.digest,
        state: asset && asset.state
    })).sort((a, b) => String(a.name).localeCompare(String(b.name)));
    requireValid(canonical.every(a => positiveId(a.id) && a.state === 'uploaded') &&
        new Set(canonical.map(a => a.id)).size === canonical.length &&
        same(canonical, expectedAssets.map(a => ({ ...a, state: 'uploaded' }))
            .sort((a, b) => a.name.localeCompare(b.name))),
    'Created draft asset identity changed or unexpected asset present');
}

function createGuard(api, verifyAssets = verifyReleaseAssets,
    readAsset = filename => fs.readFileSync(filename)) {
    requireValid(api && typeof api.get === 'function' &&
        typeof api.graphql === 'function' && typeof api.patch === 'function' &&
        typeof api.create === 'function' && typeof api.upload === 'function',
        'Missing GitHub API adapter');
    return {
        async absent(ctx) {
            validateRepositoryTag(ctx);
            const found = await lookupReleaseByGraphql(api, ctx);
            requireValid(found === null,
                'Release already exists (draft or published); refusing overwrite');
            return true;
        },
        /* Create-only: one REST POST. NEVER use the upsert/overwrite semantics
         * of a GitHub Release Action. The response ID owns every subsequent
         * asset upload. Any uncertain POST/upload response stops without retry,
         * cleanup or mutation of any pre-existing Release. */
        async stage(ctx) {
            validateContext(ctx);
            const verified = verifyAssets(ctx.directory, ctx.repository, ctx.tag);
            requireValid(Array.isArray(verified) && verified.length === NAMES.length &&
                NAMES.every(n => verified.some(f => f.name === n)),
            'Exactly four verified local assets required before create');
            await assertLiveTag(api, ctx);
            const preflight = await lookupReleaseByGraphql(api, ctx);
            requireValid(preflight === null,
                'Release already exists (draft or published); refusing create-only POST');
            await assertLiveTag(api, ctx);
            const payload = {
                tag_name: ctx.tag,
                target_commitish: ctx.commit,
                name: 'Release ' + ctx.tag + ' — Native Zigbee OTA Upgrade',
                draft: true,
                prerelease: false,
                generate_release_notes: true
            };
            let created;
            try {
                created = await api.create(pathFor(ctx, 'releases'), payload);
            } catch (err) {
                throw new Error('CREATE_OUTCOME_UNKNOWN: REST create-only POST may have ' +
                    'succeeded; do not retry or delete; inspect GitHub manually: ' + err.message);
            }
            const id = created && created.id;
            requireValid(positiveId(id) && created.draft === true &&
                created.tag_name === ctx.tag && Array.isArray(created.assets) &&
                created.assets.length === 0,
            'CREATE_OUTCOME_UNKNOWN: Unexpected create response; no upload/retry');
            const expected = [];
            const url = pathFor(ctx, 'releases/' + id);
            /* Do not upload to any Release other than the exact ID returned
             * from this successful create-only POST. */
            const check = async () => {
                await assertLiveTag(api, ctx);
                const graph = await lookupReleaseByGraphql(api, ctx);
                requireValid(graph && graph.id === id && graph.draft === true,
                    'Created draft GraphQL ID/state mismatch');
                const live = await api.get(url);
                assertCreatedDraft(live, ctx, id, expected);
                return live;
            };
            /* After a successful create response, even a readback failure can
             * leave a real remote draft or uploaded asset behind. Diagnose the
             * remote outcome as UNKNOWN, without attempting recovery/retry. */
            let uploadAttempted = false;
            const checked = async () => {
                try {
                    return await check();
                } catch (err) {
                    const kind = uploadAttempted ?
                        'ASSET_UPLOAD_OUTCOME_UNKNOWN' : 'CREATE_OUTCOME_UNKNOWN';
                    throw new Error(kind + ': post-create draft/asset readback failed; ' +
                        'remote Release/assets may exist; inspect GitHub manually, ' +
                        'do not retry/delete/rollback: ' + err.message);
                }
            };
            await checked();
            for (const file of verified) {
                requireValid(NAMES.includes(file.name) && typeof file.sha256 === 'string' &&
                    /^[0-9a-f]{64}$/.test(file.sha256) && file.size > 0,
                'Malformed local asset evidence');
                await checked(); // numeric-ID and unexpected-assets guard, no auto retry
                const localPath = path.join(ctx.directory, file.name);
                const data = readAsset(localPath);
                requireValid(data.length === file.size &&
                    crypto.createHash('sha256').update(data).digest('hex') === file.sha256,
                'Local release asset changed since verification');
                let uploaded;
                uploadAttempted = true;
                try {
                    uploaded = await api.upload(uploadEndpoint(ctx, id, file.name), localPath);
                } catch (err) {
                    throw new Error('ASSET_UPLOAD_OUTCOME_UNKNOWN: asset may already be ' +
                        'uploaded; do not retry/delete/overwrite: ' + err.message);
                }
                requireValid(uploaded && positiveId(uploaded.id) &&
                    uploaded.name === file.name && uploaded.size === file.size &&
                    uploaded.digest === 'sha256:' + file.sha256 &&
                    uploaded.state === 'uploaded' &&
                    expected.every(asset => asset.id !== uploaded.id),
                'ASSET_UPLOAD_OUTCOME_UNKNOWN: Unexpected upload response; no retry/delete');
                expected.push({
                    id: uploaded.id, name: uploaded.name,
                    size: uploaded.size, digest: uploaded.digest
                });
                await checked();
            }
            const final = await checked();
            const snapshot = captureDraft(final, ctx, verifyAssets);
            requireValid(snapshot.releaseId === id &&
                same(snapshot.assets, [...expected].sort((a, b) => a.name.localeCompare(b.name))),
            'Created draft snapshot differs from verified uploaded asset identities');
            try {
                await assertLiveTag(api, ctx);
            } catch (err) {
                throw new Error('ASSET_UPLOAD_OUTCOME_UNKNOWN: final tag readback failed; ' +
                    'remote draft/assets may exist; inspect GitHub manually, ' +
                    'do not retry/delete/rollback: ' + err.message);
            }
            return snapshot;
        },
        async prepare(ctx) {
            validateContext(ctx);
            await assertLiveTag(api, ctx);
            const release = await readDraft(api, ctx);
            const snapshot = captureDraft(release.byId, ctx, verifyAssets);
            requireValid(release.graph.id === snapshot.releaseId && release.graph.tag === snapshot.tag,
                'Draft GraphQL identity changed since verification');
            await assertLiveTag(api, ctx);
            return snapshot;
        },
        async publish(ctx, snapshot) {
            validateContext(ctx);
            requireValid(snapshot && snapshot.repository === ctx.repository &&
                snapshot.tag === ctx.tag && snapshot.commit === ctx.commit &&
                positiveId(snapshot.releaseId) &&
                Array.isArray(snapshot.assets) && snapshot.assets.length === NAMES.length,
            'Missing or wrong release snapshot');
            await assertLiveTag(api, ctx);
            const current = await readDraft(api, ctx);
            requireValid(current.byId.id === snapshot.releaseId,
                'Draft Release ID replaced since prepared snapshot');
            const verifiedNow = captureDraft(current.byId, ctx, verifyAssets);
            requireValid(same(verifiedNow, snapshot),
                'Draft Release or asset identity changed since prepared snapshot');
            compareReleaseToSnapshot(current.byId, snapshot, true);
            requireValid(current.graph.id === snapshot.releaseId &&
                current.graph.tag === snapshot.tag && current.graph.draft === true,
                'Draft GraphQL identity changed since verification');
            await assertLiveTag(api, ctx);

            /* LAST mutation: ID-bound PATCH; never PATCH by tag or name. The
             * prepublish GET and PATCH remain non-atomic against other writers. */
            const url = pathFor(ctx, 'releases/' + snapshot.releaseId);
            let result;
            try {
                result = await api.patch(url, { draft: false });
            } catch (err) {
                throw new Error('PUBLICATION_OUTCOME_UNKNOWN: PATCH may have succeeded; ' +
                    'inspect GitHub Release manually, do not auto-retry: ' + err.message);
            }
            try {
                compareReleaseToSnapshot(result, snapshot, false);
                const live = await api.get(url);
                compareReleaseToSnapshot(live, snapshot, false);
                const graph = await lookupReleaseByGraphql(api, ctx);
                requireValid(graph && graph.id === snapshot.releaseId &&
                    graph.tag === snapshot.tag && graph.draft === false,
                    'Published GraphQL Release identity mismatch');
                /* REST by-tag is appropriate ONLY once the release is public. */
                const published = await api.get(pathFor(ctx, 'releases/tags/' +
                    encodeURIComponent(ctx.tag)));
                compareReleaseToSnapshot(published, snapshot, false);
                await assertLiveTag(api, ctx);
            } catch (err) {
                throw new Error('PUBLICATION_OUTCOME_UNKNOWN: Release may already be public; ' +
                    'manual inspection required, no auto-rollback/retry: ' + err.message);
            }
            return { releaseId: snapshot.releaseId, tag: ctx.tag, published: true };
        }
    };
}

/* gh before v2.102.0 had source-ref case-folding and signer-workflow prefix
 * policy defects. Do not use an affected binary to authorize release assets. */
function requirePatchedGh(versionText) {
    requireValid(typeof versionText === 'string', 'Cannot determine GitHub CLI security patch version');
    const firstLine = versionText.split(/\r?\n/, 1)[0].trim();
    /* Match the FULL stable version token. Pre-releases (including 2.102.0-rc.1)
     * must not inherit the stable release's security-patch guarantee. */
    const match = /^gh version ([0-9]+)\.([0-9]+)\.([0-9]+)(?: \([0-9]{4}-[0-9]{2}-[0-9]{2}\))?$/.exec(firstLine);
    requireValid(match, 'Cannot determine stable GitHub CLI security patch version');
    const parts = match.slice(1).map(Number);
    requireValid(parts.every(Number.isSafeInteger), 'Invalid GitHub CLI version number');
    requireValid(parts[0] > 2 || (parts[0] === 2 &&
        (parts[1] > 102 || (parts[1] === 102 && parts[2] >= 0))),
    'GitHub CLI 2.102.0+ required for provenance policy checks');
}

function attestationArgs(directory, repository, ref, commit, workflow) {
    requireValid(/^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/.test(repository),
        'Invalid attestation repository');
    requireValid(/^refs\/(tags\/v[0-9]+\.[0-9]+\.[0-9]+|pull\/[0-9]+\/merge)$/.test(ref),
        'Invalid attestation source ref');
    requireValid(/^[0-9a-f]{40}$/.test(commit), 'Invalid attestation source commit');
    requireValid(workflow === 'release_ota.yml' ||
        workflow === 'release_asset_smoke.yml', 'Unapproved signer workflow');
    return NAMES.map(name => ['attestation', 'verify',
        require('node:path').join(directory, name),
        '--repo', repository,
        '--source-digest', commit,
        '--source-ref', ref,
        '--signer-workflow', repository + '/.github/workflows/' + workflow]);
}

function verifyAttestations(directory, repository, ref, commit, workflow, run) {
    requirePatchedGh(run(['--version']));
    const commands = attestationArgs(directory, repository, ref, commit, workflow);
    for (const args of commands) {
        run(args);
    }
    return commands.length;
}

function ghText(args) {
    const result = cp.spawnSync('gh', args, { encoding: 'utf8', maxBuffer: 4 * 1024 * 1024 });
    if (result.error) throw result.error;
    requireValid(result.status === 0, 'GitHub CLI command failed (' + result.status +
        '): ' + (result.stderr || '').slice(0, 500));
    return result.stdout;
}

function ghJson(args, input) {
    const result = cp.spawnSync('gh', args, { input, encoding: 'utf8', maxBuffer: 4 * 1024 * 1024 });
    if (result.error) throw result.error;
    requireValid(result.status === 0,
        'GitHub API failed (' + result.status + '): ' + (result.stderr || '').slice(0, 500));
    return JSON.parse(result.stdout);
}
const RELEASE_QUERY = 'query($owner: String!, $name: String!, $tagName: String!) ' +
    '{ repository(owner: $owner, name: $name) ' +
    '{ release(tagName: $tagName) { databaseId isDraft tagName } } }';
const ghApi = {
    get: endpoint => Promise.resolve(ghJson(['api', endpoint])),
    create: (endpoint, payload) => Promise.resolve(ghJson([
        'api', '-X', 'POST', endpoint, '-H', 'Content-Type: application/json',
        '--input', '-'
    ], JSON.stringify(payload))),
    upload: (endpoint, filename) => {
        requireValid(/^https:\/\/uploads\.github\.com\/repos\/[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+\/releases\/[1-9][0-9]*\/assets\?name=/.test(endpoint),
            'Refuse noncanonical upload endpoint');
        return Promise.resolve(ghJson([
            'api', '-X', 'POST', endpoint,
            '-H', 'Content-Type: application/octet-stream',
            '--input', filename
        ]));
    },
    graphql: (repository, tag) => {
        const [owner, name] = repository.split('/');
        return Promise.resolve(ghJson(['api', 'graphql',
            '-f', 'query=' + RELEASE_QUERY,
            '-f', 'owner=' + owner, '-f', 'name=' + name, '-f', 'tagName=' + tag]));
    },
    patch: (endpoint, body) => {
        requireValid(body && body.draft === false, 'Only draft=false publication is supported');
        return Promise.resolve(ghJson(['api', '-X', 'PATCH', endpoint, '-F', 'draft=false']));
    }
};

/* A local snapshot write failure cannot undo a staged remote draft.
 * Read-only prepare retains its original file-write error contract. */
function persistSnapshot(snapshotFile, snapshot, mode, writeFile = fs.writeFileSync) {
    try {
        writeFile(snapshotFile, JSON.stringify(snapshot, null, 2) + '\n', { flag: 'wx' });
    } catch (err) {
        if (mode === 'stage') {
            throw new Error('ASSET_UPLOAD_OUTCOME_UNKNOWN: snapshot write failed; ' +
                'remote draft/four assets may exist; inspect GitHub manually, ' +
                'do not retry/delete/rollback: ' + err.message);
        }
        throw err;
    }
}

async function main() {
    const [mode, directory, repository, tag, commit, snapshotFile] = process.argv.slice(2);
    if (mode === 'absent') {
        const guard = createGuard(ghApi);
        await guard.absent({ repository: directory, tag: repository });
        console.log('[PASS] No existing draft or published Release for tag');
        return;
    }
    if (mode === 'attest') {
        const [ref, sourceCommit, signerWorkflow] = [tag, commit, snapshotFile];
        const verified = verifyAttestations(directory, repository, ref,
            sourceCommit, signerWorkflow, ghText);
        console.log('[PASS] ' + verified + ' source-commit/ref/signer pinned attestations');
        return;
    }
    requireValid((mode === 'stage' || mode === 'prepare' || mode === 'publish') && snapshotFile,
        'Usage: release_publication_guard.js stage|prepare|publish <dir> <repo> <tag> <commit> <snapshot>');
    const ctx = { directory, repository, tag, commit };
    const guard = createGuard(ghApi);
    if (mode === 'prepare' || mode === 'stage') {
        /* Refuse an existing snapshot BEFORE any create-only POST. */
        requireValid(!fs.existsSync(snapshotFile), 'Snapshot already exists; no create/retry');
        const snapshot = mode === 'stage' ? await guard.stage(ctx) : await guard.prepare(ctx);
        persistSnapshot(snapshotFile, snapshot, mode);
        console.log('[PASS] Newly staged DRAFT Release ID and 4 exact asset identities; not public');
    } else {
        const snapshot = JSON.parse(fs.readFileSync(snapshotFile, 'utf8'));
        const published = await guard.publish(ctx, snapshot);
        console.log('[PASS] Release ID ' + published.releaseId +
            ' published and read back; external writer race is not atomic');
    }
}
if (require.main === module) {
    main().catch(err => {
        console.error('[FAIL] Release gate: ' + err.message);
        process.exitCode = 1;
    });
}
module.exports = { createGuard, validateContext, captureDraft, compareReleaseToSnapshot,
    assertLiveTag, lookupReleaseByGraphql, uploadEndpoint, assertCreatedDraft,
    requirePatchedGh, attestationArgs, verifyAttestations, persistSnapshot };
