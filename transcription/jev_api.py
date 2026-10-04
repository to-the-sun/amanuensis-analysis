import os
import re
import json
import logging
import requests
from concurrent.futures import ThreadPoolExecutor, as_completed

logger = logging.getLogger("jev_api")

DEFAULT_TYPESAFE_API_URL = "https://api.typesafe.ai/v1/systemone"
DEFAULT_JEV_MODEL = "jev-latest"

def load_credentials(search_dir=None):
    dirs_to_check = []
    if search_dir:
        dirs_to_check.append(search_dir)
    script_dir = os.path.dirname(os.path.abspath(__file__))
    dirs_to_check.extend([script_dir, os.getcwd(), os.path.dirname(script_dir)])

    for d in dirs_to_check:
        cp = os.path.join(d, "credentials.json")
        if os.path.exists(cp):
            try:
                with open(cp, "r", encoding="utf-8") as f:
                    return json.load(f)
            except Exception as e:
                logger.warning(f"Failed to read credentials file at {cp}: {e}")
    return {}

def get_jev_api_key(explicit_key: str = None) -> str:
    if explicit_key:
        return explicit_key

    for env_var in ["TYPESAFE_API_KEY", "JEV_API_KEY", "TYPESAFE_KEY", "JEV_KEY"]:
        val = os.environ.get(env_var)
        if val:
            return val

    creds = load_credentials()
    for key_name in ["typesafe_api_key", "jev_api_key", "typesafe_key", "jev_key", "TYPESAFE_API_KEY", "JEV_API_KEY"]:
        val = creds.get(key_name)
        if val:
            return val

    return ""

def evaluate_phrase_grammar(phrase: str, api_key: str = None, api_url: str = None, model: str = None) -> float:
    """
    Queries the TypeSafe Jev API to evaluate if a phrase/couplet is a complete phrase with proper grammar.
    Returns the 'noul' probability score (float between 0.0 and 1.0).
    """
    key = get_jev_api_key(api_key)
    if not key:
        logger.error("TypeSafe Jev API key not found in parameters, environment, or credentials.json.")
        return 0.0

    url = api_url or DEFAULT_TYPESAFE_API_URL
    model_name = model or DEFAULT_JEV_MODEL

    headers = {
        "Authorization": f"Bearer {key}",
        "Content-Type": "application/json"
    }

    payload = {
        "state": phrase,
        "model": model_name,
        "questions": {
            "is_complete_grammar": {
                "type": "noul",
                "instructions": "Is this a complete phrase with proper grammar?"
            }
        }
    }

    try:
        response = requests.post(url, headers=headers, json=payload, timeout=30)
        response.raise_for_status()
        res_data = response.json()

        answers = res_data.get("answers", {})
        grammar_answer = answers.get("is_complete_grammar", {})
        noul_score = grammar_answer.get("noul", 0.0)
        return float(noul_score)
    except Exception as e:
        logger.error(f"Error calling TypeSafe Jev API for phrase '{phrase}': {e}")
        return 0.0

def rank_phrases_via_jev(phrases: list[str], api_key: str = None, max_workers: int = 5) -> list[tuple[str, float]]:
    """
    Evaluates a list of phrases/couplets using Jev API in parallel and returns them sorted by
    their Noul likelihood percentage in descending order.
    """
    if not phrases:
        return []

    results = []
    with ThreadPoolExecutor(max_workers=min(max_workers, len(phrases))) as executor:
        future_to_phrase = {
            executor.submit(evaluate_phrase_grammar, phrase, api_key): (idx, phrase)
            for idx, phrase in enumerate(phrases)
        }
        for future in as_completed(future_to_phrase):
            idx, phrase = future_to_phrase[future]
            try:
                score = future.result()
            except Exception as e:
                logger.error(f"Failed evaluation for line {idx}: {e}")
                score = 0.0
            results.append((idx, phrase, score))

    # Sort primarily by score descending, preserving relative original index for ties
    results.sort(key=lambda x: (-x[2], x[0]))
    return [(phrase, score) for idx, phrase, score in results]

def reorder_poem_lines_via_jev(file_path: str, prompt: str = None, output_file_path: str = None, api_key: str = None) -> str:
    """
    Reads couplets/triplets/blocks from file_path (separated by double newlines or single lines if no double newlines exist),
    queries Jev API with 'Is this a complete phrase with proper grammar?' getting a NOUL response for each block,
    and ranks all likelihood values to reorder the couplets/triplets into output_file_path.
    """
    if not os.path.exists(file_path):
        raise FileNotFoundError(f"Input poem lines file not found: {file_path}")

    with open(file_path, "r", encoding="utf-8") as f:
        content = f.read()

    # Split by double newlines to treat couplets/triplets as intact blocks
    raw_blocks = re.split(r"\n\s*\n", content)
    blocks = [b.strip() for b in raw_blocks if b.strip()]

    # Fallback to single lines if no double-newline blocks were found
    if not blocks:
        lines = [line.strip() for line in content.splitlines() if line.strip()]
        blocks = lines

    if not blocks:
        logger.warning(f"No content found in {file_path}")
        out_path = output_file_path or file_path
        with open(out_path, "w", encoding="utf-8") as f:
            f.write("")
        return out_path

    logger.info(f"Evaluating {len(blocks)} couplet/triplet blocks via TypeSafe Jev API...")
    ranked = rank_phrases_via_jev(blocks, api_key=api_key)

    for phrase, score in ranked:
        formatted_phrase = phrase.replace('\n', ' / ')
        logger.info(f"Score {score:.2f} ({score * 100:.1f}%): '{formatted_phrase}'")

    ordered_blocks = [phrase for phrase, score in ranked]

    target_path = output_file_path or file_path
    with open(target_path, "w", encoding="utf-8") as f:
        f.write("\n\n".join(ordered_blocks) + "\n")

    logger.info(f"Successfully reordered {len(ordered_blocks)} couplet/triplet blocks via Jev API and saved to {target_path}")
    return target_path
