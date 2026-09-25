import os
import sys
import json
import time
import logging
import requests

logger = logging.getLogger("jules_api")

try:
    _script_dir = os.path.dirname(os.path.abspath(__file__))
except NameError:
    _script_dir = os.getcwd()


DEFAULT_JULES_API_URL = "https://jules.googleapis.com/v1alpha"
DEFAULT_JULES_SOURCE = "to-the-sun/amanuensis-analysis"


def parse_unidiff_patch(patch_str: str, target_filename: str) -> str:
    """
    Parses a unified diff string and returns the new content for target_filename
    if present in the diff patch.
    """
    if not patch_str:
        return None

    filename_clean = os.path.basename(target_filename)

    # Split patch into file sections
    file_diffs = patch_str.split("diff --git ")
    for diff in file_diffs:
        if not diff.strip():
            continue
        lines = diff.splitlines()
        matches_file = False
        for line in lines[:5]:
            if filename_clean in line or (target_filename and target_filename in line):
                matches_file = True
                break
        if not matches_file:
            continue

        new_lines = []
        in_hunk = False
        for line in lines:
            if line.startswith("@@"):
                in_hunk = True
                continue
            if not in_hunk:
                continue
            if line.startswith("+") and not line.startswith("+++"):
                new_lines.append(line[1:])
            elif line.startswith(" ") and not line.startswith("---"):
                new_lines.append(line[1:])

        if new_lines:
            return "\n".join(new_lines)

    return None


