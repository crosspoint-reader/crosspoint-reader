"""
Xteink Telegram Bot
Adapted from Palpi (pala-pi) for standalone deployment on Railway/any VPS.

Commands:
  /start            — Welcome
  /search <query>   — Search Project Gutenberg
  /popular          — Top Gutenberg books
  /download <id>    — Download Gutenberg book by ID
  /todo             — Show your todo list
  /add <text>       — Add a todo item
  /done <number>    — Toggle item done/undone
  /clear            — Remove all completed items
  /list             — List all books in library
  /help             — Command list

Send any .epub or .txt file to add it to your library.
Send any plain text to search Gutenberg.

File transfer to X4:
  Books are saved to BOOKS_DIR (default: ./books/).
  When X4 is in Wi-Fi hotspot mode, copy files manually,
  or point CrossPoint's web UI to this folder if network-accessible.
"""

import os
import re
import json
import logging
import threading
import time

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s"
)
log = logging.getLogger("xteink.bot")

# ─── Config ──────────────────────────────────────────────────────────────────

BOT_TOKEN       = os.environ.get("TELEGRAM_BOT_TOKEN", "")
ALLOWED_CHAT_ID = os.environ.get("TELEGRAM_CHAT_ID", "")   # leave blank = anyone
BOOKS_DIR       = os.environ.get("BOOKS_DIR", "./books")
DATA_DIR        = os.environ.get("DATA_DIR", "./data")

GUTENDEX_API            = "https://gutendex.com/books"
GUTENBERG_TXT_MIRROR    = "https://www.gutenberg.org/cache/epub/{id}/pg{id}.txt"

MAX_TODO_ITEMS  = 16
MAX_TODO_TEXT   = 64

# ─── Deps ─────────────────────────────────────────────────────────────────────

try:
    import requests
    HAS_REQUESTS = True
except ImportError:
    HAS_REQUESTS = False
    log.warning("requests not installed — pip install requests")

try:
    import telebot
    HAS_TELEBOT = True
except ImportError:
    HAS_TELEBOT = False
    log.error("pyTelegramBotAPI not installed — pip install pyTelegramBotAPI")

# ─── Helpers ──────────────────────────────────────────────────────────────────

def ensure_dirs():
    os.makedirs(BOOKS_DIR, exist_ok=True)
    os.makedirs(DATA_DIR, exist_ok=True)

def sanitize_filename(name, ext=".txt"):
    clean = re.sub(r'[^\w\s\-]', '', name).strip()[:60]
    if not clean:
        clean = "book"
    return clean + ext

def list_books():
    if not os.path.isdir(BOOKS_DIR):
        return []
    return sorted([
        f for f in os.listdir(BOOKS_DIR)
        if f.lower().endswith((".txt", ".epub"))
    ])

# ─── Text normalization (from Palpi) ─────────────────────────────────────────

def normalize(text):
    if text.startswith("﻿"):
        text = text[1:]
    replacements = {
        " ": " ", "‘": "'", "’": "'", "‚": "'",
        "“": '"', "”": '"', "„": '"',
        "«": '"', "»": '"',
        "–": "-", "—": "-", "…": "...",
    }
    for old, new in replacements.items():
        text = text.replace(old, new)
    return text

def compact(text):
    lines = text.split("\n")
    result = []
    blank_count = 0
    for line in lines:
        line = line.rstrip().replace("\t", " ")
        while "  " in line:
            line = line.replace("  ", " ")
        if not line:
            blank_count += 1
            if blank_count <= 2:
                result.append("")
        else:
            blank_count = 0
            result.append(line)
    while result and not result[-1]:
        result.pop()
    return "\n".join(result)

def strip_gutenberg_boilerplate(text):
    lines = text.split("\n")
    start, end = 0, len(lines)
    for i, line in enumerate(lines):
        u = line.upper()
        if "*** START OF" in u and "PROJECT GUTENBERG" in u:
            start = i + 1
            break
    for i in range(len(lines) - 1, start, -1):
        u = lines[i].upper()
        if "*** END OF" in u and "PROJECT GUTENBERG" in u:
            end = i
            break
    return "\n".join(lines[start:end])

# ─── Gutenberg ────────────────────────────────────────────────────────────────

