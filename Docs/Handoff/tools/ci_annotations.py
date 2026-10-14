"""Prints the annotations of every job of the GitHub Actions runs for a commit of 2042Third/GameEngine.
Usage: ci_annotations.py <sha> [max characters per annotation]"""
import json
import sys
import urllib.request

REPO = "https://api.github.com/repos/2042Third/GameEngine"


def get(url):
    request = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json", "User-Agent": "strata-ci-poll"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


limit = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
for run in get(REPO + "/actions/runs?head_sha=" + sys.argv[1]).get("workflow_runs", []):
    for job in get(REPO + "/actions/runs/%d/jobs" % run["id"]).get("jobs", []):
        print("=== job", job["name"], job["status"], job.get("conclusion"))
        if job.get("conclusion") in ("success", None):
            continue
        for annotation in get(REPO + "/check-runs/%d/annotations?per_page=100" % job["id"]):
            if annotation.get("annotation_level") != "failure":
                continue
            title = annotation.get("title") or ""
            location = "%s:%s" % (annotation.get("path"), annotation.get("start_line"))
            print("--- %s %s" % (location, title))
            print((annotation.get("message") or "")[:limit])
