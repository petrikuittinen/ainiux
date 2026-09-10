# Everything Markdown Fixture

This file is a **syntax-highlighting** and **Markdown-to-PDF** fixture. It
exercises the constructs Ainiux actually renders: ATX headings, paragraphs,
emphasis, links, images, lists, quotes, tables, fenced and indented code,
thematic breaks, HTML fragments, entities, and escapes.

A second paragraph follows a blank line. Hard break after this sentence.  
The next line stays in the same paragraph.

Inline mix: **bold**, *italic*, ***bold italic***, `inline_code()`,
++underlined++, ~~strikethrough~~, an [inline link](https://example.com/path?q=1),
a reference link [ainiux docs][docs], autolink
<https://example.com/autolink>, and a bare URL https://example.com/bare.

Escapes and entities: \*not emphasis\*, \[not a link\], ampersand &amp;,
copyright &copy;, em dash &#8212;.

HTML fragments: press <kbd>Ctrl</kbd>+<kbd>P</kbd> and a line break<br>inside
a paragraph.

[docs]: https://example.com/ainiux#markdown "Ainiux Markdown"

Setext-style heading
====================

This paragraph sits under a Setext `=` underline (highlight treats the
underline as an operator; PDF layout keeps `=` lines as body text).

## Images

Standalone image with alt text (PDF writes the alt, not pixels):

![China EV sales chart](../image_files/China_EV_sales_March_2024.png)

Linked image:

[![Gallery placeholder](https://example.com/assets/placeholder.png)](https://example.com/gallery)

## Block quote

> Outer quote with **bold**, a [quoted link](https://example.com/quote), and
> a second quoted line.
>
> > Nested quote: *still quoted*, with `code`.

A thematic break follows and is drawn as a hairline, not a page break.

---

## Lists

Unordered with `-`, nested, and mixed markers:

- Alpha with **bold**
- Bravo with [a link](https://example.com/list)
  - Nested one
  - Nested two with `code`
- Charlie
* Star marker
+ Plus marker
- [ ] Open task (plain list text)
- [x] Done task (plain list text)

Ordered, including a nested ordered list:

1. First step
2. Second step with *italic*
   1. Nested ordered
   2. Nested ordered again
3. Third step
10. Double-digit start
11. Continues after 10

## Headings through level six

### Level three
#### Level four
##### Level five
###### Level six with closing hashes ######

Short body under H6 so the PDF footer heading can change.

***

## Table

Pipe table with alignment, inline markup, and a URL in a cell:

| Feature | Left | Center | Right |
| :------ | :--- | :----: | ----: |
| Emphasis | **bold** | *italic* | ~~strike~~ |
| Code | `left` | `mid` | `right` |
| Link | [docs](https://example.com/table) | 42 | 1000 |

Table without leading or trailing pipes:

Name | Value
---- | -----
alpha | 1
beta | 2

## Fenced code

Python:

```python
def greet(name: str) -> str:
    """Return a greeting used by the PDF and highlight fixtures."""
    return f"Hello, {name}!"


if __name__ == "__main__":
    print(greet("ainiux"))
```

JavaScript:

```javascript
function greet(name) {
  return `Hello, ${name}!`;
}
console.log(greet("ainiux"));
```

C++:

```cpp
#include <string>
std::string greet(const std::string& name) { return "Hello, " + name + "!"; }
```

Indented code block (four spaces):

    int answer = 42;
    printf("answer=%d\n", answer);

Unlabeled fence:

```
plain fenced block
with two lines
```

___

## Long wrapping paragraph

The PDF writer wraps WinAnsi text to the universal page width. This paragraph
is long enough to wrap: portable command-line clients should keep conversion
offline, bound input size, refuse binary documents as prompt text, and emit
actionable errors. Ainiux converts Markdown to PDF with Core-14 Helvetica and
Courier, Flate content streams, and a classic xref table. Words such as
MarkdownToPdfNeedle and WrapAndPaginateNeedle should survive extract after
`--output-format pdf`.

## Unicode and substitution

WinAnsi-safe Latin is the default. The next line is for substitution tests
(CJK and emoji become `?` in PDF until a Unicode font slice lands):

你好，世界 — مرحبا بالعالم — 😀 🚀 ✅

End of everything Markdown fixture.