def search_gutenberg(query, page=1):
    if not HAS_REQUESTS:
        return [], 0
    try:
        resp = requests.get(GUTENDEX_API, params={
            "search": query, "languages": "en",
            "mime_type": "text/plain", "page": page,
        }, timeout=15)
        resp.raise_for_status()
        data = resp.json()
        results = []
        for book in data.get("results", [])[:8]:
            authors = ", ".join(a["name"] for a in book.get("authors", []))
            txt_url = None
            for mime, url in book.get("formats", {}).items():
                if "text/plain" in mime and "utf-8" in mime.lower():
                    txt_url = url
                    break
            if not txt_url:
                for mime, url in book.get("formats", {}).items():
                    if "text/plain" in mime:
                        txt_url = url
                        break
            results.append({
                "id": book["id"],
                "title": book.get("title", "Unknown"),
                "author": authors or "Unknown",
                "downloads": book.get("download_count", 0),
                "txt_url": txt_url,
            })
        return results, data.get("count", 0)
    except Exception as e:
        log.error("Gutenberg search error: %s", e)
        return [], 0

def download_gutenberg_book(book_id, txt_url=None):
    if not HAS_REQUESTS:
        return None, "requests not installed"
    try:
        meta = requests.get(f"{GUTENDEX_API}/{book_id}", timeout=15).json()
        title = meta.get("title", f"Book {book_id}")
    except Exception:
        title = f"Book {book_id}"
        meta = {}

    if not txt_url:
        for mime, url in meta.get("formats", {}).items():
            if "text/plain" in mime:
                txt_url = url
                break
    if not txt_url:
        txt_url = GUTENBERG_TXT_MIRROR.format(id=book_id)

    try:
        resp = requests.get(txt_url, timeout=30)
        resp.raise_for_status()
        text = resp.text
    except Exception as e:
        return None, f"Download failed: {e}"

    text = strip_gutenberg_boilerplate(text)
    text = normalize(text)
    text = compact(text)

    if len(text.strip()) < 100:
        return None, "Text too short — may be placeholder"

    return text, title

# ─── Todo List ────────────────────────────────────────────────────────────────

TODO_PATH = os.path.join(DATA_DIR, "todo.json")

def load_todo():
    if not os.path.exists(TODO_PATH):
        return []
    try:
        with open(TODO_PATH) as f:
            data = json.load(f)
        return data.get("items", [])
    except Exception:
        return []

def save_todo(items):
    os.makedirs(DATA_DIR, exist_ok=True)
    with open(TODO_PATH, "w") as f:
        json.dump({"items": items}, f)

def format_todo(items):
    if not items:
        return "Your list is empty. Use /add <text> to add items."
    lines = ["*Your List:*\n"]
    for i, item in enumerate(items, 1):
        tick = "✅" if item["done"] else "⬜"
        lines.append(f"{tick} {i}. {item['text']}")
    return "\n".join(lines)

# ─── Bot ──────────────────────────────────────────────────────────────────────

def is_authorized(message):
    if not ALLOWED_CHAT_ID:
        return True
    return str(message.chat.id) == str(ALLOWED_CHAT_ID)