class JulesAPI:
    """
    API client interface for submitting tasks and files to Jules via the official Jules REST API (v1alpha).
    """
    def __init__(
        self,
        api_url: str = None,
        api_key: str = None,
        source_repo: str = None,
        credentials_path: str = None,
        poll_interval: int = 5,
        poll_timeout: int = None,
    ):
        self.api_url = (api_url or os.environ.get("JULES_API_URL") or DEFAULT_JULES_API_URL).rstrip("/")
        self.api_key = api_key or os.environ.get("JULES_API_KEY") or os.environ.get("JULES_API_TOKEN") or os.environ.get("JULES_TOKEN")
        self.source_repo = source_repo or os.environ.get("JULES_SOURCE_REPO") or DEFAULT_JULES_SOURCE
        self.poll_interval = poll_interval
        self.poll_timeout = poll_timeout
        self.key_source = "constructor/env" if self.api_key else None

        if not self.api_key:
            self._load_from_credentials(credentials_path)

        self._log_credential_status()

    def _mask_key(self, key: str) -> str:
        if not key:
            return "<None>"
        if len(key) <= 8:
            return key[0:2] + "..." + key[-1:]
        return key[0:4] + "..." + key[-4:]

    def _log_credential_status(self):
        if self.api_key:
            logger.info(f"Jules API Key found (source: {self.key_source or 'credentials.json'}): {self._mask_key(self.api_key)}")
        else:
            logger.warning("No Jules API key found in constructor, environment (JULES_API_KEY), or credentials.json (key: jules_api_key).")
        logger.info(f"Jules API Base URL: {self.api_url}")
        logger.info(f"Jules Repository Source: {self.source_repo}")

    def _load_from_credentials(self, credentials_path=None):
        dirs_to_check = []
        if credentials_path:
            dirs_to_check.append(credentials_path if os.path.isdir(credentials_path) else os.path.dirname(credentials_path))
            if os.path.isfile(credentials_path) and os.path.exists(credentials_path):
                try:
                    with open(credentials_path, "r", encoding="utf-8") as f:
                        creds = json.load(f)
                    if self._apply_creds_dict(creds):
                        self.key_source = credentials_path
                        return
                except Exception as e:
                    logger.warning(f"Failed to load credentials from {credentials_path}: {e}")

        dirs_to_check.extend([_script_dir, os.getcwd(), os.path.dirname(_script_dir)])
        for d in dirs_to_check:
            cp = os.path.join(d, "credentials.json")
            if os.path.exists(cp):
                try:
                    with open(cp, "r", encoding="utf-8") as f:
                        creds = json.load(f)
                    if self._apply_creds_dict(creds):
                        self.key_source = cp
                        logger.info(f"Loaded Jules API credentials from {cp}")
                        break
                except Exception as e:
                    logger.warning(f"Failed to load credentials from {cp}: {e}")

    def _apply_creds_dict(self, creds: dict) -> bool:
        found = False
        if not self.api_url or self.api_url == DEFAULT_JULES_API_URL:
            c_url = creds.get("jules_api_url") or creds.get("jules_url") or creds.get("JULES_API_URL")
            if c_url:
                self.api_url = c_url.rstrip("/")

        if not self.source_repo or self.source_repo == DEFAULT_JULES_SOURCE:
            c_source = creds.get("jules_source_repo") or creds.get("jules_source") or creds.get("source_repo") or creds.get("source")
            if c_source:
                self.source_repo = c_source

        if not self.api_key:
            extracted_key = (
                creds.get("jules_api_key")
                or creds.get("jules_key")
                or creds.get("jules_api_token")
                or creds.get("jules_token")
                or creds.get("JULES_API_KEY")
                or creds.get("JULES_API_TOKEN")
            )
            if extracted_key:
                self.api_key = extracted_key
                found = True
        return found

    def _get_headers(self) -> dict:
        headers = {"Content-Type": "application/json"}
        if self.api_key:
            headers["x-goog-api-key"] = self.api_key
        return headers

    def resolve_source(self) -> tuple[str, str]:
        """
        Queries GET /v1alpha/sources to resolve a user-configured source string (e.g. 'to_the_sun/amanuensis-analysis')
        to a tuple of (source_resource_name, starting_branch).
        Handles underscore/hyphen normalization (e.g. 'to_the_sun' matching 'to-the-sun').
        """
        default_branch = "main"

        if self.source_repo and self.source_repo.startswith("sources/"):
            return self.source_repo, default_branch

        url = f"{self.api_url}/sources"
        try:
            res = requests.get(url, headers=self._get_headers(), timeout=15)
            if res.status_code == 200:
                sources = res.json().get("sources", [])
                if sources:
                    clean_repo = self.source_repo.strip("/").lower()
                    owner, _, repo_name = clean_repo.rpartition("/")
                    normalized_owner = owner.replace("_", "-")
                    normalized_repo = repo_name.replace("_", "-")

                    for src in sources:
                        gh = src.get("githubRepo", {})
                        s_name = src.get("name", "")
                        s_id = src.get("id", "")
                        gh_owner = (gh.get("owner") or "").lower().replace("_", "-")
                        gh_repo = (gh.get("repo") or "").lower().replace("_", "-")

                        # Extract branch from githubRepo metadata if available
                        def_branch = gh.get("defaultBranch", {}).get("displayName")
                        if not def_branch and gh.get("branches"):
                            def_branch = gh["branches"][0].get("displayName")
                        branch_to_use = def_branch or default_branch

                        if gh_owner == normalized_owner and gh_repo == normalized_repo:
                            logger.info(f"Resolved source '{self.source_repo}' to '{s_name}' (branch: {branch_to_use})")
                            return s_name, branch_to_use

                        if normalized_repo in s_id.lower() or normalized_repo in s_name.lower():
                            logger.info(f"Matched source '{self.source_repo}' to '{s_name}' (branch: {branch_to_use})")
                            return s_name, branch_to_use

                    fallback_src = sources[0]
                    fallback_name = fallback_src.get("name")
                    gh = fallback_src.get("githubRepo", {})
                    def_branch = gh.get("defaultBranch", {}).get("displayName") or default_branch
                    if fallback_name:
                        logger.info(f"Using default available source '{fallback_name}' (branch: {def_branch}) for '{self.source_repo}'")
                        return fallback_name, def_branch
            else:
                logger.warning(f"List Sources API returned HTTP {res.status_code}: {res.text}")
        except Exception as e:
            logger.warning(f"Failed to list Jules sources: {e}")

        sanitized = self.source_repo.replace("/", "-").replace("_", "-")
        s_res = f"sources/{sanitized}" if not sanitized.startswith("sources/") else sanitized
        return s_res, default_branch

    def create_session(self, prompt: str, title: str = None, source_name: str = None, starting_branch: str = None) -> dict:
        """
        Creates a new coding session via POST /v1alpha/sessions.
        Ensures startingBranch inside githubRepoContext is always provided.
        """
        url = f"{self.api_url}/sessions"
        if not source_name or not starting_branch:
            res_source, res_branch = self.resolve_source()
            resolved_source = source_name or res_source
            resolved_branch = starting_branch or res_branch
        else:
            resolved_source = source_name
            resolved_branch = starting_branch

        payload = {
            "prompt": prompt,
            "title": title or "Reorder Poem Lines",
            "requirePlanApproval": False,
            "sourceContext": {
                "source": resolved_source,
                "githubRepoContext": {
                    "startingBranch": resolved_branch or "main"
                }
            }
        }

        logger.info(f"Creating Jules session at {url} (source: {resolved_source}, branch: {resolved_branch or 'main'})...")
        res = requests.post(url, headers=self._get_headers(), json=payload, timeout=30)
        try:
            res.raise_for_status()
        except requests.exceptions.HTTPError as e:
            logger.error(f"Failed to create Jules session (HTTP {res.status_code}): {res.text}")
            raise e

        session_data = res.json()
        logger.info(f"Successfully created Jules session: {session_data.get('name')} (ID: {session_data.get('id')}, State: {session_data.get('state')})")
        return session_data

    def approve_plan(self, session_id: str) -> bool:
        """
        Approves a pending plan in a session via POST /v1alpha/sessions/{sessionId}:approvePlan.
        """
        clean_id = session_id.split("/")[-1]
        url = f"{self.api_url}/sessions/{clean_id}:approvePlan"
        logger.info(f"Approving plan for session {clean_id}...")
        try:
            res = requests.post(url, headers=self._get_headers(), json={}, timeout=15)
            if res.status_code in (200, 204):
                logger.info(f"Plan approved for session {clean_id}")
                return True
            else:
                logger.warning(f"Approve Plan returned HTTP {res.status_code}: {res.text}")
        except Exception as e:
            logger.error(f"Failed to approve plan for session {clean_id}: {e}")
        return False

    def poll_session(self, session_id: str, poll_interval: int = None, timeout: int = None) -> dict:
        """
        Polls GET /v1alpha/sessions/{sessionId} until the session reaches COMPLETED or FAILED state.
        By default, polling continues without timing out unless timeout parameter or poll_timeout is specified.
        """
        clean_id = session_id.split("/")[-1]
        url = f"{self.api_url}/sessions/{clean_id}"
        interval = poll_interval or self.poll_interval
        max_time = timeout if timeout is not None else self.poll_timeout

        start_time = time.time()
        timeout_str = f" (timeout: {max_time}s)" if max_time is not None else " (no timeout)"
        logger.info(f"Polling session {clean_id} every {interval}s{timeout_str}...")

        while True:
            if max_time is not None and (time.time() - start_time >= max_time):
                logger.error(f"Timed out after {max_time}s polling session {clean_id}.")
                return {"name": f"sessions/{clean_id}", "state": "TIMEOUT"}

            try:
                res = requests.get(url, headers=self._get_headers(), timeout=15)
                res.raise_for_status()
                session_data = res.json()
                state = session_data.get("state")
                logger.info(f"Session {clean_id} state: {state}")

                if state == "COMPLETED":
                    return session_data
                elif state == "FAILED":
                    logger.error(f"Session {clean_id} failed.")
                    return session_data
                elif state == "AWAITING_PLAN_APPROVAL":
                    self.approve_plan(clean_id)

            except Exception as e:
                logger.warning(f"Error polling session {clean_id}: {e}")

            time.sleep(interval)

    def get_activities(self, session_id: str) -> list:
        """
        Lists activities for a session via GET /v1alpha/sessions/{sessionId}/activities.
        """
        clean_id = session_id.split("/")[-1]
        url = f"{self.api_url}/sessions/{clean_id}/activities"
        try:
            res = requests.get(url, headers=self._get_headers(), timeout=15)
            if res.status_code == 200:
                return res.json().get("activities", [])
            else:
                logger.warning(f"Get Activities returned HTTP {res.status_code}: {res.text}")
        except Exception as e:
            logger.error(f"Failed to fetch activities for session {clean_id}: {e}")
        return []

    def extract_reordered_content_from_activities(self, activities: list, target_filename: str) -> str:
        """
        Scans session activities for git patches or agent messages containing reordered content.
        """
        for act in reversed(activities):
            artifacts = act.get("artifacts", [])
            for art in artifacts:
                cs = art.get("changeSet", {})
                gp = cs.get("gitPatch", {})
                patch_str = gp.get("unidiffPatch")
                if patch_str:
                    extracted = parse_unidiff_patch(patch_str, target_filename)
                    if extracted:
                        logger.info(f"Extracted reordered lines from activity git patch for {target_filename}")
                        return extracted

        for act in reversed(activities):
            agent_msg = act.get("agentMessaged", {}).get("agentMessage")
            if agent_msg:
                lines = [l.strip() for l in agent_msg.splitlines() if l.strip()]
                if len(lines) > 1:
                    logger.info(f"Extracted reordered lines from agent message for {target_filename}")
                    return "\n".join(lines)

        return None

    def submit_file_and_prompt(self, file_path: str, prompt: str, output_file_path: str = None) -> str:
        """
        Submits a file and prompt to Jules via the REST API, polls for completion,
        and saves the reordered text to output_file_path (or file_path if omitted).
        """
        if not os.path.exists(file_path):
            raise FileNotFoundError(f"File not found: {file_path}")

        with open(file_path, "r", encoding="utf-8") as f:
            file_content = f.read()

        target_path = output_file_path if output_file_path else file_path
        filename = os.path.basename(file_path)
        out_filename = os.path.basename(target_path)

        if not self.api_key:
            logger.error("Cannot call Google Jules API: No API key found. Specify 'jules_api_key' in credentials.json or export JULES_API_KEY.")
            if output_file_path and output_file_path != file_path:
                with open(target_path, "w", encoding="utf-8") as f:
                    f.write(file_content)
            return target_path

        full_prompt = (
            f"{prompt}\n\n"
            f"Input File: {filename}\n"
            f"Output File: {out_filename}\n\n"
            f"Raw Lines To Reorder:\n{file_content}"
        )

        try:
            source_name, starting_branch = self.resolve_source()
            session = self.create_session(
                prompt=full_prompt,
                title=f"Reorder {filename}",
                source_name=source_name,
                starting_branch=starting_branch
            )
            session_id = session.get("name") or session.get("id")

            if session_id:
                final_session = self.poll_session(session_id)
                if final_session.get("state") == "COMPLETED":
                    activities = self.get_activities(session_id)
                    reordered = self.extract_reordered_content_from_activities(activities, out_filename)
                    if not reordered:
                        reordered = self.extract_reordered_content_from_activities(activities, filename)

                    if not reordered and os.path.exists(target_path) and os.path.getmtime(target_path) > os.path.getmtime(file_path):
                        with open(target_path, "r", encoding="utf-8") as f:
                            reordered = f.read()

                    if reordered:
                        with open(target_path, "w", encoding="utf-8") as f:
                            f.write(reordered)
                        logger.info(f"Successfully reordered file via Jules API saved to {target_path}")
                        return target_path

        except Exception as e:
            logger.error(f"Error executing Jules API session: {e}")

        logger.warning(f"Jules API processing incomplete or produced no diffs. Preserving content at {target_path}.")
        if output_file_path and output_file_path != file_path and not os.path.exists(target_path):
            with open(target_path, "w", encoding="utf-8") as f:
                f.write(file_content)
        return target_path


def submit_to_jules(file_path: str, prompt: str, output_file_path: str = None, api_url=None, api_key=None) -> str:
    """
    Convenience function to submit a file and prompt to Jules via the API.
    """
    client = JulesAPI(api_url=api_url, api_key=api_key)
    return client.submit_file_and_prompt(file_path, prompt, output_file_path=output_file_path)
