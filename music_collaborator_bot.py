import os
import json
import logging
import asyncio
import re
import urllib.parse
import xml.etree.ElementTree as ET
import aiohttp
import discord
from bs4 import BeautifulSoup

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s'
)
logger = logging.getLogger('MusicCollaboratorBot')

CREDENTIALS_FILE = 'credentials.json'
SEEN_LINKS_FILE = 'seen_links.json'
DEFAULT_CHANNEL_NAME = 'general'

def load_config():
    """
    Loads credentials and configuration from credentials.json.
    Supported keys in credentials.json:
      - token: Discord Bot Token (required for running bot)
      - interests: List of interest strings/genres (e.g., ["ambient", "synthwave", "electronic music", "sound design"])
      - search_queries: List of custom search query strings
      - check_interval: Background loop interval in seconds (default: 300)
      - channel_name: Name of text channel to post to (default: "general")
    """
    if not os.path.exists(CREDENTIALS_FILE):
        logger.warning(f"Credentials file '{CREDENTIALS_FILE}' not found. Defaulting empty configuration.")
        return {
            'token': '',
            'interests': ['music collaboration', 'music producer', 'indie musician', 'audio production'],
            'search_queries': [],
            'check_interval': 300,
            'channel_name': DEFAULT_CHANNEL_NAME
        }

    with open(CREDENTIALS_FILE, 'r', encoding='utf-8') as f:
        data = json.load(f)

    config = {
        'token': data.get('token', ''),
        'interests': data.get('interests', ['music collaboration', 'music producer', 'indie musician', 'synthwave', 'electronic music']),
        'search_queries': data.get('search_queries', []),
        'check_interval': data.get('check_interval', 300),
        'channel_name': data.get('channel_name', DEFAULT_CHANNEL_NAME)
    }
    return config

def load_seen_links() -> set:
    """
    Loads set of already seen / posted URLs from seen_links.json.
    """
    if not os.path.exists(SEEN_LINKS_FILE):
        return set()
    try:
        with open(SEEN_LINKS_FILE, 'r', encoding='utf-8') as f:
            data = json.load(f)
            return set(data)
    except Exception as e:
        logger.error(f"Error loading seen links from {SEEN_LINKS_FILE}: {e}")
        return set()

def save_seen_links(seen_links: set):
    """
    Saves updated set of seen URLs to seen_links.json.
    """
    try:
        with open(SEEN_LINKS_FILE, 'w', encoding='utf-8') as f:
            json.dump(list(seen_links), f, indent=2)
    except Exception as e:
        logger.error(f"Error saving seen links to {SEEN_LINKS_FILE}: {e}")

def calculate_relevance_score(title: str, snippet: str, interests: list) -> float:
    """
    Calculates relevance score for a given title and snippet based on interests and music collaboration keywords.
    """
    text = f"{title} {snippet}".lower()
    score = 0.0

    # High relevance keywords
    collab_keywords = ['collab', 'collaboration', 'collaborator', 'looking for', 'feature', 'vocalist', 'producer', 'remix', 'project', 'jam']
    for kw in collab_keywords:
        if kw in text:
            score += 1.5

    # Check interests
    for interest in interests:
        if interest.lower() in text:
            score += 2.0

    return score

async def fetch_url(session: aiohttp.ClientSession, url: str, headers: dict = None) -> str:
    """
    Helper function to asynchronously fetch content from a URL with timeout and fallback user-agent.
    """
    default_headers = {
        'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/115.0.0.0 Safari/537.36'
    }
    if headers:
        default_headers.update(headers)

    try:
        async with session.get(url, headers=default_headers, timeout=15) as resp:
            if resp.status == 200:
                return await resp.text()
            else:
                logger.debug(f"Fetch {url} failed with status {resp.status}")
                return ""
    except Exception as e:
        logger.debug(f"Error fetching {url}: {e}")
        return ""

def parse_rss_feed(feed_content: str) -> list:
    """
    Parses RSS / Atom XML feeds and extracts items.
    """
    items = []
    if not feed_content:
        return items

    try:
        root = ET.fromstring(feed_content)
        # Check RSS 2.0
        channel = root.find('channel')
        if channel is not None:
            for item in channel.findall('item'):
                title = item.findtext('title', default='').strip()
                link = item.findtext('link', default='').strip()
                description = item.findtext('description', default='').strip()
                if link:
                    items.append({
                        'title': title,
                        'link': link,
                        'snippet': BeautifulSoup(description, 'html.parser').get_text()[:300]
                    })
            return items

        # Check Atom feed
        ns = {'atom': 'http://www.w3.org/2005/Atom'}
        for entry in root.findall('atom:entry', ns) or root.findall('entry'):
            title = entry.findtext('atom:title', ns) or entry.findtext('title', default='').strip()
            link_elem = entry.find('atom:link', ns) or entry.find('link')
            link = link_elem.get('href', '') if link_elem is not None else ''
            summary = entry.findtext('atom:summary', ns) or entry.findtext('summary', default='') or entry.findtext('atom:content', ns) or entry.findtext('content', default='')
            if link:
                items.append({
                    'title': title,
                    'link': link,
                    'snippet': BeautifulSoup(summary, 'html.parser').get_text()[:300]
                })
    except Exception as e:
        logger.debug(f"Error parsing RSS feed: {e}")

    return items

