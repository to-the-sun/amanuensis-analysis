import os
import sys
import json
import logging
import requests

logger = logging.getLogger("jules_api")

class JulesAPI:
    """
    API client interface for submitting files and prompts to Jules in the repository via the API,
    and receiving reordered text files back.
    """
    def __init__(self, api_url=None, api_key=None):
        self.api_url = api_url or os.environ.get("JULES_API_URL")
        self.api_key = api_key or os.environ.get("JULES_API_KEY")

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

        if self.api_url:
            try:
                headers = {"Content-Type": "application/json"}
                if self.api_key:
                    headers["Authorization"] = f"Bearer {self.api_key}"

                payload = {
                    "prompt": prompt,
                    "filename": os.path.basename(file_path),
                    "file_content": file_content,
                    "lines": lines
                }

                response = requests.post(self.api_url, headers=headers, json=payload, timeout=30)
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
            # Fallback reordering when no external API endpoint is connected
            logger.info("Processing line reordering locally via Jules API fallback...")

            def score_line_grammar(line):
                # Grammatical score heuristic based on standard structure (capitalization, punctuation, word count)
                score = 0
                words = line.split()
                if len(words) >= 3:
                    score += 2
                if line[0].isupper():
                    score += 1
                if line[-1] in ".!?":
                    score += 2
                return score

            sorted_lines = sorted(lines, key=score_line_grammar, reverse=True)
            reordered_content = "\n".join(sorted_lines)

        target_path = output_file_path if output_file_path else file_path
        with open(target_path, "w", encoding="utf-8") as f:
            f.write(reordered_content)

        logger.info(f"Reordered text file submitted back and saved to {target_path}")
        return target_path

def submit_to_jules(file_path: str, prompt: str, output_file_path: str = None, api_url=None, api_key=None) -> str:
    """
    Convenience function to submit a file and prompt to Jules via the API.
    """
    client = JulesAPI(api_url=api_url, api_key=api_key)
    return client.submit_file_and_prompt(file_path, prompt, output_file_path=output_file_path)
