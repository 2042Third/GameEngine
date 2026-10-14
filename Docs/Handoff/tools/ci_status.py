"""Prints the GitHub Actions status of a commit of 2042Third/GameEngine: one line per job (status, conclusion), plus
the failed steps. Exit code 0 when every run completed, 3 while something is still running, 2 on API errors.
Usage: ci_status.py <sha>"""
import json
import sys
import urllib.request

REPO = "https://api.github.com/repos/2042Third/GameEngine"


def get(url):
    request = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json", "User-Agent": "strata-ci-poll"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


try:
    runs = get(REPO + "/actions/runs?head_sha=" + sys.argv[1]).get("workflow_runs", [])
except Exception as error:  # Network or rate-limit problems: report and let the caller retry.
    print("api error:", error)
    sys.exit(2)

if not runs:
    print("no runs yet")
    sys.exit(3)

pending = False
for run in runs:
    print("run", run["id"], run["name"], run["status"], run.get("conclusion"), run["html_url"])
    if run["status"] != "completed":
        pending = True
    try:
        jobs = get(REPO + "/actions/runs/%d/jobs" % run["id"]).get("jobs", [])
    except Exception as error:
        print("  api error:", error)
        sys.exit(2)
    for job in jobs:
        print("  job", job["name"], job["status"], job.get("conclusion"))
        for step in job.get("steps", []):
            if step.get("conclusion") not in (None, "success", "skipped"):
                print("    step", step["name"], step.get("conclusion"))
sys.exit(3 if pending else 0)
