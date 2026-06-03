import sys
import os
# Add the current directory to sys.path so we can import llama_query
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import llama_query

def test():
    print("Testing llama_query API...")

    max_len = llama_query.get_max_context_length()
    print(f"Max context length: {max_len}")

    test_text = "Hello, world! This is a test."
    tokens = llama_query.count_tokens(test_text)
    print(f"Tokens in '{test_text}': {tokens}")

    query = "What is the most poetic phrase?"
    query_tokens = llama_query.count_query_tokens(query)
    print(f"Total tokens for query '{query}': {query_tokens}")

    print("API Test complete.")

if __name__ == "__main__":
    test()
