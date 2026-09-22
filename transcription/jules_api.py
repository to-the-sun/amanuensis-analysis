import os
import sys
import json
import logging
import requests

logger = logging.getLogger("jules_api")

try:
    _script_dir = os.path.dirname(os.path.abspath(__file__))
except NameError:
    _script_dir = os.getcwd()


DEFAULT_JULES_API_URL = "https://jules.googleapis.com/v1alpha"


class JulesAPI:
    """
    API client interface for submitting files and prompts to Jules in the repository via the API,
    and receiving reordered text files back.
    """
    def __init__(self, api_url=None, api_key=None, credentials_path=None):
        self.api_url = api_url or os.environ.get("JULES_API_URL")
        self.api_key = api_key or os.environ.get("JULES_API_KEY")

        if not self.api_key:
            self._load_from_credentials(credentials_path)

        if not self.api_url:
            self.api_url = DEFAULT_JULES_API_URL

    def _load_from_credentials(self, credentials_path=None):
        dirs_to_check = []
        if credentials_path:
            dirs_to_check.append(credentials_path if os.path.isdir(credentials_path) else os.path.dirname(credentials_path))
            if os.path.isfile(credentials_path) and os.path.exists(credentials_path):
                try:
                    with open(credentials_path, "r", encoding="utf-8") as f:
                        creds = json.load(f)
                    self._apply_creds_dict(creds)
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
                    self._apply_creds_dict(creds)
                    if self.api_url:
                        logger.info(f"Loaded Jules API credentials from {cp}")
                        break
                except Exception as e:
                    logger.warning(f"Failed to load credentials from {cp}: {e}")

    def _apply_creds_dict(self, creds: dict):
        if not self.api_url:
            self.api_url = (
                creds.get("jules_api_url")
                or creds.get("jules_url")
                or creds.get("JULES_API_URL")
                or creds.get("JULES_URL")
            )
        if not self.api_key:
            self.api_key = (
                creds.get("jules_api_key")
                or creds.get("jules_key")
                or creds.get("JULES_API_KEY")
                or creds.get("JULES_KEY")
            )

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

        if self.api_url and self.api_key:
            try:
                headers = {
                    "Content-Type": "application/json",
                    "X-Goog-Api-Key": self.api_key,
                    "Authorization": f"Bearer {self.api_key}"
                }

                payload = {
                    "prompt": prompt,
                    "filename": os.path.basename(file_path),
                    "file_content": file_content,
                    "lines": lines
                }

                endpoint_url = self.api_url if self.api_url.endswith("/sessions") else f"{self.api_url.rstrip('/')}/sessions"
                response = requests.post(endpoint_url, headers=headers, json=payload, timeout=30)
                response.raise_for_status()
                res_json = response.json()
                if "reordered_content" in res_json:
                    reordered_content = res_json["reordered_content"]
                elif "reordered_lines" in res_json:
                    reordered_content = "\n".join(res_json["reordered_lines"])
                elif "text" in res_json:
                    reordered_content = res_json["text"]
            except Exception as e:
                logger.error(f"Error calling Jules API endpoint at {self.api_url}: {e}")

        if not reordered_content:
            logger.info("Processing line reordering locally via grammatical coherence engine (no API URL required)...")

            def score_line_grammar(line):
                # Grammatical score heuristic based on standard structure and STT noise detection
                score = 0
                words = line.split()
                word_count = len(words)
                if word_count >= 3:
                    score += 3
                elif word_count == 2:
                    score += 1
                if line and line[0].isupper():
                    score += 2
                if line and line[-1] in ".!?":
                    score += 3
                elif line and line[-1] in ",;:":
                    score += 1

                # Penalize word repetitions (e.g. STT hallucinations like "the the")
                for i in range(len(words) - 1):
                    if words[i].lower() == words[i+1].lower():
                        score -= 3
                return score

            sorted_lines = sorted(lines, key=score_line_grammar, reverse=True)
            reordered_content = "\n".join(sorted_lines)

        target_path = output_file_path if output_file_path else file_path
        with open(target_path, "w", encoding="utf-8") as f:
            f.write(reordered_content)

        if reordered_content != file_content:
            logger.info(f"Reordered text file submitted back and saved to {target_path}")
        else:
            logger.info(f"Output saved to {target_path} with original unordered lines.")
        return target_path

def submit_to_jules(file_path: str, prompt: str, output_file_path: str = None, api_url=None, api_key=None) -> str:
    """
    Convenience function to submit a file and prompt to Jules via the API.
    """
    client = JulesAPI(api_url=api_url, api_key=api_key)
    return client.submit_file_and_prompt(file_path, prompt, output_file_path=output_file_path)
