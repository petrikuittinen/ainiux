# XLSX test fixtures

Successful conversion inputs. Copied from the PhpSpreadsheet reader corpus
(`tests/data/Reader/XLSX`) as a curated, dependency-free subset:

- `blankcell.xlsx` — empty cells in the used range
- `without_cell_reference.xlsx` — `<c>` elements without `r`
- `threesheets.xlsx` — multiple worksheets
- `1900_Calendar.xlsx` / `1904_Calendar.xlsx` — date systems
- `explicitdate.xlsx` — `t="d"` ISO dates
- `namespacestd.xlsx` / `namespacepurl.xlsx` / `namespacenonstd.xlsx`
- `utf16be.xlsx` / `utf16be.bom.xlsx` — UTF-16 XML
- `sharedformulae.xlsx` — cached formula values
- `Zip-Windows-Directory-Separator.xlsx` — `\\` ZIP names
- `rootZipFiles.xlsx` — non-standard workbook path
- `countries-population.html` / `.md` / `.xlsx` — Wikipedia
  [list of countries and dependencies by population](https://en.wikipedia.org/wiki/List_of_countries_and_dependencies_by_population)
  snipped to Location/Population/% of world/Date/Source (240 body rows). HTML was
  converted with `ainiux --input … --output-format md`, then Markdown with
  `--output-format xlsx`. Flags, reference superscripts, and the Notes column
  were dropped so GFM pipes stay aligned.
- `hello-world.html` / `.md` / `.xlsx` — “Hello world” in Latin, Han (simplified and
  traditional), Hiragana/Kanji, Hangul, Arabic, Hebrew, Cyrillic, Greek, Thai,
  Devanagari, Bengali, Tamil, Georgian, Armenian, and Ethiopic. Converted with
  `ainiux --input … --output-format md` then `--output-format xlsx`.

Golden Markdown is asserted in `tests/unit/xlsx/test_xlsx.cpp` rather than
sibling `.md` files so numeric/date formatting stays in one place.

Malformed inputs live in `errors/`.
