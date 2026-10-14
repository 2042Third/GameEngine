"""Prints the annotations (any level) of the check runs of a commit whose names match an argument."""
import json
import sys
import urllib.request

REPO = "https://api.github.com/repos/2042Third/GameEngine"


def get(url):
    request = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json", "User-Agent": "strata-ci-poll"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


for run in get(REPO + "/commits/" + sys.argv[1] + "/check-runs?per_page=100")["check_runs"]:
    if run["output"]["annotations_count"] == 0:
        continue
    for annotation in get(run["output"]["annotations_url"]):
        print("==", run["name"], annotation["annotation_level"], annotation.get("title"))
        print(annotation["message"])
