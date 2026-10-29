"""Upload a complete package to a draft; publish only after every asset verifies."""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path
from release_assets import REPOSITORY, WEB, publishable_native, require, sha, validate, verify_file, write


class Github:
    def api(self, endpoint, missing=False):
        run = subprocess.run(['gh', 'api', endpoint], capture_output=True, encoding='utf-8')
        if missing and run.returncode and 'HTTP 404' in run.stderr:
            return None
        if run.returncode:
            raise RuntimeError(run.stderr.strip() or 'GitHub API request failed.')
        return json.loads(run.stdout)

    def release(self, *args):
        subprocess.run(['gh', 'release', *args, '--repo', REPOSITORY], check=True)


def find_release(github, tag):
    remote = github.api(f'repos/{REPOSITORY}/releases/tags/{tag}', missing=True)
    if remote is not None:
        return remote
    # The by-tag endpoint only promises published releases. Authenticated listing
    # includes drafts, which must be found when resuming an interrupted upload.
    page = 1
    while True:
        releases = github.api(f'repos/{REPOSITORY}/releases?per_page=100&page={page}')
        matches = [r for r in releases if r['tag_name'] == tag]
        require(len(matches) <= 1, 'Multiple drafts use this tag; resolve them before publishing.')
        if matches:
            return matches[0]
        if len(releases) < 100:
            return None
        page += 1


def verify_remote(out, plan, remote, github, complete=False):
    require(remote['tag_name'] == plan['tag'], 'GitHub release tag differs from the build.')
    require(remote['target_commitish'] == plan['revision'], 'Existing release targets another commit; retained.')
    require(remote['prerelease'] == (plan['channel'] == 'Experimental'), 'Release channel differs from the build.')
    require(remote.get('body', '').replace('\r\n', '\n').strip() == (out/'ReleaseNotes.md').read_text(encoding='utf-8-sig').strip(), 'Existing release notes differ; retained.')
    expected = {a['name']:a for a in plan['assets']}
    uploaded = {a['name']:a for a in remote['assets']}
    require(len(uploaded) == len(remote['assets']) and set(uploaded) <= set(expected), 'Unexpected or duplicate release assets; retained.')
    if complete:
        require(set(uploaded) == set(expected), 'Release is missing required assets, including its texture pack.')
    for name, item in uploaded.items():
        pinned = expected[name]
        require(item['state'] == 'uploaded' and item['size'] == pinned['size'], f'Incomplete uploaded asset: {name}')
        digest = item.get('digest')
        if digest:
            require(digest == 'sha256:'+pinned['sha256'], f'Uploaded checksum mismatch: {name}; retained.')
        else:
            # Older GitHub responses may omit digest. Verify downloaded bytes then.
            with tempfile.TemporaryDirectory(prefix='release-verify-', dir=out) as temporary:
                github.release('download', plan['tag'], '--pattern', name, '--dir', temporary)
                require(sha(Path(temporary)/name) == pinned['sha256'], f'Uploaded checksum mismatch: {name}; retained.')
        if not remote['draft']:
            require(item['browser_download_url'] == pinned['url'], f'Published download URL differs: {name}')
    return set(expected)-set(uploaded)


def publish(out, github=None):
    plan = validate(out)  # No network writes if a local asset is absent/changed.
    publishable_native(plan)
    github = github or Github()
    remote = find_release(github, plan['tag'])
    require(remote is None or remote['draft'], 'This release is already published. Existing assets are retained; use a new build version.')
    commit = github.api(f'repos/{REPOSITORY}/commits/{plan["revision"]}')
    require(commit['sha'] == plan['revision'], 'Push the packaged source commit before publishing.')
    tag = github.api(f'repos/{REPOSITORY}/commits/{plan["tag"]}', missing=True)
    require(tag is None or tag['sha'] == plan['revision'], 'Existing GitHub tag points to another commit; retained.')
    if remote is None:
        args = ['create', plan['tag'], '--draft', '--target', plan['revision'], '--title', f'GRID2VR {plan["version"]} {plan["channel"]}',
                '--notes-file', str(out/'ReleaseNotes.md'), '--latest=false']
        if plan['channel'] == 'Experimental':
            args.append('--prerelease')
        github.release(*args)
        remote = find_release(github, plan['tag'])
        require(remote is not None, 'Created draft was not returned by GitHub; retry after checking the release.')
    endpoint = f'repos/{REPOSITORY}/releases/{remote["id"]}'
    missing = verify_remote(out, plan, remote, github)
    for item in plan['assets']:
        if item['name'] in missing:
            # No --clobber: retries reuse matching draft assets, never replace them.
            verify_file(out/item['name'], item)
            github.release('upload', plan['tag'], str(out/item['name']))
    remote = github.api(endpoint)
    require(remote['draft'], 'Release was published externally before verification.')
    verify_remote(out, plan, remote, github, complete=True)
    github.release('edit', plan['tag'], '--draft=false', '--latest='+('true' if plan['channel'] == 'Stable' else 'false'))
    remote = github.api(endpoint)
    require(not remote['draft'], 'GitHub did not publish the verified draft.')
    verify_remote(out, plan, remote, github, complete=True)
    receipt = dict(published=True, tag=plan['tag'], url=f'{WEB}/releases/tag/{plan["tag"]}', assets=plan['assets'])
    write(out/'publish-receipt.json', receipt)
    return receipt


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--check', action='store_true', help='Validate the local publication set without contacting GitHub.')
    args = parser.parse_args()
    print(json.dumps(validate(args.package.resolve()) if args.check else publish(args.package.resolve()), indent=2))
