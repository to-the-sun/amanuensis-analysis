# Music Collaborator Discord Bot Report

## Overview

The `music_collaborator_bot.py` module is an autonomous background Discord bot designed to continuously browse the internet for potential music collaborators, open project calls, feature requests, and shared musical communities. Discovered links and descriptions are evaluated for relevance, deduplicated, and automatically posted to a designated Discord text channel (defaulting to `#general`).

---

## Architecture & Workflows

```
┌────────────────────────┐
│    credentials.json    │
└───────────┬────────────┘
            │ loads configuration & token
            ▼
┌────────────────────────┐
│ music_collaborator_bot │
└───────────┬────────────┘
            │
            ├──────► 1. Web Crawling & RSS Discovery
            │           - RSS Feeds (Music subreddits & feeds)
            │           - Web Queries (DuckDuckGo HTML)
            │
            ├──────► 2. Relevance Scoring Engine
            │           - Keyword matching (collab, vocalist, producer, etc.)
            │           - Custom interest matching
            │
            ├──────► 3. Link Deduplication
            │           - Persists visited URLs to seen_links.json
            │
            └──────► 4. Discord Bot Posting Loop
                        - Posts top matches to #general channel
                        - Error-handling wrapper keeps terminal window open
```

---

## Technical Specifications

### 1. Configuration Management (`credentials.json`)

The bot reads credentials and runtime parameters from `credentials.json` located in the same directory:

```json
{
  "token": "YOUR_DISCORD_BOT_TOKEN",
  "interests": [
    "music collaboration",
    "music producer",
    "synthwave",
    "electronic music",
    "vocalist"
  ],
  "search_queries": [
    "looking for producer synthwave collaboration",
    "musician feature request"
  ],
  "check_interval": 300,
  "channel_name": "general"
}
```

* **`token`**: Discord bot token.
* **`interests`**: List of musical interests or genres used for query generation and relevance scoring.
* **`search_queries`**: Custom web search query overrides.
* **`check_interval`**: Frequency in seconds between background browsing iterations (default: 300s).
* **`channel_name`**: Target text channel name on connected Discord servers (default: `"general"`).

---

### 2. Web Crawling & Search Mechanics

The bot combines two primary web discovery strategies:

1. **RSS Feed Parsing (`parse_rss_feed`)**:
   - Asynchronously queries public music collaboration RSS feeds (such as `/r/MusicInTheMaking`, `/r/NeedMakers`, `/r/WeAreTheMusicMakers`).
   - Parses XML RSS 2.0 and Atom feeds to extract post titles, links, and text descriptions.

2. **Dynamic Web Query Scraping (`search_and_browse`)**:
   - Constructs web search queries based on configured `interests` and `search_queries`.
   - Uses `aiohttp` and `BeautifulSoup` to scrape DuckDuckGo HTML search results, extracting titles, snippets, and target URLs.

---

### 3. Relevance Scoring (`calculate_relevance_score`)

Each discovered opportunity is scored based on text matching against high-relevance collaboration keywords and user-defined interests:

* **Collaboration Keywords (+1.5 points each)**: `collab`, `collaboration`, `collaborator`, `looking for`, `feature`, `vocalist`, `producer`, `remix`, `project`, `jam`.
* **Configured Interests (+2.0 points each)**: Matches against strings in `interests`.

Items with a score $> 0$ are ranked in descending order of relevance.

---

### 4. Persistence & Deduplication (`seen_links.json`)

To prevent spamming or posting duplicate links:
* Previously posted links are loaded from `seen_links.json` on startup.
* New opportunities are checked against `seen_links.json`.
* Once posted to Discord, new links are added to the in-memory set and persisted back to `seen_links.json`.

---

### 5. Discord Integration & Terminal Persistence

* **`MusicCollaboratorClient`**: Inherits from `discord.Client` and initializes a non-blocking background task in `setup_hook`.
* **Channel Fallback**: Locates the channel matching `channel_name` across all joined guilds, falling back to the first available text channel if necessary.
* **Console Window Persistence**: Execution in `if __name__ == '__main__':` is wrapped in a `try...except...finally` block that prints stack traces on error and calls `input("Press Enter to exit...")`, ensuring console windows remain open when double-clicked or executed directly.

---

## Dependencies & Installation

Install required Python dependencies:

```bash
pip install aiohttp beautifulsoup4 discord.py
```

---

## Running the Bot

Run the bot directly from the terminal or by double-clicking:

```bash
python3 music_collaborator_bot.py
```
