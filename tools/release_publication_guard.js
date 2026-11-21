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
 * node tools/release_publication_guard.js prepare <dir> <owner/repo> <tag> <40hex-commit> <snapshot-json>
 * node tools/release_publication_guard.js publish <dir> <owner/repo> <tag> <40hex-commit> <snapshot-json>
 */
const fs = require('node:fs');
const cp = require('node:child_process');
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

function createGuard(api, verifyAssets = verifyReleaseAssets) {
    requireValid(api && typeof api.get === 'function' &&
        typeof api.graphql === 'function' && typeof api.patch === 'function',
        'Missing GitHub API adapter');
    return {
        async absent(ctx) {
            validateRepositoryTag(ctx);
            const found = await lookupReleaseByGraphql(api, ctx);
            requireValid(found === null,
                'Release already exists (draft or published); refusing overwrite');
            return true;
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

function ghJson(args) {
    const result = cp.spawnSync('gh', args, { encoding: 'utf8', maxBuffer: 4 * 1024 * 1024 });
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
    requireValid((mode === 'prepare' || mode === 'publish') && snapshotFile,
        'Usage: release_publication_guard.js prepare|publish <dir> <repo> <tag> <commit> <snapshot>');
    const ctx = { directory, repository, tag, commit };
    const guard = createGuard(ghApi);
    if (mode === 'prepare') {
        const snapshot = await guard.prepare(ctx);
        fs.writeFileSync(snapshotFile, JSON.stringify(snapshot, null, 2) + '\n', { flag: 'wx' });
        console.log('[PASS] Draft Release ID and 4 exact asset identities captured; no publication');
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
    assertLiveTag, lookupReleaseByGraphql, requirePatchedGh, attestationArgs,
    verifyAttestations };
