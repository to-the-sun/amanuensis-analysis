import os
import sys
import json
import tempfile
import unittest
from unittest.mock import patch, MagicMock

# Ensure parent directory is in sys.path for imports
_script_dir = os.path.dirname(os.path.abspath(__file__))
if _script_dir not in sys.path:
    sys.path.insert(0, _script_dir)

from jules_api import JulesAPI, parse_unidiff_patch, submit_to_jules


class TestUnidiffPatchParser(unittest.TestCase):
    def test_parse_new_file_patch(self):
        patch_text = (
            "diff --git a/ordered_poem_lines.txt b/ordered_poem_lines.txt\n"
            "new file mode 100644\n"
            "--- /dev/null\n"
            "+++ b/ordered_poem_lines.txt\n"
            "@@ -0,0 +1,3 @@\n"
            "+First line\n"
            "+Second line\n"
            "+Third line\n"
        )
        res = parse_unidiff_patch(patch_text, "ordered_poem_lines.txt")
        self.assertEqual(res, "First line\nSecond line\nThird line")

    def test_parse_unrelated_file_patch(self):
        patch_text = (
            "diff --git a/other_file.txt b/other_file.txt\n"
            "--- a/other_file.txt\n"
            "+++ b/other_file.txt\n"
            "@@ -1,2 +1,2 @@\n"
            "-Unused\n"
            "+Used\n"
        )
        res = parse_unidiff_patch(patch_text, "ordered_poem_lines.txt")
        self.assertIsNone(res)


