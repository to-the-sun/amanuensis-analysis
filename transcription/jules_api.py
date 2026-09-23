import os
import sys
import json
import logging
import subprocess
import requests

logger = logging.getLogger("jules_api")

try:
    _script_dir = os.path.dirname(os.path.abspath(__file__))
except NameError:
    _script_dir = os.getcwd()


DEFAULT_JULES_API_URL = "https://jules.googleapis.com/v1alpha"
DEFAULT_JULES_PROJECT_ID = "714089051017"


class JulesAPI:
    """
    API client interface for submitting files and prompts to Jules in the repository via the API,
    and receiving reordered text files back.
    """
    def __init__(self, api_url=None, api_key=None, project_id=None, credentials_path=None):
        self.api_url = api_url or os.environ.get("JULES_API_URL")
        self.api_key = api_key or os.environ.get("JULES_API_TOKEN") or os.environ.get("JULES_TOKEN") or os.environ.get("JULES_API_KEY")
        self.project_id = project_id or os.environ.get("JULES_PROJECT_ID")
        self.key_source = "constructor" if api_key else ("environment variable" if (os.environ.get("JULES_API_TOKEN") or os.environ.get("JULES_TOKEN") or os.environ.get("JULES_API_KEY")) else None)

        if not self.api_key or not self.project_id:
            self._load_from_credentials(credentials_path)

        # Google Jules API requires a Google OAuth2 access token starting with "ya29.".
        # If the loaded credential is an API key (e.g. starting with "AQ." or "AIza"), attempt to fetch the OAuth2 token via gcloud.
        if not self.api_key or not self.api_key.startswith("ya29."):
            old_key = self.api_key
            self._try_gcloud_auth()
            if old_key and self.api_key and self.api_key != old_key:
                logger.info(f"Replaced non-OAuth2 API key ({self._mask_key(old_key)}) with active gcloud OAuth2 token ({self._mask_key(self.api_key)}).")

        if not self.api_url:
            self.api_url = DEFAULT_JULES_API_URL

        if not self.project_id:
            self.project_id = DEFAULT_JULES_PROJECT_ID

        self._log_credential_status()

    def _try_gcloud_auth(self):
        try:
            use_shell = sys.platform == "win32"
            res = subprocess.run(["gcloud", "auth", "print-access-token"], capture_output=True, text=True, timeout=5, shell=use_shell)
            if res.returncode == 0 and res.stdout.strip():
                self.api_key = res.stdout.strip()
                self.key_source = "gcloud CLI (gcloud auth print-access-token)"
                logger.info("Retrieved Google OAuth2 access token automatically via gcloud CLI.")
        except Exception:
            pass

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
            logger.warning("No Jules API key found in constructor, environment (JULES_API_KEY), or credentials.json (keys: jules_api_key, jules_key).")
        logger.info(f"Jules API Target URL: {self.api_url}")
        logger.info(f"Google Cloud Project ID: {self.project_id} (Jules-harness)")

    def _load_from_credentials(self, credentials_path=None):
        dirs_to_check = []
        if credentials_path:
            dirs_to_check.append(credentials_path if os.path.isdir(credentials_path) else os.path.dirname(credentials_path))
            if os.path.isfile(credentials_path) and os.path.exists(credentials_path):
                try:
                    with open(credentials_path, "r", encoding="utf-8") as f:
                        creds = json.load(f)
                    if self._apply_creds_dict(creds):
                        self.key_source = f"{credentials_path}"
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

    def _refresh_oauth2_token(self, refresh_token: str, client_id: str = None, client_secret: str = None) -> str:
        try:
            url = "https://oauth2.googleapis.com/token"
            data = {
                "grant_type": "refresh_token",
                "refresh_token": refresh_token,
            }
            if client_id:
                data["client_id"] = client_id
            if client_secret:
                data["client_secret"] = client_secret

            res = requests.post(url, data=data, timeout=10)
            if res.status_code == 200:
                token_data = res.json()
                access_token = token_data.get("access_token")
                if access_token:
                    logger.info("Successfully refreshed Google OAuth2 access token programmatically.")
                    return access_token
            else:
                logger.warning(f"Google OAuth2 token refresh HTTP {res.status_code}: {res.text}")
        except Exception as e:
            logger.warning(f"Failed to refresh Google OAuth2 access token: {e}")
        return None

    def _apply_creds_dict(self, creds: dict) -> bool:
        found = False
        if not self.api_url:
            self.api_url = (
                creds.get("jules_api_url")
                or creds.get("jules_url")
                or creds.get("JULES_API_URL")
                or creds.get("JULES_URL")
            )
        if not self.project_id:
            self.project_id = (
                creds.get("jules_project_id")
                or creds.get("jules_project")
                or creds.get("project_id")
                or creds.get("project")
                or creds.get("JULES_PROJECT_ID")
            )
        if not self.api_key:
            refresh_token = creds.get("jules_refresh_token") or creds.get("refresh_token")
            if refresh_token:
                client_id = creds.get("jules_client_id") or creds.get("client_id")
                client_secret = creds.get("jules_client_secret") or creds.get("client_secret")
                refreshed = self._refresh_oauth2_token(refresh_token, client_id, client_secret)
                if refreshed:
                    self.api_key = refreshed
                    found = True

        if not self.api_key:
            extracted_token = (
                creds.get("jules_api_token")
                or creds.get("jules_token")
                or creds.get("jules_oauth_token")
                or creds.get("JULES_API_TOKEN")
                or creds.get("JULES_TOKEN")
            )
            if extracted_token:
                self.api_key = extracted_token
                found = True

        if not self.api_key:
            extracted_key = (
                creds.get("jules_api_key")
                or creds.get("jules_key")
                or creds.get("JULES_API_KEY")
                or creds.get("JULES_KEY")
            )
            if extracted_key:
                self.api_key = extracted_key
                found = True
        return found

    def submit_file_and_prompt(self, file_path: str, prompt: str, output_file_path: str = None) -> str:
        """
        Submits a text file and prompt to Jules via the API.
        The reordered text file is saved/written to output_file_path (or file_path if not specified),
        leaving the original file intact when output_file_path is specified.
        """
        if not os.path.exists(file_path):
            raise FileNotFoundError(f"File not found: {file_path}")

        with open(file_path, "r", encoding="utf-8") as f:
            file_content = f.read()

        lines = [line.strip() for line in file_content.splitlines() if line.strip()]

        reordered_content = None

        endpoint_url = self.api_url if self.api_url.endswith("/sessions") else f"{self.api_url.rstrip('/')}/sessions"

        if not self.api_key:
            logger.error("Cannot call Google Jules API: No API key found. Please specify 'jules_api_key' in credentials.json or set JULES_API_KEY environment variable.")
            reordered_content = file_content
        else:
            try:
                headers = {
                    "Content-Type": "application/json",
                    "Authorization": f"Bearer {self.api_key}",
                    "X-Goog-User-Project": self.project_id
                }

                payload = {
                    "prompt": prompt,
                    "filename": os.path.basename(file_path),
                    "file_content": file_content,
                    "lines": lines
                }

                logger.info(f"Submitting line reordering request to Google Jules API at {endpoint_url} using OAuth2 Bearer token ({self._mask_key(self.api_key)})...")
                response = requests.post(endpoint_url, headers=headers, json=payload, timeout=30)

                if response.status_code == 401:
                    logger.error(
                        f"Jules API Authentication Failed (HTTP 401 Unauthorized).\n"
                        f"Endpoint: {endpoint_url}\n"
                        f"Token Used: {self._mask_key(self.api_key)} (Source: {self.key_source or 'credentials.json'})\n"
                        f"Reason: Google Jules API requires a valid OAuth2 Access Token in Authorization: Bearer <token>. API keys are blocked (API_KEY_SERVICE_BLOCKED).\n"
                        f"To generate a valid OAuth2 token, set 'jules_api_token' in credentials.json, run 'gcloud auth print-access-token', or export JULES_API_TOKEN.\n"
                        f"API Response Details: {response.text}"
                    )
                    reordered_content = file_content
                else:
                    response.raise_for_status()
                    res_json = response.json()
                    if "reordered_content" in res_json:
                        reordered_content = res_json["reordered_content"]
                    elif "reordered_lines" in res_json:
                        reordered_content = "\n".join(res_json["reordered_lines"])
                    elif "text" in res_json:
                        reordered_content = res_json["text"]
                    else:
                        logger.warning(f"Jules API responded HTTP 200 but did not return reordered_content/reordered_lines. Response: {res_json}")
                        reordered_content = file_content
            except requests.exceptions.RequestException as e:
                resp_detail = getattr(e.response, "text", "") if hasattr(e, "response") and e.response is not None else ""
                logger.error(f"Error calling Google Jules API endpoint at {endpoint_url}: {e}. Response details: {resp_detail}")
                reordered_content = file_content
            except Exception as e:
                logger.error(f"Unexpected error calling Google Jules API: {e}")
                reordered_content = file_content

        if not reordered_content:
            reordered_content = file_content

        target_path = output_file_path if output_file_path else file_path
        with open(target_path, "w", encoding="utf-8") as f:
            f.write(reordered_content)

        if reordered_content != file_content:
            logger.info(f"Reordered text file submitted back from Jules API and saved to {target_path}")
        else:
            logger.info(f"Output saved to {target_path} with original unordered lines (Jules API reordering was not applied).")
        return target_path

def submit_to_jules(file_path: str, prompt: str, output_file_path: str = None, api_url=None, api_key=None) -> str:
    """
    Convenience function to submit a file and prompt to Jules via the API.
    """
    client = JulesAPI(api_url=api_url, api_key=api_key)
    return client.submit_file_and_prompt(file_path, prompt, output_file_path=output_file_path)
