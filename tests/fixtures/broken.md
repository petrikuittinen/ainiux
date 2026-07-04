# Broken Markdown Fixture

Paragraph with **unclosed bold and *mixed markers* that never finish.

[broken link without closing paren(https://example.com/broken

![broken image](https://example.com/image.png

~~strikethrough without closing

### Heading without blank line before
Still part of heading attempt?

| table | missing |
row without separator
| bad | row |

```
fenced code without closing fence
still inside code block

- list item one
  - nested item
1. ordered after unordered without blank line
2. second ordered

> block quote line
forgets continuation marker on next line
should still appear as text

Random HTML snippet in markdown: <div class="unclosed">

End of broken markdown fixture.