class TestJulesAPI(unittest.TestCase):
    def setUp(self):
        self.api_key = "test_jules_api_key_123"
        self.client = JulesAPI(
            api_url="https://jules.googleapis.com/v1alpha",
            api_key=self.api_key,
            source_repo="to_the_sun/amanuensis-analysis",
            poll_interval=0.1,
            poll_timeout=2.0,
        )

    def test_headers(self):
        headers = self.client._get_headers()
        self.assertEqual(headers.get("x-goog-api-key"), self.api_key)
        self.assertEqual(headers.get("Content-Type"), "application/json")

    @patch("requests.get")
    def test_resolve_source_success(self, mock_get):
        mock_resp = MagicMock()
        mock_resp.status_code = 200
        mock_resp.json.return_value = {
            "sources": [
                {
                    "name": "sources/github-to-the-sun-amanuensis-analysis",
                    "id": "github-to-the-sun-amanuensis-analysis",
                    "githubRepo": {
                        "owner": "to-the-sun",
                        "repo": "amanuensis-analysis",
                        "defaultBranch": {
                            "displayName": "main"
                        }
                    }
                }
            ]
        }
        mock_get.return_value = mock_resp

        s_name, s_branch = self.client.resolve_source()
        self.assertEqual(s_name, "sources/github-to-the-sun-amanuensis-analysis")
        self.assertEqual(s_branch, "main")

    @patch("requests.post")
    def test_create_session(self, mock_post):
        mock_resp = MagicMock()
        mock_resp.status_code = 200
        mock_resp.json.return_value = {
            "name": "sessions/sess123",
            "id": "sess123",
            "state": "QUEUED"
        }
        mock_post.return_value = mock_resp

        session = self.client.create_session("Test prompt", title="Test Title", source_name="sources/github-test", starting_branch="main")
        self.assertEqual(session["id"], "sess123")

        mock_post.assert_called_once()
        _, kwargs = mock_post.call_args
        payload = kwargs["json"]
        self.assertEqual(payload["prompt"], "Test prompt")
        self.assertEqual(payload["requirePlanApproval"], False)
        self.assertEqual(payload["sourceContext"]["source"], "sources/github-test")
        self.assertEqual(payload["sourceContext"]["githubRepoContext"]["startingBranch"], "main")

    @patch("requests.post")
    def test_approve_plan(self, mock_post):
        mock_resp = MagicMock()
        mock_resp.status_code = 200
        mock_post.return_value = mock_resp

        res = self.client.approve_plan("sessions/sess123")
        self.assertTrue(res)

    @patch("requests.get")
    def test_poll_session_completed(self, mock_get):
        resp_queued = MagicMock()
        resp_queued.status_code = 200
        resp_queued.json.return_value = {"name": "sessions/sess123", "state": "IN_PROGRESS"}

        resp_completed = MagicMock()
        resp_completed.status_code = 200
        resp_completed.json.return_value = {"name": "sessions/sess123", "state": "COMPLETED"}

        mock_get.side_effect = [resp_queued, resp_completed]

        res = self.client.poll_session("sess123")
        self.assertEqual(res["state"], "COMPLETED")

    @patch("requests.get")
    def test_get_activities_and_extract_patch(self, mock_get):
        mock_resp = MagicMock()
        mock_resp.status_code = 200
        mock_resp.json.return_value = {
            "activities": [
                {
                    "name": "sessions/sess123/activities/act1",
                    "artifacts": [
                        {
                            "changeSet": {
                                "gitPatch": {
                                    "unidiffPatch": (
                                        "diff --git a/ordered.txt b/ordered.txt\n"
                                        "--- a/ordered.txt\n"
                                        "+++ b/ordered.txt\n"
                                        "@@ -0,0 +1,2 @@\n"
                                        "+Line 1\n"
                                        "+Line 2\n"
                                    )
                                }
                            }
                        }
                    ]
                }
            ]
        }
        mock_get.return_value = mock_resp

        activities = self.client.get_activities("sess123")
        extracted = self.client.extract_reordered_content_from_activities(activities, "ordered.txt")
        self.assertEqual(extracted, "Line 1\nLine 2")

    @patch("requests.get")
    @patch("requests.post")
    def test_submit_file_and_prompt_end_to_end(self, mock_post, mock_get):
        # Mock source listing response
        mock_sources_resp = MagicMock()
        mock_sources_resp.status_code = 200
        mock_sources_resp.json.return_value = {
            "sources": [{"name": "sources/github-repo", "id": "github-repo"}]
        }

        # Mock session creation response
        mock_create_resp = MagicMock()
        mock_create_resp.status_code = 200
        mock_create_resp.json.return_value = {"name": "sessions/s1", "id": "s1", "state": "QUEUED"}

        mock_post.return_value = mock_create_resp

        # Mock poll session response & get activities
        mock_poll_resp = MagicMock()
        mock_poll_resp.status_code = 200
        mock_poll_resp.json.return_value = {"name": "sessions/s1", "state": "COMPLETED"}

        mock_activities_resp = MagicMock()
        mock_activities_resp.status_code = 200
        mock_activities_resp.json.return_value = {
            "activities": [
                {
                    "artifacts": [
                        {
                            "changeSet": {
                                "gitPatch": {
                                    "unidiffPatch": (
                                        "diff --git a/ordered_poem_lines.txt b/ordered_poem_lines.txt\n"
                                        "--- /dev/null\n"
                                        "+++ b/ordered_poem_lines.txt\n"
                                        "@@ -0,0 +1,2 @@\n"
                                        "+Reordered line 1\n"
                                        "+Reordered line 2\n"
                                    )
                                }
                            }
                        }
                    ]
                }
            ]
        }

        def get_side_effect(url, *args, **kwargs):
            if "/sources" in url:
                return mock_sources_resp
            elif "/activities" in url:
                return mock_activities_resp
            else:
                return mock_poll_resp

        mock_get.side_effect = get_side_effect

        temp_dir = tempfile.mkdtemp()
        in_path = os.path.join(temp_dir, "unordered_poem_lines.txt")
        out_path = os.path.join(temp_dir, "ordered_poem_lines.txt")

        with open(in_path, "w", encoding="utf-8") as f:
            f.write("Unordered line 2\nUnordered line 1\n")

        try:
            res_path = self.client.submit_file_and_prompt(in_path, "Reorder lines", output_file_path=out_path)
            self.assertEqual(res_path, out_path)
            with open(out_path, "r", encoding="utf-8") as f:
                content = f.read()
            self.assertEqual(content, "Reordered line 1\nReordered line 2")
        finally:
            if os.path.exists(in_path):
                os.remove(in_path)
            if os.path.exists(out_path):
                os.remove(out_path)
            if os.path.exists(temp_dir):
                os.rmdir(temp_dir)


if __name__ == "__main__":
    unittest.main()
