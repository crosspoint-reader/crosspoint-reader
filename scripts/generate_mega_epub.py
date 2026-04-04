from ebooklib import epub

book_sizes = [1000, 2000, 3000, 5000, 10000, 17000, 19000, 65530]

# This script generates epubs with entirely too many toc and spine entries
for x in book_sizes:
    book = epub.EpubBook()
    book.set_identifier(f"MegaBook-{x}")
    book.set_title(f"MegaBook-{x}")
    book.set_language('en')
    book.spine = ['nav']
    for y in range(0,x):
        chapter = epub.EpubHtml(title=f'{y}', file_name=f'{y}.xhtml')
        chapter.content = f"<h1>Chap {y}</h1><p>Chapchapt {y}</p>"
        book.add_item(chapter)
        book.spine.append(chapter)
        book.toc.append(chapter)
    # Add navigation
    book.add_item(epub.EpubNcx())
    book.add_item(epub.EpubNav())
    epub.write_epub(f'{x}.epub', book)