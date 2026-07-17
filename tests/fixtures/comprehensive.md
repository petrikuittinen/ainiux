# Comprehensive Markdown Fixture

This document exercises **bold text**, *italic text*, ++underlined text++,
`inline code`, and a [normal link](https://example.com/docs?lang=en&mode=test).

English: The quick brown fox tests a portable Markdown renderer.

Chinese: 你好，世界！这是一个 Markdown 转换测试。

Arabic: مرحبا بالعالم! هذا اختبار لتحويل Markdown.

Emoji: 😀 🚀 ✅ 🌍 👨‍👩‍👧‍👦

## Lists And Structure

### Unordered List

- First item
- Second item with **bold content**
  - Nested item in Chinese: 嵌套项目
  - Nested item in Arabic: عنصر متداخل
- Final item with an [inline link](https://example.com/list)

### Ordered List

1. Prepare the fixture
2. Convert the document
   1. Preserve Unicode
   2. Preserve structure
3. Verify the result ✅

## Image And Link

[![A linked placeholder image](https://example.com/assets/placeholder.png)](https://example.com/gallery)

Standalone image: ![Standalone placeholder](https://example.com/assets/standalone.png)

## Quotation And Code

> A block quote with **strong emphasis**, multilingual text 你好 مرحبا, and emoji 💬.

```javascript
function greet(name) {
  return `Hello, ${name}!`;
}
console.log(greet("ainiux"));
```

## Table

| Feature | English | Chinese | Arabic |
| --- | --- | --- | --- |
| Greeting | Hello | 你好 | مرحبا |
| Status | **Ready** | 已准备好 | جاهز |
| Emoji | 🚀 | 🌏 | ✅ |

## Mixed Formatting

Text can be **bold**, *italic*, ++underlined++, and linked to
[the ainiux example page](https://example.com/ainiux#markdown).

### Third-Level Heading

This final section confirms that at least three heading levels are represented.

End of comprehensive Markdown fixture.
