#!/usr/bin/env python3
"""Reads the GitHub Actions results of a commit of this repository: the state of its runs and jobs, and the annotations
the jobs left (.github/scripts/annotate_failures.py writes the failures there, readable without a job log).

    python3 Tools/CIStatus.py status <commit>          # one line per run and job, with failed steps (a hash or a branch)
    python3 Tools/CIStatus.py annotations <commit>        # the failure annotations of the jobs that did not succeed
    python3 Tools/CIStatus.py annotations <commit> --all  # every annotation of every check run (notices too)

The repository is origin's (GitHub). The API allows 60 requests an hour without a token: the tool uses GH_TOKEN or
GITHUB_TOKEN, else `gh auth token` when the GitHub CLI is signed in, and never prints it.

Exit codes of status: 0 when every run completed, 3 while something still runs or no run started yet, 2 on API or usage
errors. annotations: 0, or 2 on errors. Only the standard library is used.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request


def find_repository():
    url = subprocess.run(['git', 'remote', 'get-url', 'origin'], capture_output=True, text=True,
                         cwd=os.path.dirname(os.path.abspath(__file__))).stdout.strip()
    match = re.search(r'github\.com[:/]([^/]+)/([^/]+?)(\.git)?$', url)
    if not match:
        raise RuntimeError("origin is not a GitHub repository: '%s'" % url)
    return 'https://api.github.com/repos/%s/%s' % (match.group(1), match.group(2))


def resolve_commit(revision):
    """The full hash of a revision (a short hash, a branch): the API knows commits by their full hashes only."""
    result = subprocess.run(['git', 'rev-parse', '--verify', '--quiet', revision + '^{commit}'], capture_output=True, text=True,
                            cwd=os.path.dirname(os.path.abspath(__file__)))
    sha = result.stdout.strip()
    if result.returncode != 0 or not re.fullmatch(r'[0-9a-f]{40}', sha):
        raise RuntimeError("'%s' does not name a commit of this repository (fetch it first?)" % revision)
    return sha


def find_token():
    for variable in ('GH_TOKEN', 'GITHUB_TOKEN'):
        if os.environ.get(variable):
            return os.environ[variable]
    if shutil.which('gh'):
        result = subprocess.run(['gh', 'auth', 'token'], capture_output=True, text=True)
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
    return None


class GitHub:
    def __init__(self):
        self.repository = find_repository()
        self.token = find_token()

    def get(self, url):
        if not url.startswith('https://'):
            url = self.repository + url
        headers = {'Accept': 'application/vnd.github+json', 'User-Agent': 'strata-ci-status'}
        if self.token:
            headers['Authorization'] = 'Bearer ' + self.token
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30) as response:
            return json.load(response)


def status(github, sha):
    runs = github.get('/actions/runs?head_sha=' + sha).get('workflow_runs', [])
    if not runs:
        print('no runs yet')
        return 3
    pending = False
    for run in runs:
        print('run', run['id'], run['name'], run['status'], run.get('conclusion'), run['html_url'])
        pending = pending or run['status'] != 'completed'
        for job in github.get('/actions/runs/%d/jobs' % run['id']).get('jobs', []):
            print('  job', job['name'], job['status'], job.get('conclusion'))
            for step in job.get('steps', []):
                if step.get('conclusion') not in (None, 'success', 'skipped'):
                    print('    step', step['name'], step.get('conclusion'))
    return 3 if pending else 0


def annotations(github, sha, every, limit):
    if every:
        for run in github.get('/commits/%s/check-runs?per_page=100' % sha)['check_runs']:
            if run['output']['annotations_count'] == 0:
                continue
            for annotation in github.get(run['output']['annotations_url']):
                print('==', run['name'], annotation['annotation_level'], annotation.get('title'))
                print((annotation.get('message') or '')[:limit])
        return 0
    for run in github.get('/actions/runs?head_sha=' + sha).get('workflow_runs', []):
        for job in github.get('/actions/runs/%d/jobs' % run['id']).get('jobs', []):
            print('=== job', job['name'], job['status'], job.get('conclusion'))
            if job.get('conclusion') in ('success', None):
                continue
            for annotation in github.get('/check-runs/%d/annotations?per_page=100' % job['id']):
                if annotation.get('annotation_level') != 'failure':
                    continue
                print('--- %s:%s %s' % (annotation.get('path'), annotation.get('start_line'), annotation.get('title') or ''))
                print((annotation.get('message') or '')[:limit])
    return 0


def main():
    parser = argparse.ArgumentParser(description='Reads the GitHub Actions results of a commit.')
    parser.add_argument('command', choices=['status', 'annotations'])
    parser.add_argument('sha', help='the commit (a hash, short or full, or a branch)')
    parser.add_argument('--all', action='store_true', help='annotations: every annotation of every check run')
    parser.add_argument('--limit', type=int, default=20000, help='annotations: characters per annotation (default 20000)')
    options = parser.parse_args()
    try:
        sha = resolve_commit(options.sha)
        github = GitHub()
        if options.command == 'status':
            return status(github, sha)
        return annotations(github, sha, options.all, options.limit)
    except (urllib.error.URLError, RuntimeError, ValueError) as error:
        print('api error:', error)
        return 2


if __name__ == '__main__':
    sys.exit(main())