async def search_and_browse(session: aiohttp.ClientSession, interests: list, custom_queries: list = None) -> list:
    """
    Surfs the web and searches RSS feeds / public sources to discover potential music collaborators.
    Returns a list of opportunity dicts: [{'title': ..., 'link': ..., 'snippet': ..., 'score': ...}, ...]
    """
    results = []
    queries = list(custom_queries) if custom_queries else []

    # Generate queries based on interests if none supplied
    if not queries:
        for interest in interests:
            queries.append(f"{interest} music collaboration looking for producer vocalist")
            queries.append(f"{interest} musician looking for collaborators")

    # Standard RSS / Open search feed endpoints for music communities
    feed_urls = [
        "https://www.reddit.com/r/MusicInTheMaking/new/.rss",
        "https://www.reddit.com/r/NeedMakers/new/.rss",
        "https://www.reddit.com/r/WeAreTheMusicMakers/new/.rss",
    ]

    # 1. Fetch RSS Feeds
    for feed_url in feed_urls:
        content = await fetch_url(session, feed_url)
        parsed_items = parse_rss_feed(content)
        for item in parsed_items:
            score = calculate_relevance_score(item['title'], item['snippet'], interests)
            if score > 0:
                item['score'] = score
                results.append(item)

    # 2. Dynamic Web Query Search using public HTML scraping (e.g. DuckDuckGo HTML / Web feeds)
    for q in queries[:5]:
        search_url = f"https://html.duckduckgo.com/html/?q={urllib.parse.quote(q)}"
        html_content = await fetch_url(session, search_url)
        if html_content:
            soup = BeautifulSoup(html_content, 'html.parser')
            for result in soup.select('.result'):
                title_elem = result.select_one('.result__title')
                snippet_elem = result.select_one('.result__snippet')
                url_elem = result.select_one('.result__url')

                if title_elem and url_elem:
                    title = title_elem.get_text().strip()
                    snippet = snippet_elem.get_text().strip() if snippet_elem else ""

                    # Extract raw URL from DDG redirect if necessary
                    raw_href = url_elem.get('href', '') or title_elem.find('a', href=True).get('href', '') if title_elem.find('a', href=True) else ''
                    if 'uddg=' in raw_href:
                        parsed_qs = urllib.parse.parse_qs(urllib.parse.urlparse(raw_href).query)
                        link = parsed_qs.get('uddg', [raw_href])[0]
                    else:
                        link = raw_href

                    if link and link.startswith('http'):
                        score = calculate_relevance_score(title, snippet, interests)
                        if score > 0:
                            results.append({
                                'title': title,
                                'link': link,
                                'snippet': snippet,
                                'score': score
                            })

    # Sort results by score descending
    results.sort(key=lambda x: x['score'], reverse=True)
    return results

def format_opportunity_message(opp: dict) -> str:
    """
    Formats an opportunity result into a clean Discord markdown message.
    """
    title = opp.get('title', 'Musical Opportunity / Potential Collaborator')
    link = opp.get('link', '')
    snippet = opp.get('snippet', '')
    if len(snippet) > 250:
        snippet = snippet[:247] + "..."

    msg = f"🎵 **Potential Collaborator / Opportunity Discovered!**\n"
    msg += f"**{title}**\n"
    if snippet:
        msg += f"> {snippet}\n"
    msg += f"🔗 {link}"
    return msg

class MusicCollaboratorClient(discord.Client):
    def __init__(self, config: dict, *args, **kwargs):
        intents = discord.Intents.default()
        intents.guilds = True
        intents.messages = True
        super().__init__(intents=intents, *args, **kwargs)
        self.config = config
        self.seen_links = load_seen_links()

    async def setup_hook(self):
        self.loop.create_task(self.background_browsing_loop())

    async def on_ready(self):
        logger.info(f"Logged in as {self.user} (ID: {self.user.id})")

    async def background_browsing_loop(self):
        await self.wait_until_ready()
        target_channel_name = self.config.get('channel_name', DEFAULT_CHANNEL_NAME)
        check_interval = self.config.get('check_interval', 300)

        async with aiohttp.ClientSession() as session:
            while not self.is_closed():
                try:
                    logger.info("Browsing the internet for potential collaborators and musical opportunities...")
                    opportunities = await search_and_browse(
                        session,
                        self.config.get('interests', []),
                        self.config.get('search_queries', [])
                    )

                    new_opps = [o for o in opportunities if o['link'] not in self.seen_links]
                    logger.info(f"Discovered {len(opportunities)} total items, {len(new_opps)} new.")

                    # Target channels across joined guilds
                    target_channels = []
                    for guild in self.guilds:
                        # Find channel by name or fallback to first text channel
                        ch = discord.utils.get(guild.text_channels, name=target_channel_name)
                        if not ch and guild.text_channels:
                            ch = guild.text_channels[0]
                        if ch:
                            target_channels.append(ch)

                    # Post up to 3 new top opportunities per loop iteration
                    for opp in new_opps[:3]:
                        msg_text = format_opportunity_message(opp)
                        for ch in target_channels:
                            try:
                                await ch.send(msg_text)
                                await asyncio.sleep(1)
                            except Exception as e:
                                logger.error(f"Error posting message to channel {ch.name}: {e}")

                        self.seen_links.add(opp['link'])

                    save_seen_links(self.seen_links)

                except Exception as e:
                    logger.error(f"Error in background browsing loop: {e}")

                await asyncio.sleep(check_interval)

def main():
    config = load_config()
    token = config.get('token')
    if not token:
        logger.error("No Discord bot token supplied in credentials.json. Please add 'token' to credentials.json.")
        return

    client = MusicCollaboratorClient(config=config)
    client.run(token)

if __name__ == '__main__':
    main()