def make_bot():
    bot = telebot.TeleBot(BOT_TOKEN, parse_mode="Markdown")

    # ── /start ────────────────────────────────────────────────────────────────
    @bot.message_handler(commands=["start"])
    def cmd_start(msg):
        if not is_authorized(msg):
            return
        bot.reply_to(msg,
            "Hey! I'm your Xteink bot 📚\n\n"
            "Send me a book title or author to search Gutenberg, "
            "or upload a .epub/.txt file to add it to your library.\n\n"
            "/help for all commands"
        )

    # ── /help ─────────────────────────────────────────────────────────────────
    @bot.message_handler(commands=["help"])
    def cmd_help(msg):
        if not is_authorized(msg):
            return
        bot.reply_to(msg,
            "*Xteink Bot*\n\n"
            "*Books*\n"
            "/search <query> — Search Gutenberg\n"
            "/popular — Top downloads\n"
            "/download <id> — Get book by ID\n"
            "/list — Books in library\n\n"
            "*Todo List*\n"
            "/todo — Show list\n"
            "/add <text> — Add item\n"
            "/done <number> — Toggle done\n"
            "/clear — Remove completed items\n\n"
            "*Other*\n"
            "/myid — Your chat ID\n\n"
            "Send any text to search\\. Upload .epub or .txt to save directly\\."
        )

    # ── /myid ─────────────────────────────────────────────────────────────────
    @bot.message_handler(commands=["myid"])
    def cmd_myid(msg):
        bot.reply_to(msg, f"Your chat ID: `{msg.chat.id}`")

    # ── /list ─────────────────────────────────────────────────────────────────
    @bot.message_handler(commands=["list"])
    def cmd_list(msg):
        if not is_authorized(msg):
            return
        books = list_books()
        if not books:
            bot.reply_to(msg, "No books in library yet.")
            return
        lines = [f"*Library ({len(books)} books):*\n"]
        for b in books:
            lines.append(f"• {b}")
        bot.reply_to(msg, "\n".join(lines))

    # ── /popular ──────────────────────────────────────────────────────────────
    @bot.message_handler(commands=["popular"])
    def cmd_popular(msg):
        if not is_authorized(msg):
            return
        bot.reply_to(msg, "Fetching popular books...")
        try:
            resp = requests.get(GUTENDEX_API, params={
                "languages": "en", "mime_type": "text/plain", "sort": "popular",
            }, timeout=15)
            books = resp.json().get("results", [])[:8]
            if not books:
                bot.reply_to(msg, "No results.")
                return
            lines = ["*Most Popular on Gutenberg:*\n"]
            for b in books:
                authors = ", ".join(a["name"] for a in b.get("authors", []))
                lines.append(
                    f"• *{b['title']}*\n"
                    f"  {authors}\n"
                    f"  /download\\_{b['id']}"
                )
            bot.reply_to(msg, "\n".join(lines))
        except Exception as e:
            bot.reply_to(msg, f"Error: {e}")

    # ── /search ───────────────────────────────────────────────────────────────
    @bot.message_handler(commands=["search"])
    def cmd_search(msg):
        if not is_authorized(msg):
            return
        query = msg.text.replace("/search", "").strip()
        if not query:
            bot.reply_to(msg, "Usage: /search <title or author>")
            return
        do_search(msg, query)

    # ── /download ─────────────────────────────────────────────────────────────
    @bot.message_handler(func=lambda m: m.text and m.text.startswith("/download"))
    def cmd_download(msg):
        if not is_authorized(msg):
            return
        text = msg.text.replace("/download_", "").replace("/download", "").strip()
        try:
            book_id = int(text)
        except ValueError:
            bot.reply_to(msg, "Usage: /download <gutenberg_id>")
            return
        do_download(msg, book_id)

    # ── /todo ─────────────────────────────────────────────────────────────────
    @bot.message_handler(commands=["todo"])
    def cmd_todo(msg):
        if not is_authorized(msg):
            return
        bot.reply_to(msg, format_todo(load_todo()))

    # ── /add ──────────────────────────────────────────────────────────────────
    @bot.message_handler(commands=["add"])
    def cmd_add(msg):
        if not is_authorized(msg):
            return
        text = msg.text.replace("/add", "").strip()[:MAX_TODO_TEXT]
        if not text:
            bot.reply_to(msg, "Usage: /add <item text>")
            return
        items = load_todo()
        if len(items) >= MAX_TODO_ITEMS:
            bot.reply_to(msg, f"List is full ({MAX_TODO_ITEMS} items max). Use /clear to remove done items.")
            return
        items.append({"text": text, "done": False})
        save_todo(items)
        bot.reply_to(msg, f"Added: _{text}_\n\n" + format_todo(items))

    # ── /done ─────────────────────────────────────────────────────────────────
    @bot.message_handler(commands=["done"])
    def cmd_done(msg):
        if not is_authorized(msg):
            return
        text = msg.text.replace("/done", "").strip()
        try:
            idx = int(text) - 1
        except ValueError:
            bot.reply_to(msg, "Usage: /done <item number>")
            return
        items = load_todo()
        if not 0 <= idx < len(items):
            bot.reply_to(msg, f"No item #{idx + 1}. You have {len(items)} items.")
            return
        items[idx]["done"] = not items[idx]["done"]
        save_todo(items)
        status = "✅ Done" if items[idx]["done"] else "↩️ Undone"
        bot.reply_to(msg, f"{status}: _{items[idx]['text']}_\n\n" + format_todo(items))

    # ── /clear ────────────────────────────────────────────────────────────────
    @bot.message_handler(commands=["clear"])
    def cmd_clear(msg):
        if not is_authorized(msg):
            return
        items = load_todo()
        before = len(items)
        items = [i for i in items if not i["done"]]
        save_todo(items)
        removed = before - len(items)
        bot.reply_to(msg, f"Removed {removed} completed item(s).\n\n" + format_todo(items))

    # ── File uploads (.epub / .txt) ───────────────────────────────────────────
    @bot.message_handler(content_types=["document"])
    def handle_file(msg):
        if not is_authorized(msg):
            return
        doc = msg.document
        fname = doc.file_name or "upload"
        ext = os.path.splitext(fname)[1].lower()

        if ext not in (".epub", ".txt"):
            bot.reply_to(msg, "Only .epub and .txt files are supported.")
            return

        bot.reply_to(msg, f"Downloading _{fname}_...")
        try:
            file_info = bot.get_file(doc.file_id)
            file_bytes = bot.download_file(file_info.file_path)
        except Exception as e:
            bot.reply_to(msg, f"Failed to download: {e}")
            return

        dest = os.path.join(BOOKS_DIR, sanitize_filename(
            os.path.splitext(fname)[0], ext=ext
        ))
        # Avoid overwrite
        if os.path.exists(dest):
            base = os.path.splitext(dest)[0]
            dest = f"{base}_{int(time.time())}{ext}"

        try:
            with open(dest, "wb") as f:
                f.write(file_bytes)
        except Exception as e:
            bot.reply_to(msg, f"Save failed: {e}")
            return

        size_kb = len(file_bytes) / 1024
        bot.reply_to(msg,
            f"*Saved!*\n\n"
            f"File: `{os.path.basename(dest)}`\n"
            f"Size: {size_kb:.1f} KB\n\n"
            f"Transfer to X4: connect X4 to Wi\\-Fi hotspot, "
            f"then copy from the books folder via CrossPoint's web UI\\."
        )

    # ── Plain text → search ───────────────────────────────────────────────────
    @bot.message_handler(func=lambda m: m.text and not m.text.startswith("/"))
    def plain_search(msg):
        if not is_authorized(msg):
            return
        do_search(msg, msg.text.strip())

    # ── Helpers ───────────────────────────────────────────────────────────────
    def do_search(msg, query):
        bot.reply_to(msg, f"Searching for _{query}_...")
        results, total = search_gutenberg(query)
        if not results:
            bot.reply_to(msg, "No books found. Try a different search.")
            return
        lines = [f"*Found {total} books. Showing top {len(results)}:*\n"]
        for r in results:
            lines.append(
                f"• *{r['title']}*\n"
                f"  {r['author']} ({r['downloads']:,} downloads)\n"
                f"  /download\\_{r['id']}"
            )
        bot.reply_to(msg, "\n".join(lines))

    def do_download(msg, book_id):
        bot.reply_to(msg, f"Downloading book \\#{book_id}...")
        text, title_or_err = download_gutenberg_book(book_id)
        if text is None:
            bot.reply_to(msg, f"Failed: {title_or_err}")
            return
        filename = sanitize_filename(title_or_err, ext=".txt")
        dest = os.path.join(BOOKS_DIR, filename)
        if os.path.exists(dest):
            base = os.path.splitext(dest)[0]
            dest = f"{base}_{book_id}.txt"
        try:
            with open(dest, "w", encoding="utf-8") as f:
                f.write(text)
        except Exception as e:
            bot.reply_to(msg, f"Save failed: {e}")
            return
        size_kb = os.path.getsize(dest) / 1024
        bot.reply_to(msg,
            f"*Saved!*\n\n"
            f"Title: _{title_or_err}_\n"
            f"File: `{os.path.basename(dest)}`\n"
            f"Size: {size_kb:.1f} KB\n\n"
            f"Transfer to X4 via CrossPoint Wi\\-Fi when ready\\."
        )

    return bot


# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    ensure_dirs()

    if not BOT_TOKEN:
        log.error("No TELEGRAM_BOT_TOKEN set. Add it to your environment or .env file.")
        return

    if not HAS_TELEBOT or not HAS_REQUESTS:
        log.error("Missing dependencies. Run: pip install pyTelegramBotAPI requests")
        return

    log.info("Starting Xteink bot...")
    log.info("Books dir: %s", os.path.abspath(BOOKS_DIR))
    log.info("Data dir:  %s", os.path.abspath(DATA_DIR))
    if ALLOWED_CHAT_ID:
        log.info("Restricted to chat ID: %s", ALLOWED_CHAT_ID)
    else:
        log.info("No chat ID restriction — anyone can use the bot")

    bot = make_bot()

    while True:
        try:
            bot.polling(non_stop=True, interval=2, timeout=20)
        except Exception as e:
            log.error("Polling error: %s — retrying in 5s", e)
            time.sleep(5)


if __name__ == "__main__":
    main()
