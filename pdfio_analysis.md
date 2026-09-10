# pdfio Library Analysis: Reading and Writing PDF Files

This document describes how the **pdfio** C library (version 1.7.0, Michael R. Sweet, Apache 2.0) reads and writes PDF files. The goal is to give enough detail (algorithms, data structures, constants, error handling, edge cases) to replicate its behavior independently.

Source layout (all C files include `pdfio-private.h`):

| File | Role |
|---|---|
| `pdfio.h` | Public API (all `pdfio*` functions) |
| `pdfio-private.h` | Internal structures, constants, internal function prototypes |
| `pdfio-file.c` | Open/close/read plumbing: header, startxref, xref tables/streams, repair, page tree, trailer write, object stream write |
| `pdfio-common.c` | Buffered file I/O (read/write/seek/tell/gets/peek/consume), error reporting, flush |
| `pdfio-token.c` | Lexer: tokens (strings, names, numbers, keywords, comments, delimiters) |
| `pdfio-value.c` | Parse/write values, dates, decrypting values; `_pdfio_strtod` locality-safe double parse |
| `pdfio-dict.c` / `pdfio-array.c` | Dictionary and array data structures + read/write |
| `pdfio-object.c` | Lazy object load, object stream detection, object write header |
| `pdfio-stream.c` | Stream read/write, Flate/LZW decode, ASCII85 decode, PNG/TIFF predictors, stream encryption |
| `pdfio-lzw.c` | LZW decompressor (read only) |
| `pdfio-aes.c` / `pdfio-rc4.c` / `pdfio-md5.c` / `pdfio-sha256.c` | Crypto primitives (AES-128 CBC, RC4, MD5, SHA-256) |
| `pdfio-crypto.c` | Standard security handler: key derivation, unlock/lock, per-object crypto callbacks |
| `pdfio-string.c` | Durable string pool, UTF-16→UTF-8, locale-safe `printf` with `%H/%N/%S` |
| `pdfio-page.c` | Page accessors (with parent inheritance), page copy |
| `pdfio-content.c/.h` | Page content stream generator/parser (PDF operators), fonts, images |

---

## 1. Key internal data structures

### `_pdfio_file_s` (per-file state, `pdfio-private.h`)
- `filename`, `file_id[32]` (SHA-256 of filename + creation time + trailer ID values — used to identify source files when copying objects between files)
- `version` (string like `"1.4"`, `"2.0"`), `profile` (PDF/A or PCLm profiles)
- `media_box`, `crop_box` (default page rectangles; default = "universal" size 8.27in × 11in → x2 = 210.0*72/25.4, y2 = 11*72)
- `mode` = `_PDFIO_MODE_READ` or `_PDFIO_MODE_WRITE`
- error callback + data; default callback prints `filename: message` to stderr and returns `true` only for messages starting with `"WARNING:"`
- encryption state: `encryption` (mode), `permissions`, `file_key[16]`, `owner_key[32]`, `user_key[32]`, `password[32]`, key lengths, `encrypt_metadata`
- I/O: `fd`, 8192-byte buffer (`PDFIO_BUF_SIZE`), `bufptr/bufend`, `bufpos` (file offset of buffer start); `output_cb` + ctx for streamed output
- parsed objects: `trailer_dict`, `root_obj` (catalog), `info_obj`, `pages_obj`, `encrypt_obj/encrypt_dict`, `objstm_obj/objstm_dict`, `id_array`, `markinfo`, `cache` (TTF font cache)
- memory pools: `arrays`, `dicts`, `objs` (sorted by object number), `objmaps` (copy-cache, sorted by source file id + number), `pages`, `strings` (deduped sorted string pool), `strbuffers` (reusable 128KB+32 scratch buffers)
- `num_objstm`, `last_obj` (hint for binary insert position)

### `_pdfio_obj_s` (an object)
- `pdf`, `number` (1..N), `generation`, `offset` (file offset), `length_offset` (offset of `/Length` placeholder for write), `stream_offset`, `stream_length`
- `value` (`_pdfio_value_t`; `PDFIO_VALTYPE_NONE` until loaded)
- `stream` (open stream handle), `objstm_number`, `objstm_data/objstm_datalen` (object-stream buffered write data)
- `data`/`datafree` extension fields for internal users (fonts, images, etc.)

### `_pdfio_value_t`
union of: array, binary (raw bytes + len), boolean, date (`time_t`), dict, indirect (`number` + `generation`), name, number (`double`), string, null.

### `_pdfio_token_t`
- `pdf`, `consume_cb`/`peek_cb` + `cb_data` (either the file or a stream), 256-byte char buffer, `bufptr/bufend`, up to 4 saved tokens (`tokens[4]`).

### `_pdfio_stream_s`
- `pdf`, `obj`, `length_obj` (indirect length object for output-callback mode), `filter`, `remaining` (bytes left in encoded stream), 8192-byte `buffer`
- ASCII85 state (`a85buffer` 5200 bytes, decoded block), `z_stream flate`, `_pdfio_lzw_t *lzw`
- predictor state: `predictor`, `pbpixel`, `pbsize`, `cbuffer`, `prbuffer` (previous raw line), `psbuffer` (PNG filter byte + current raw line)
- crypto: `crypto_cb` + `crypto_ctx` (RC4 or AES context + IV)

### Limits / constants
- `PDFIO_BUF_SIZE` = 8192; `PDFIO_MAX_DEPTH` = 32 (nesting of arrays/dicts and page-tree depth); `PDFIO_MAX_STRING` = 131072 (max token/string bytes)
- xref table chain: max 100 (`xrefs[100]`); object streams per file: max 16384 (`sobjs[16384]`); objects inside one object stream: max 16384 (`objs[16384]`)
- token whitespace/comment run: max 2048 bytes before "Too much whitespace"/"Comment too long"
- `_pdfio_obj_s.objstm` write limit: `num_objstm < 0x10000` (65536)

---

## 2. Reading PDF files

### 2.1 Entry point: `pdfioFileOpen(filename, password_cb, password_data, error_cb, error_data)`

Steps (pdfio-file.c `pdfioFileOpen`, ~line 1116):

1. Validate args; allocate `pdfio_file_t` (calloc); set mode READ, permissions = `PDFIO_PERMISSION_ALL`; `get_lconv()` for locale-safe numeric parsing; `strdup` filename.
2. `open(filename, O_RDONLY | O_BINARY)` (Windows uses UTF-8→UTF-16 wrapper `_pdfio_win32_open`). Failure → error "Unable to open file - %s".
3. Read the **first line** with `_pdfioFileGets`.
4. **Header check**: accepts `%PDF-1.` or `%PDF-2.` followed by a digit (`isdigit(line[7])`). Anything else → "Bad header '%s'." and open fails. `pdf->version = strdup(line + 5)` stores `"1.7"`, `"2.0"`, etc. This is the *only* version gating for basic reading; the rest is tolerant of anything syntactically valid.
5. **Find startxref**: `lseek` to `1 - sizeof(line)` (= −1025) from SEEK_END (clamped implicitly by seek to byte 0 for small files), read up to 1024 bytes (the last ~1KB of file). Scan forward for a `"startxref"` occurrence that is not followed by another `"startxref"` and whose offset parses to a positive number (so trailing `%%EOF` and comments don't confuse it; also `strtol(ptr+9)>0`).
6. If `startxref` found → `load_xref(pdf, xref_offset, ...)`. If not → warning "WARNING: Unable to find start of cross-reference table, will attempt to rebuild." and, if the error callback returns `true`, `repair_xref(...)`; otherwise open fails.
7. After xref load: compute `file_id` = SHA-256(filename ∥ creationDate-time_t ∥ ID[0] ∥ ID[1]) used for object-copy identity.
8. On any fatal error the function `goto error` → `pdfioFileClose(pdf)` → return NULL.

### 2.2 `load_xref` — classic tables and cross-reference streams

Loop over `xref_offset` (single entry for first xref; up to 100 for `Prev` chain; duplicate/recursive `Prev` offsets → "Recursive xref table." fatal; more than 100 → "Too many xref tables." fatal):

1. Seek to `xref_offset`; read first non-empty line.
2. **Detect xref-stream object**: line matches `%d+ ... obj` (first digit, contains `" obj"`, and after `" obj"` is `'<'` or whitespace). Then:
   - parse object number `(>=1)` and generation `(0..65535)`; must be followed by `obj`.
   - seek to just after `obj`; tokenize the dictionary with `_pdfioValueRead` (object tokenizer bound to the file); require `PDFIO_VALTYPE_DICT` (else "repair").
   - expect next token exactly `stream`; skip one CR/LF after token; record `obj->stream_offset = tell()`.
   - get `Index` array (default = one pair counting from 0, count = 999999999) and `W` array (required; missing → repair).
   - Validate `W`: at most 3 entries, `w[1] != 0`, `w[2] <= 4` (w[0] may be 0 per spec — "no field" case handled), each `w[i] <= 32`; total `w_total = w0+w1+w2 <= 32`.
   - Open the **decoded** stream (`pdfioObjOpenStream(obj, true)`; encryption is skipped for `/Type /XRef` objects).
   - For each entry: read `w_total` bytes; if `w[0] > 0` and type byte == 0 → free object, skip (number++). Else decode big-endian offset from `w[1]` bytes and generation from `w[2]` bytes.
     - **Generation size fixes (Issue #46)**: Microsoft generators use 3-byte or 4-byte generation fields; values > 65535 are clamped to 65535.
   - Entry type 1 (regular, or w[0]==0) → offset = field; type 2 (in object stream) → offset field is the **object-stream number**, index written into the stream; such objects are queued in `sobjs[]` for later `load_obj_stream`.
   - If an object with the same number already exists and the new generation is **higher**, update generation and (for type 1/0-field) offset; type-2 with different number → offset = 0 (will come from object stream load).
   - Missing → `add_obj(pdf, number, generation, offset)` (placeholder, value lazy-loaded later).
3. **Classic xref table** (`xref` + spaces): read lines until `trailer`:
   - subsection header: `START COUNT` (sscanf "%jd%jd"); blank lines skipped.
   - each record is exactly **20 bytes**: 10-byte offset, 1 space, 5-byte generation, 1 space, 1 type byte, 2-byte EOL.
   - **EOL tolerance**: accepted EOLs are `"\r\n"`, `"\r\r"`, `" \n"`, `" \r"` (some generators write space+CR — handled), else repair.
   - offset parsed with `strtoimax` (must be ≥0), generation 0..65535 (unless offset == 0, i.e., free entries, where generation may be 65535), types `f` (skip) or `n` (in-use: adds placeholder if not already present).
   - find `trailer` keyword; if `trailer` is followed by content on the same line, rewind to after `trailer` and parse the dictionary.
4. **Trailer handling** (both kinds): first trailer wins (only used when `!pdf->trailer_dict`; subsequent trailers are only consulted for `Prev`). Store `trailer_dict`, `id_array = /ID`, and `encrypt_dict`: `/Encrypt` as an **indirect object** (`pdfioDictGetObj`) or as an **inline dict** (`pdfioDictGetDict` — nonstandard but tolerated).
5. If `encrypt_dict` present → `_pdfioCryptoUnlock(...)` (see §2.4). Failure is fatal (open aborts).
6. After the chain: `info_obj = /Info` ref; `root_obj = /Root` ref (missing → repair); `pages_obj = /Pages` ref in the catalog (missing → repair); then `load_pages(pdf, pages_obj, 0)`.

Any hard error inside the loop jumps to the `repair:` label → warning "WARNING: Cross-reference is damaged, will attempt to rebuild." → if error callback returns true → `repair_xref()` else fail.

### 2.3 `repair_xref` — rebuilding a broken file

Scans the whole file from byte 0, line by line (does not rely on xref):

1. Resets trailer/root/info/pages/encrypt state.
2. For each line starting with a digit 1–9, tries `N G obj` (`strtoimax` N ≥ 1, generation 0..65535, then optional whitespace + `obj`). If matched:
   - `add_obj` (or update `offset` of existing object), rewind to just after `obj` if more content on the line.
   - `_pdfioValueRead` the object value. If that fails: warning "WARNING: Unable to read object dictionary/value." and continue (skips this object).
   - Read the next token: if `stream` → set `stream_offset`; if object type is `ObjStm` queue it; if type is `XRef` and no trailer yet, use its dictionary as the trailer. If `endobj` and type is `Catalog`, synthesize a backup trailer with `/Root` (used only if no real trailer was found).
3. Lines starting with `trailer` → parse trailer dictionary (rewind after keyword if same line); use it only if it contains `/Root`.
4. If no trailer found anywhere, use the backup catalog-derived trailer.
5. Unlock encryption if `encrypt_dict` exists.
6. Load all queued object streams (see §2.6).
7. Resolve `info_obj`, `root_obj`, `pages_obj` (fatal if missing: "Missing Root object." / "Missing Pages object."), then `load_pages`.

Repair works even when: startxref missing/corrupt, xref table corrupt, streams mislabeled, or object offsets wrong — as long as `N G obj` headers are intact.

### 2.4 Encryption: `_pdfioCryptoUnlock` (Standard security handler)

Reads from `encrypt_dict`:
- `/Filter` must be `Standard` (else "Unsupported security handler").
- `/V` version: 
  - V=1 or 2: R=2 → `PDFIO_ENCRYPTION_RC4_40` (length forced 40); R=3 → `RC4_128` (length 40..128, default 128).
  - V=4, R=4: reads `/CF` crypt filters; requires `/StmF` and `/StrF` **equal** names (else "Different stream and string encryption filters - not supported."), looks up the named filter, `/CFM` must be `V2` (→ RC4_128) or `AESV2` (→ AES_128); `/Length` defaulted to 128 bits if missing/out of range.
  - V=6, R=6: sets `AES_256` but **unlock is unimplemented** → "Unable to unlock AES-256 encrypted file at this time." (open fails).
  - anything else → "Unsupported encryption V%d R%d."
- `/EncryptMetadata` boolean (default true) → `pdf->encrypt_metadata`; affects file-key derivation.
- `file_keylen = Length/8` (5..16 bytes; else "Unsupported key length %d.").
- `/P` permissions: `double`; if `< 0x7fffffff` cast directly, else subtract 4294967296 (convert unsigned 32-bit to signed).
- `/O` owner key (≥32 bytes) and `/U` user key (≥32 bytes) copied into `pdf`; `/ID` array element 0 used as file ID (≥16 bytes, else "Missing or bad file ID...").

**Password guessing loop** (max 4 tries):
1. `pad_password(password)` → 32-byte padded password using the standard PDF padding string:
   `0x28 0xbf 0x4e 0x5e 0x4e 0x75 0x8a 0x41 0x64 0x00 0x4e 0x56 0xff 0xfa 0x01 0x08 0x2e 0x2e 0x00 0xb6 0xd0 0x68 0x3e 0x80 0x2f 0x0c 0xa9 0xfe 0x64 0x53 0x69 0x7a`
   (password truncated to 32 bytes if longer; empty password = all padding).
2. `test_password()`: derive file key with `make_file_key` (below), then decrypt `/U` with `decrypt_ou_key` and compare:
   - RC4-40 (R2): decrypted U must equal the padding string.
   - R3/R4: decrypted U must equal the computed user key `md5(passpad ∥ ID) ∥ 16 zero bytes` (first 16 bytes compared).
3. If user-password test fails → derive owner key via `md5^50(owner_pad)` (or plain MD5 for RC4-40), decrypt `/O` with it 1× (RC4-40) or 20× (R3/R4) with key XORed by loop index, and test the result as the user password.
4. If both fail and `password_cb != NULL`, call `password_cb(data, filename)` for a new password and try again; 4 attempts; finally "Unable to unlock PDF file.".

**Key derivations** (in `pdfio-crypto.c`):
- `make_password_key`: `key = md5(pad)`; for RC4-128/AES-128 repeat `md5(key[:file_keylen])` 50 times; take first `file_keylen` bytes.
- `make_file_key(F)`: `md5(user_pad ∥ owner_key ∥ P4(LE) ∥ file_id)`, append `0xffffffff` if `!encrypt_metadata`; then 50× `md5(digest[:file_keylen])`; `file_key = digest[:file_keylen]`.
- `make_owner_key`: `owner_key = encrypt_ou_key(user_pad, rc4(md5^50(owner_pad)))` (1× for RC4-40, 20× for 128-bit).
- `make_user_key`: `md5(passpad ∥ file_id) ∥ 0[16]`.

**Per-object crypto callbacks** (`_pdfioCryptoMakeReader`/`Writer`): object key = `md5(file_key ∥ o1 ∥ o2 ∥ o3 ∥ g1 ∥ g2)` where o = object number (little-endian, 3 bytes) and g = generation (little-endian, 2 bytes); for AES-128 append `"sAlT"` (0x73 0x41 0x6C 0x54). RC4-40 uses only 5 key bytes and calls `md5(data[0..9])` (i.e., 5-byte file key + 5 obj bytes). 
- RC4: `RC4Init(digest, 10 or 16)`; no IV.
- AES-128: **CBC**; first 16 bytes of the stream/string are the IV; decrypt rounds with IV chaining; PKCS#7 padding removed on last block: `if last && outbytes>0 && outbuffer[-1] <= 0x10 → outbytes -= outbuffer[-1]`.
- `_pdfioCryptoMakeRandom`: arc4random (macOS), CryptGenRandom (Windows), `getrandom` (Linux), `/dev/urandom` fallback, else Mersenne-Twister seeded from time.

**Not encrypted** (read path): the xref-stream object itself (type XRef), trailer `/ID` values, and `/Type /XRef` stream data. Everything else (strings, binary strings, stream data of non-XRef objects) is decrypted.

### 2.5 Tokenizer (`pdfio-token.c`)

`_pdfioTokenRead` reads one token through a byte-level buffered callback pair (peek/consume on either the whole file or a stream). Grammar handled:

- **Whitespace/comment skipping**: `%` starts a comment to EOL; runs >2048 bytes → "Comment too long."/"Too much whitespace."
- **Delimiters**: `< > ( ) { } [ ] / %` set the state machine.
- **Literal strings** `( ... )`:
  - tracks nested parens level; `\` escapes: `\n \r \t \b \f \\ \( \)` and **1–3 digit octal** (`\0`..`\7`; up to 2 more octal digits; non-octal next char is un-read via `bufptr--`).
  - unknown escape → the backslash is dropped (per spec, ignore).
  - if a `\0` byte occurred inside → the token is converted to a **hex string** token (`<HHHH...`) because C strings can't hold NULs.
  - if the string starts with `\376\377` (UTF-16BE) or `\377\376` (UTF-16LE) and has ≥1 more byte → converted to UTF-8 in place by `_pdfio_utf16cpy`.
  - unterminated → "Unterminated string literal."
- **Hex strings** `< ... >`: keeps only even-length digit stream (whitespace allowed, odd digit counts tolerated at value level: trailing nibble padded with 0); `<>` (empty hex) is **treated as an empty literal string** `()` (Issue #46 Microsoft quirk); malformed char → "Syntax error: '<%c'" / "Invalid hex string character '%c'." / "Unterminated hex string."
- **Names** `/...`: `#HH` escapes decoded (bad `#` → "Bad # escape in name."); delimiters terminate the name.
- **Numbers**: `[+-]digits.digits` (state machine is permissive: non-digit/dot terminates; `_pdfio_strtod` parses, converting `.` to the locale decimal point first).
- **Keywords**: any run of non-whitespace, non-delimiter chars.
- Stacked tokens (`_pdfioTokenPush`/`Get`): up to 4 save/restore — used by the array reader to un-read the first token.

### 2.6 Value parser (`pdfio-value.c` `_pdfioValueRead`)

Top-level dispatch on the first token:

| Token | Result |
|---|---|
| `[` | array (`_pdfioArrayRead`; depth check `PDFIO_MAX_DEPTH=32` → "Too many nested arrays.") |
| `<<` | dictionary (`_pdfioDictRead`; depth check → "Too many nested dictionaries.") |
| `(` | try `get_date_time(token+1)` (see below); if date → `PDFIO_VALTYPE_DATE`, else string |
| `/` | name |
| `<` | hex string → binary (`datalen = strlen/2`, hex→bytes, odd last digit padded) |
| number-like | integer-only token + lookahead (fill buffer if <10 bytes remain) for `N\s+G\s+R` → indirect ref; else number via `_pdfio_strtod` |
| `true`/`false` | boolean |
| `null` | null |
| anything else | "Unexpected '%s' token seen." → NULL |

**Date parsing** (`get_date_time`): strings starting `D:` followed by 6–16 digits (even count ≥6) parsed as `YYYYMMDDhhmmss`, optional `Z`, `+HH'mm'` or `-HH'mm'` zone (with optional trailing quote), converted to `time_t` via `timegm`/`mktime`+tz correction; returns 0 if any part is malformed (then treated as a plain string).

**Dictionary read** (`_pdfioDictRead`): loop: `>>` ends; keys must start with `/` (else "Invalid dictionary contents."); value read via `_pdfioValueRead` (missing → "Missing value for dictionary key '%s'."); **duplicate keys → warning "WARNING: Discarding value for duplicate dictionary key '%s'." and first value wins** (Issue 118). Pairs stored sorted (qsort on `strcmp`) and found via `bsearch`.

**Array read** (`_pdfioArrayRead`): reads tokens until `]`; each token is pushed back and parsed as a value.

**Strings/tokens memory**: values hold durable strings from the file's string pool (`pdfioStringCreate`, sorted deduped); binary data is malloc'd per value (freed on dict clear/delete).

### 2.7 Lazy object loading (`_pdfioObjLoad` in pdfio-object.c)

Objects from the xref are placeholders (`value.type == NONE`, `offset` set). On first access (`pdfioObjGetDict`, `pdfioObjGetArray`, `pdfioObjGetName`, `pdfioPageOpenStream`, etc.):

1. `lseek` to `obj->offset`; peek 63 bytes.
2. Verify header: `N G obj` where N and G **exactly** match the xref record (mismatch → "Bad header for object %lu."); optional whitespace; `obj` may be followed directly by `<`/`[`/whitespace.
3. Consume header bytes; tokenize from that position; read value (`_pdfioValueRead`); read next token: if `stream` → `stream_offset = tell()` (stream data starts after token + its trailing CR/LF, consumed via `_pdfioTokenFlush`).
4. Decrypt the object's value strings/arrays/dicts recursively (`_pdfioValueDecrypt` → `_pdfioArrayDecrypt`/`_pdfioDictDecrypt`) when `pdf->encryption && pdf->encrypt_metadata` (exact source condition, pdfio-object.c `_pdfioObjLoad`; note that this couples value decryption to the EncryptMetadata flag — harmless in the common all-encrypted case, but relevant when replicating). Stream data decryption happens separately in `_pdfioStreamOpen` (per-object callback), and the `/ID` / xref-stream entries are never decrypted.
5. `endobj` is never explicitly verified (the tokenizer just stops at `endobj` which is fine as the stream check already consumed the dictionary+stream).

**Object streams** (`load_obj_stream` in pdfio-file.c):
- open the ObjStm object decoded; `N = /N` (mandatory), read `N` pairs of `number offset` tokens (non-digit → stop), create placeholder objects (number, generation **0**, offset 0).
- then sequentially read exactly `N` values with `_pdfioValueRead` into those objects (using the object-stream's `/First` boundary implicitly by token position).
- limits: N ≤ 16384 per stream; too many object streams per file → fatal.
- Compression: ObjStm is typically FlateDecoded; **nested** object streams are not (the values are dictionaries/arrays, not streams).
- Offsets for xref type-2 entries refer to the object stream's object number; index is the position in the (number,offset) header list — offset header values are actually not used by pdfio (it reads the objects sequentially in listed order), and the xref entry's third field is treated as the index.

### 2.8 Streams: `pdfioObjOpenStream` / `_pdfioStreamOpen`

1. Requires object loaded, a dict, and `stream_offset`; `current_obj` must be NULL (one open stream per file — enforced error "Another object (%u) is already open.").
2. `remaining = pdfioObjGetLength(obj)`:
   - `/Length` direct number > 0, else `/Length` **indirect object** (lazy-loaded number; must be > 0), else "No stream data." error. (Note: unlike the spec, pdfio does *not* scan backward from `endstream` if Length is missing.)
3. Seek to `stream_offset`.
4. **Encryption**: if `pdf->encryption && type != "XRef"`: peek IV (AES-128: 16 bytes consumed and subtracted from `remaining`; RC4: no IV), set up per-object decrypt callback; for AES round `remaining` up to multiple of 16: `remaining = (remaining + 15) & ~15`.
5. **decode=false**: `filter = NONE` → caller gets raw (still decrypted) bytes.
6. **decode=true** filters (from `/Filter` name or array):
   - `FlateDecode` → zlib `inflateInit` (streaming; no `Z_SYNC_FLUSH`; concatenated members not handled).
   - `LZWDecode` → `_pdfioLZWCreate(8, early=1, reversed=false)`; `/DecodeParms` `/EarlyChange` (0..100; default 1) adjusts code-size growth; clear code = 256, EOD = 257; codes packed MSB-first; table up to 4096 entries; "Output overflow"/"Table loop detected" protections.
   - `ASCII85Decode` + exactly one other filter → wrapped reader (a85buffer 5200 bytes), then the second filter applies. `z` = 4 zero bytes, `~>` terminator, groups of 5 → 4 bytes, last group padded with `u` (84). Bare `ASCII85Decode` alone is **not** handled in `_pdfioStreamOpen` (only in the a85+filter combination; the enum lists ASCII85 as read-supported, and the compound case is what's implemented).
   - Filter array with >2 entries or 2 entries not starting with ASCII85Decode → "Unsupported compound stream filter."
   - anything else → "Unsupported stream filter '/%s'."
   - `/DecodeParms` for Flate/LZW: `/BitsPerComponent` (allowed 1,2,4,8,16 — explicitly rejects 3, 5-7, 9-15, >16; default 8), `/Colors` (1..32, default 1), `/Columns` (≤65536, default 1), `/Predictor`:
     - 1 or absent = none; 2 = TIFF predictor 2; 10–15 = PNG filters (the *filter byte per row* is honored; `0/10` none, `1/11` sub, `2/12` up, `3/13` average, `4/14` paeth; "Bad PNG filter %d" otherwise)
     - decode *must* read whole rows (`pbsize = (bpc*colors*columns+7)/8`, rows are exactly that; for PNG predictor rows = pbsize+1); "Read buffer too small for stream." if shorter.
7. `pdfioStreamRead` returns decoded bytes; read of exactly `bytes` may return fewer at stream end; `pdfioStreamGetToken/Peek/Consume`/`Printf`/`Write`/`PutChar`/`Puts` for content processing (get-token uses the same tokenizer with stream callbacks).
8. `pdfioStreamClose` frees inflate/LZW state and buffers.

### 2.9 Page tree: `load_pages` + page accessors

`load_pages(pdf, obj, depth)`:
- object must have a dict; `Type` must be `Pages` or `Page`, else warning "WARNING: No Type value for pages object." (tolerated, warned).
- if `/Kids` array → recurse into each (as indirect refs), depth+1; `depth >= PDFIO_MAX_DEPTH (32)` → "Depth of pages objects too great to load." (fatal for that load path).
- else (no Kids) → treated as a leaf page: append to `pdf->pages[]`.
- Infinite recursion through Kids is bounded by depth; a page tree with both Kids and leaf content is treated as a parent node (children scanned).
- `pdfioFileGetPage(pdf, n)` returns `pages[n]` (0-based).

Page accessors (`pdfio-page.c`) read from the page dict **or inherited from ancestors** via `Parent` chain (`get_page_value` walks `Parent` refs until found/end; each node lazy-loaded). `Contents` may be an indirect stream **or an indirect array of streams** (`get_contents` resolves it); `pdfioPageOpenStream(page, n, decode)` opens stream n (arrays) or stream 0.

### 2.10 Metadata / IDs

- `/ID` array (2 binary strings, ≥16 bytes) from trailer; `pdfioFileGetID`.
- Info dictionary: `Author`, `Creator`, `Keywords`, `Producer`, `Subject`, `Title`, `CreationDate`, `ModDate` (`get_info_string`, binary strings converted via `pdfioDictGetString` which handles UTF-16, and (for binary<4096) converts to string in place).
- `pdfioFileGetLanguage` reads catalog `/Lang` (BCP 47 tag, e.g., "en-CA").
- Version: `pdfioFileGetVersion` returns the header version string (not a normalized one).

---

## 3. Edge cases, tolerance, and error handling

### 3.1 Error callback contract
- Every error/warning goes through `_pdfioFileError(pdf, fmt, ...)` → `error_cb(pdf, message, data)`.
- **"WARNING:"-prefixed messages are recoverable**: the default callback returns `true` only if the message starts with `"WARNING:"`. Callers usually `if (!_pdfioFileError(...)) return false;` so a `false` return aborts the operation.
- Some errors are unrecoverable (return value ignored): OOM, bad header, recursive xref, too many xrefs/streams, missing Root/Pages, unlock failure, etc.
- Fatal errors in `pdfioFileOpen` always `pdfioFileClose` + return NULL.

### 3.2 Tolerance quirks (replication-relevant)
- Header: **only** `%PDF-1.X`/`%PDF-2.X` accepted (not `%PDF-1.0` oddities like `%!PS-Adobe-...`). The binary marker line is *not* validated.
- startxref scan is anchored in the **last 1024 bytes**; files with >1KB of trailing garbage before `startxref` fail → but then **repair** kicks in (only if the error callback allows).
- Missing/invalid `startxref`, malformed xref entry (bad EOL per above, missing type space, bad generation), corrupt trailer dict → all trigger **repair_xref** rather than hard failure.
- Repair = brute-force full scan; it *loses* lazy-loading guarantees (it eagerly reads every object) and it does **not** preserve `Prev` chains (it uses the last trailer found, or a synthetic one from the Catalog).
- xref-stream quirks tolerated: 3/4-byte generation fields (clamped), `W[0]==0` (no type field → all in-use), no `Index` (default full range), `Index` with count 999999999 when omitted.
- Empty hex string `<>` → empty literal string.
- Duplicate dictionary keys → first wins + warning.
- Strings with embedded NULs → promoted to binary.
- Nested parentheses in literal strings tracked; CR/LF inside strings kept; `\r\n` in strings... (kept as-is; tokenizer doesn't normalize EOL inside strings).
- Literal-string line continuation `\<EOL>` is *not* specially implemented (the escape table ignores the backslash and the newline is kept) — a known simplification vs. spec.
- `strtol`/`strtod` are locale-shimmed (`_pdfio_strtod`, `_pdfio_vsnprintf`) so numbers/formatting don't depend on the C locale.
- Timezone via `timegm`/`mktime` + tm_gmtoff adjustments; DST quirks documented in code (Lord Howe comment) — for exact replication, prefer `timegm`.
- Page tree: loops bounded by depth 32; missing `Type` warned; page-leaf detection is "no Kids".
- Object stream: generation forced to 0; values loaded eagerly and stored in memory.
- LZW: EarlyChange default **1** (spec default); rejects EarlyChange > 100; code sizes grow to 12 bits; stack overflow guarded (8192-entry stack).
- Predictors: PNG row filter byte honored; accepts both `0-4` and `10-14` (buggy writers); TIFF predictor 2 only with full-row reads.
- Streams with missing `Length` fail (no endstream scanning).
- Encrypted files: RC4-40 read-only; RC4-128 & AES-128 read/write; AES-256 **not supported** (open fails); V4 requires equal StmF/StrF with CFM V2|AESV2; `/Encrypt` as inline dict in trailer tolerated.
- Only one stream open at a time per file (`current_obj` guard).
- PDF files using **Linearization** are not specially handled (read as normal); incremental updates are handled through the `Prev` chain (last xref wins, per-object generation/recency rules).

### 3.3 Common error messages (greppable)
`Bad header`, `Unable to open file`, `Unable to read startxref data`, `WARNING: Unable to find start of cross-reference table`, `WARNING: Cross-reference is damaged`, `Recursive xref table`, `Too many xref tables`, `Missing Root object`, `Missing Pages object`, `Unable to unlock PDF file`, `Missing or bad file ID`, `Unsupported security handler`, `Unsupported encryption V%d R%d`, `Unable to seek to object`, `Bad header for object %lu`, `Unable to read value for object`, `Early end-of-file for object`, `No stream data`, `Unable to decompress stream data`, `Unsupported stream filter`, `Unsupported compound stream filter`, `Bad PNG filter %d`, `Read buffer too small for stream`, `Invalid ASCII85Decode character`, `Invalid ASCII85Decode sequence`, `String token too large`, `Unterminated string literal`, `Syntax error`, `Invalid dictionary contents`, `Missing value for dictionary key`, `Unexpected token`, `Value too deep`, `Depth of pages objects too great to load`, `Unable to open compressed object stream`, `Too many compressed objects`.

---

## 4. PDF version differences handled by the reader

- **Header**: `%PDF-1.0`…`%PDF-1.7` and `%PDF-2.0` accepted; version string preserved. (1.0/1.1/1.2 are legal inputs; nothing in the reader rejects low versions.)
- **Cross-reference format**: both classic `xref` tables (PDF ≤1.4 era, still used) and **cross-reference streams** (PDF 1.5+) are auto-detected by content — no version gating in load_xref. Cross-reference streams are only *written* for version ≥ 1.5.
- **Object streams** (`/Type /ObjStm`, PDF 1.5+): auto-detected from xref type-2 entries or repair; loaded eagerly.
- **Encryption** maps to versions: RC4-40 (PDF 1.3 era), RC4-128 (PDF 1.4), AES-128 (PDF 1.6 era, V4/R4), AES-256 (PDF 2.0, V6/R6 — read *not implemented*).
- **Compression filters** by era: ASCIIHex/ASCII85/RunLength/CCITT/JBIG2/JPX are declared in the public enum as reading-only but `_pdfioStreamOpen` only implements Flate, LZW, and ASCII85+other; others error out (`Unsupported stream filter`). (The content parser in pdfio-content.c may reference image filters; for replicating the core reader, Flate+LZW+ASCII85(+predictor) suffice.)
- **PDF 2.0 quirk in writer**: when writing version 2.0, deprecated Info keys (Author/Creator/Keywords/Producer/Subject/Title/Trapped) are removed from the Info dict and XMP metadata is written instead (see write_metadata).
- **PDF/A profiles**: reading doesn't special-case them; writing adds `pdfaid:part/conformance` XMP and, for PDF/A-1a, a default OutputIntent for CMYK (GTS_PDFA1/CGATS001).

---

## 5. Writing (generating) PDF files — complete specification

### 5.1 Entry points and file creation

All three create APIs funnel into `create_common(filename, fd, output_cb, output_cbdata, version, media_box, crop_box, error_cb, error_cbdata)` (pdfio-file.c):

- `pdfioFileCreate(filename, version, media_box, crop_box, error_cb, error_cbdata)`
  - `open(filename, O_WRONLY|O_BINARY|O_CREAT|O_TRUNC, 0666)`. Failure → `"Unable to create '%s': %s"`, returns NULL. NULL filename → NULL (range check).
  - If `create_common` fails → `unlink(filename)` (no partial file left behind).
- `pdfioFileCreateOutput(output_cb, output_ctx, ...)` — streamed output, `fd = -1`, filename `"output.pdf"` (used only in error messages). No seeking, everything written sequentially via the callback.
- `pdfioFileCreateTemporary(buffer, bufsize, ...)` — requires `bufsize >= 32`; picks temp dir: `%TEMP%`/`GetTempPathA` (Windows), `$TMPDIR` or `_CS_DARWIN_USER_TEMP_DIR`/`/private/tmp` (macOS), `$TMPDIR` or `/tmp` (others); generates random 32-bit `%08x.pdf` names, up to 1000 attempts with `O_EXCL`; on failure of create_common also `unlink` + clears buffer.
- All create functions require `filename` and either `fd >= 0` or `output_cb`; else NULL. NULL `error_cb` → `_pdfioFileDefaultError` (prints to stderr, only WARNING: continues).

**Version/profile mapping** (what `pdf->version`, `pdf->profile` and the header become):

| requested string | header | profile | notes |
|---|---|---|---|
| `1.3` … `1.7`, `2.0` | `%PDF-<v>` | none | generic |
| `PCLm-1.0` | `%PDF-1.4\n%PCLm-1.0\n` | PCLm | raster PDF for printers |
| `PDF/A-1a` / `1b` | `%PDF-1.4` | PDFA_1A / 1B | |
| `PDF/A-2a/2b/2u` | `%PDF-1.7` | 2A/2B/2U | |
| `PDF/A-3a/3b/3u` | `%PDF-1.7` | 3A/3B/3U | |
| `PDF/A-4` | `%PDF-2.0` | PDFA_4 | |
| `NULL` | `%PDF-2.0` | none | default is **2.0** |
| anything else | `%PDF-<string>` | none | passed through verbatim |

Header bytes: `"%PDF-" + version + "\n%" + 0xE2 0xE3 0xCF 0xD3 + "\n"` (PCLm adds a second comment line with the PCLm version string). No consistency check is performed between the requested string and the file-version capabilities.

Other initialization in `create_common`:
- `permissions = PDFIO_PERMISSION_ALL`, mode = WRITE, locale data captured (`get_lconv`); default MediaBox/CropBox = "universal" `[0 0 595.275591 792]` (x2 = 210.0*72/25.4, y2 = 11*72), copied from the passed rects if provided.
- Creates object **1 = Pages** (`/Type /Pages`), **2 = Info** (`/CreationDate <now>`, `/Producer pdfio/1.7.0`), **3 = Root/Catalog** (`/Type /Catalog`, `/Pages 1 0 R`).
- For version ≥ `"1.5"` **and** `!output_cb`: pre-creates the (empty-for-now) `/Type /ObjStm` dictionary (`pdf->objstm_dict`). With an output callback object streams are never used.
- ID: two identical random 16-byte binary strings (`_pdfioCryptoMakeRandom`); `file_id` = SHA-256(filename ∥ time_t(now) ∥ id ∥ id) used for object-map identity when copying from other files.
- Nothing is written to disk at creation except the header — objects accumulate in memory (dicts/arrays/objects/strings are owned by the pdfio_file_t and freed on close).

### 5.2 Durable value model (what gets written)

All values are stored in `_pdfio_value_t` and serialized by `_pdfioValueWrite`/`_pdfioDictWrite`/`_pdfioArrayWrite` through a `printf`-style callback (`_pdfio_printf_t` = `_pdfioFilePrintf` for objects, `_pdfioStringPrintf` for object-stream buffering):

- array → `[ ... ]`
- binary → `<hex>` (odd final nibble padded with 0; encrypted if `obj->pdf->encryption`)
- boolean → ` true` / ` false`
- date → `(D:YYYYMMDDhhmmssZ)` (converted with `gmtime_r`; encrypted as hex string if encryption)
- dict → `<< /key value ... >>` writing **exactly** the pairs stored, in sorted-by-key order, with `%N` name escaping
- indirect → ` N G R`
- name → `%N` (leading `/`, characters <0x21, >0x7e, and `#` as `#XX`)
- null → ` null`
- number → ` %.6f` (always 6 decimals — this is the source of the "%.6f" output format used everywhere; integers and reals are identical on output)
- string → `%S` (`(`...`)` escaping `\`, `(`, `)`, and control chars as `\NNN` octal; encrypted as hex string if encryption)
- The `%H`, `%N`, `%S`, `%n` formats are implemented in `_pdfio_vsnprintf` (locale-independent; floats always use `.` as decimal separator, trailing zeros stripped for `%f`-family formatting).

Dictionary write also implements the `/Length` placeholder logic (see §5.4). Numbers written as `%.6f` are 6-decimal fixed — important for exact replication of byte output.

### 5.3 Object creation & writing

- `pdfioFileCreateObj(pdf, dict)` / `_pdfioFileCreateObj(pdf, srcpdf, value)`: requires WRITE mode; `calloc` object; append to `pdf->objs`; `number = pdf->num_objs` (1-based, sequential allocation order — *not* sorted; object numbers never reused). `_pdfioValueCopy` deep-copies the value (cross-file: dicts/arrays re-created, indirect refs resolved through the object map so shared objects are only copied once).
- Convenience wrappers: `pdfioFileCreateArrayObj`, `pdfioFileCreateNameObj`, `pdfioFileCreateNumberObj`, `pdfioFileCreateStringObj` (worth noting: string object just stores the pointer — caller's string must stay valid until close, or use `pdfioStringCreate`).
- `pdfioObjClose(obj)`:
  - Write mode only; clears `current_obj`.
  - **Object-stream path** (see §5.5): if `objstm_dict` exists, no `objstm_obj` yet, object ≠ encrypt object, `num_objstm < 0x10000`, and the value is a dict or array → serialize the value into `objstm_data` (max buffer 131072+32), assign it an object stream index, return. (Object numbers are then allocated to all remaining objects *around* the stream object.)
  - **Regular path** `_pdfioObjWriteHeader(obj)`: `offset = tell()` then writes `"<number> <generation> obj\n"` + serialized value + `"\n"` via `_pdfioValueWrite` (which captures `length_offset` for `/Length` placeholders); then `"endobj\n"`. For a stream object, `pdfioObjClose` instead closes the stream (see below).
- `pdfioObjCreateStream(obj, PDFIO_FILTER_NONE|FLATE)`:
  - Validates: write mode, value is dict, `obj->offset == 0` (not already written — else "Object has already been written."), filter is `NONE` or `FLATE` (else "Unsupported filter value for PDFioObjCreateStream."), no other object currently open ("Another object (%u) is already open.").
  - Adds `/Length` if missing: **output-callback mode** → creates an indirect length object (new object holding number `0`) and sets `/Length` to that reference (streams on non-seekable output need no seek-patching); **file mode** → `pdfioDictSetNumber("/Length", 0.0)` with the placeholder patched later (see `_pdfioDictWrite` below).
  - Writes header (`N G obj\n` + dict + `\n`), then `"stream\n"`, records `obj->stream_offset = tell()`, creates the stream (`_pdfioStreamCreate`), sets `current_obj`.

  Note: `pdfioObjCreateStream` itself does *not* write the `0000000000`-style placeholder literally; the `/Length` placeholder is written as the literal text `9999999999` **at the moment the dictionary is serialized** when the stored numeric Length is ≤ 0 (`_pdfioDictWrite`): `*length = tell() + 1; cb(" 9999999999")`. `pdfioStreamClose` then patches that exact 10-character field with `%-10lu` after seeking back.

- `pdfioFileCreatePage(pdf, dict)` (pdfio-file.c): copies `dict` (or creates empty), fills missing `CropBox`/`MediaBox` from file defaults, sets `/Parent` → pages object, adds `DefaultGray/DefaultRGB/DefaultCMYK` color spaces to `/Resources` (sRGB / sRGB / CGATS001 ICC), forces `/Type /Page` if absent, creates the page object (closed immediately), appends to `pdf->pages`, creates the Contents object (dict with `/Filter /FlateDecode` unless DEBUG) and returns a **contents stream** to write into. NOTE: the page is written to the file at `pdfioObjClose` time inside this function; content operators are written afterward into the contents stream.
- `pdfioPageCopy(pdf, srcpage)`: walks `srcpage` + `Parent` chain (depth ≤ 32), copies non-parent keys (skips `Count`/`Kids`; parent-owned keys only fill in missing ones), replaces Parent with the destination pages object, forces `/Type /Page` and default MediaBox, creates the page and adds it. Used for merging/filtering documents.

### 5.4 Stream writing (`pdfioStreamWrite` / `pdfioStreamClose`)

- `pdfioStreamWrite(st, buffer, bytes)`:
  - `PDFIO_FILTER_NONE`: if crypto → encrypt through the stream buffer in 16-byte-aligned blocks (AES requires 16-byte multiples; bytes < 16 buffered until enough or final); else raw `_pdfioFileWrite`.
  - `PDFIO_FILTER_FLATE`: if predictor == NONE → `stream_write` (zlib `deflate` with `Z_NO_FLUSH`; flush when output buffer below `cbsize/8`; AES block-aligned encrypt on flush). If predictor set: **caller must pass whole rows** (`bytes % (pbsize-1) == 0`, else error "Write buffer size must be a multiple of a complete row.") — then encode each row with the configured PNG predictor (byte 0 = filter type `predictor-10`, PNG_AUTO=4/Paeth) maintaining the previous-row buffer, and write.
  - Predictor parameters come from `/DecodeParms`: `BitsPerComponent` (default 8; valid 1,2,4,8,16), `Colors` (default 1; ≤32), `Columns` (default 1; ≤65536), `Predictor` (2 = TIFF — read-only; 10–15 = PNG; 15 = Paeth). Invalid values → errors ("Unsupported BitsPerColor value %d.", "Unsupported Colors value %d.", "Unsupported Columns value %d.", "Unsupported Predictor function %d.").
- `pdfioStreamClose(st)`:
  - Flate: loop `deflate(Z_FINISH)` writing output (encrypting through crypto_cb on 16-byte boundaries; final partial block with `last=true` → PKCS#7 pad); `deflateEnd`.
  - No filter + crypto: encrypt remaining buffered bytes with `last=true` (AES adds the final pad block; RC4 just streams).
  - `stream_length = tell() - stream_offset` (this is the **compressed + encrypted** length, always written as an integer).
  - writes `"\nendstream\nendobj\n"`.
  - patches `/Length`: indirect length object → set its number value and close it; direct placeholder → `lseek(length_offset)` + `_pdfioFilePrintf(fmt, "%-10lu", stream_length)` + `lseek(0, SEEK_END)`; failure → error.
  - frees filter/predictor/crypto buffers, clears `current_obj`.
- Page content streams use the same machinery; content operators are written with the `%f` family at 6 decimals and `%N`/`%S` escaping.

### 5.5 Object streams (PDF 1.5+ only, regular files only)

`write_objstm(pdf)` runs at close for versions ≥ `1.5` when `objstm_dict` exists and at least one object was queued:
- Iterate objects in number order; for each queued one (has `objstm_data`) assign `objstm_number = idx++` and write `"<number> <offset>\n"` pairs into a string buffer, accumulating `length` (the concatenated data offset — note: the header offsets are the *byte offsets in the raw stream* relative to `/First`).
- Dict: `/First` = header size, `/N` = count, `/Filter /FlateDecode`.
- Create the ObjStm object (number = next after all queued objects), open a **FLATE** stream (no predictor), write the header pairs then each object's serialized value, close.
- The xref then maps every queued object with type-2 entries pointing at the ObjStm object number + its index. Limits: max 65536 objects per file stream (0x10000 from pdfioObjClose), 16384 per stream on read.
- **Encrypted files**: the encryption object itself is NEVER inside an object stream; also stream-carrying objects (those with `stream_offset`) are never buffered (only dict/array values are).

### 5.6 XMP metadata (`write_metadata` at close)

Creates object `/Type /Metadata` `/Subtype /XML`; writes an XMP packet stream:
- `<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>`
- `<x:xmpmeta xmlns:x="adobe:ns:meta/">` / `<rdf:RDF ...>`
- `xmp:CreateDate`, `xmp:CreatorTool` (if Creator set), `xmp:MetadataDate`, `xmp:ModifyDate` (if set) — ISO 8601 `%04d-%02d-%02dT%02d:%02d:%02dZ`
- `pdf:Producer`, `pdf:Keywords` (if set)
- `dc:format=application/pdf`, `dc:title`/`dc:creator`/`dc:description` (if set) using `<rdf:Alt>...<rdf:li xml:lang="x-default">` / `<rdf:Seq>`
- PDF/A profiles add `<pdfaid:part>` and `<pdfaid:conformance>` from a static table (1A: part 1 conformance A, ..., 4: part 4 no conformance)
- `</rdf:RDF>...` + `<?xpacket end="r"?>`
- Sets catalog `/Metadata` → the metadata object.
- **For version `"2.0"`**: removes the deprecated Info keys `Author`, `Creator`, `Keywords`, `Producer`, `Subject`, `Title`, `Trapped` from the Info dict (XMP is the replacement).

### 5.7 Finalization sequence (`pdfioFileClose`, WRITE mode)

1. If the file's profile is a PDF/A profile, add the default CMYK OutputIntent for PDF/A printing: `pdfioFileAddOutputIntent(pdf, "GTS_PDFA1", "CMYK", "CGATS001", NULL, "CMYK Printing", NULL)`. (Note: the call site in `pdfioFileClose` is unconditional, but pdfioFileAddOutputIntent itself only stores the intent in the catalog's `/OutputIntents` array, deduplicated by subtype — for non-PDF/A profiles none of the file's own code consults it further; the authoritative check is that the call happens for every write-mode close.)
2. `write_metadata(pdf)` — see §5.6 (fails → close fails).
3. `pdfioObjClose(pdf->info_obj)` — writes Info object (or into object stream).
4. `write_objstm(pdf)` (if applicable).
5. `write_pages(pdf)` — builds `/Kids` array of page refs, sets `/Count`, closes Pages object.
6. `pdfioObjClose(pdf->root_obj)` — writes catalog.
7. `write_trailer(pdf)` — xref + trailer + EOF marker.
8. `_pdfioFileFlush(pdf)`; `close(fd)`.
9. Free all memory pools: arrays, dicts, objects (with extension free callbacks), objmaps, pages, strings, string buffers, TTF cache. If *any* step failed → `pdfioFileClose` returns `false` (and the file data may be incomplete/truncated — disk close still happens).

### 5.8 Trailer & xref output — exact format

**Cross-reference stream** (version ≥ `"1.5"`, regular file mode only):
- object dict: `/Type /XRef`, `/Size <num_objs+2>`, `/W [1 <offsize> <idxsize>]`, `/Filter /FlateDecode`, `/Info`, `/Root`, `/Encrypt` (if any), `/ID` (if any).
- `offsize` = 1..8, minimal bytes for `max(xref_offset, num_objs)`; `idxsize` = 1 if `num_objstm ≤ 0x100`, else 2. (These are computed from byte-position thresholds <0xff, <0xffff, <0xffffff, <0xffffffff, <0xffffffffff, <0xffffffffffff, <0xffffffffffffff, else 8.)
- Stream content (uncompressed rows, then Flate): entry 0 = type 0 (`0000000000`-style free, all zeros including generation); entry per object: type 1 + big-endian `offsize`-byte offset (regular objects) **or** type 2 + big-endian ObjStm object number + big-endian `idxsize`-byte index (queued objects).
- **Encryption is momentarily disabled** while writing this stream (`pdf->encryption = NONE`, restored afterward) — xref streams are never encrypted (matches the reader's exemption).
- There is NO `Prev` or `StartXRef` inside; only one xref stream is written.

**Classic xref table** (versions < 1.5, or any version when using `pdfioFileCreateOutput`):
```
xref
0 <num_objs+1> 
0000000000 65535 f 
<10-digit offset> <5-digit gen> n 
...
trailer
<< ... >>

startxref
<offset>
%%EOF
```
- The `%%EOF` marker is written after `\nstartxref\n<offset>\n` (no trailing newline after `%%EOF`).
- Trailer keys: `/Encrypt` (if any), `/ID` (if any), `/Info`, `/Root`, `/Size`.

### 5.9 Encryption on write (`pdfioFileSetPermissions` → `_pdfioCryptoLock`)

- `pdfioFileSetPermissions(pdf, permissions, encryption, owner_password, user_password)`:
  - `encryption == NONE` → returns true, no-op (permissions recording only).
  - PDF/A profile (any `_PDFIO_PROFILE_PDFA_*`) + encryption → error "Encryption is not allowed for PDF/A files." and false.
  - If `pdf->num_objs > 3` (i.e., more than the Pages+Info+Root created at start) → error "You must call pdfioFileSetPermissions before adding any objects." — **must be called before any other object/page is created**.
- `_pdfioCryptoLock` supports **only** RC4-128 (V=2/R=3) and AES-128 (V=4/R=4): builds the Encrypt dict (`/Filter /Standard`, `/Length 128`, `/O` 32 bytes, `/P` permissions int, `/R`, `/V`, `/U` 32 bytes; AES adds `/CF << /StdCF << /Type /CryptFilter /CFM /AESV2 >> >>`, `/StmF /StdCF`, `/StrF /StdCF`, `/EncryptMetadata true`).
  - Key derivation is the *inverse* of reading: `make_password_key` (md5, 50 rounds for 128-bit), `make_owner_key` (RC4-encrypt padded user password with owner key, 20 rounds), `make_file_key` (md5(Upad∥O∥P4∥ID[∥ffffffff]), 50 rounds), `make_user_key` (md5(passpad∥ID)∥0[16]) then RC4-encrypted 20 rounds with file key.
  - If `owner_password == NULL && user_password != NULL && *user_password != 0` → random 32-byte owner password (PDF-spec behavior: owner = random).
  - Encrypt object is created/closed like a normal object (it is object #4 if permissions set before any other objects) and referenced from the trailer/xref via `/Encrypt`.
  - After lock: every stream and string/binary value is encrypted on write with the per-object key (file_key ∥ objnum(3 LE) ∥ gen(2 LE) [∥ sAlT]); AES-128 CBC with a fresh random 16-byte IV written before the ciphertext and PKCS#7 padding; RC4-128 streams statefully (no IV).
  - RC4-40 (`PDFIO_ENCRYPTION_RC4_40`) write → falls through to default → error "Encryption mode %d not supported for writing."; AES-256 write → same error (TODO).

### 5.10 Content-stream and resource generation (higher level, `pdfio-content.c`)

Summary of the generator used when writing pages — all operators emitted with `%.6f` numbers and one operator per line:
- path: `m`/`l`/`c`/`v`/`y`/`re`/`h`, `W`/`W*` clip, `S`/`s`/`f`/`f*`/`B`/`B*`/`b`/`b*` stroke/fill
- graphics state: `q`/`Q` save/restore, `cm` (matrix, rotate/scale/translate shortcuts), `w`, `J` (cap), `j` (join), `M`, `i`, `d` dash
- color: `g`/`G`, `rg`/`RG`, `k`/`K` (device), `sc`/`SC`/`cs`/`CS` (named/calibrated via `%N`)
- text: `BT`/`ET`, `Tf` (font + size), `Tm`, `Td`/`TD`, `T*`, `Tj`, `'`, `"`, `TJ` (justified arrays of strings and offsets), `Tc`, `Tw`, `Tz`, `TL`, `Ts`, `Tr`
- `write_string()`: `unicode=false` → literal `(string)` with CP1252-ish byte mapping; `unicode=true` → hex string `<....>` with UTF-16BE code units (surrogate pairs for >FFFF); auto `T*` before `Tj` when a `\n` ends the string
- marked content: `BMC`/`BDC` (with property dict) /`EMC`; sets `/MarkInfo /Marked true` in the catalog
- images: `q w 0 0 h x y cm /Name Do Q`
- fonts: base-14 (`pdfioFileCreateFontObjFromBase`, Type1 + WinAnsiEncoding object (shared, object created once) — **not allowed in PDF/A** (error), Symbol/ZapfDingbats get no encoding), embedded TTF/OTF (`pdfioFileCreateFontObjFromFile/Data/System`): font file object (FlateDecode stream), font descriptor (`FontName`, `FontFile2`, `Flags` 0x21/0x20, `FontBBox`, `ItalicAngle`, `Ascent`, `Descent`, `CapHeight`, `XHeight`, `StemV = weight/4+25`); simple TrueType font (FirstChar 32..LastChar, Widths array with CP1252 mapping for 0x80–0x9F) or Unicode CID font (Type0/CIDFontType2 with CIDToGIDMap stream, ToUnicode CMap `Adobe-Identity-UCS`, `W` widths array with run-length encoding [c0 c1 w] or [c0 [w...]], `/Encoding /Identity-H`)
- ICC: `/N <n>` + Flate stream (from file or data, 1/3/4 colors)
- color arrays: ICCBased, CalGray/CalRGB (Gamma/WhitePoint/Matrix; from primaries by solving CIE XYZ equations — degenerate cases return NULL), Indexed (DeviceRGB + palette binary)
- images from files: JPEG → `DCTDecode` passthrough (reads APP2 ICC chunks, SOFn dimensions; 8-bit only; `Unsupported` errors otherwise, incl. 12-bit), PNG → decoded to raw pixels with libpng (or manual chunk scanner fallback) then re-encoded Flate + PNG predictor (`/Predictor 15`), GIF → LZW-decoded (internal GIF LZW, code size 2-12, reversed bit order), WebP (if libwebp) → decoded RGBA then Flate; alpha → SMask (second image object, DeviceGray, same predictor); PDF/A-1 rejects alpha (error), dimensions capped at 16384×16384
- page resources: `pdfioPageDictAddFont/AddImage/AddColorSpace` create `/Resources << /Font|/XObject|/ColorSpace >>` sub-dicts on demand; adding a duplicate name returns false
- **PDF/A restrictions enforced**: base fonts and alpha images rejected; XMP carries `pdfaid` info; OutputIntent always added at close (GTS_PDFA1/CMYK/CGATS001).

### 5.11 Writing edge cases & errors
- NULL args: most create/set functions return NULL/false on NULL pdf; setters (`pdfioFileSetTitle` etc.) silently do nothing if `info_obj` missing.
- `pdfioDictSet*` replace existing values; binary pair memory freed on replace; dictionary keys sorted alphabetically on output (output order is deterministic).
- `pdfioStringCreate` dedupes strings (sorted array) — repeated values cost nothing; strings must live until close.
- `pdfioObjClose` on a stream-bearing object that has an open stream → first call closes the stream (must be paired); idempotent afterwards.
- Only ONE stream open at a time per file (current_obj invariants) — calling `pdfioObjCreateStream`/`pdfioObjOpenStream` while another is open fails with "Another object (%u) is already open."
- Object numbers are never freed/reused; `pdfioFileGetNumObjs` returns the count as allocated; there is no incremental-update mode (single xref written at close; no `Prev` chain on output).
- `pdfioFileCreateOutput` + version < 1.5 → classic xref (no xref stream possible without seeking); object stream disabled for output callbacks (no seeking for the `9999999999` patch — instead an indirect length object is used).
- Empty pages file: `write_pages` still writes `/Count 0` and an empty `/Kids` array, and `pdfioFileClose` permits closing a file with no pages (no validation is performed).
- **Failed writes**: any NULL/false from stream/object close propagates to `pdfioFileClose` returning false; the caller can't retry (file already partially written); `pdfioFileCreate` unlinks the file only if `create_common` fails, not if a later write fails.
- String/name escaping on write: names escape `#` and all bytes <0x21 or >0x7e as `#XX`; strings escape `\ ( )` and control bytes as octal `\NNN`; `%H` escapes `&<>` for XML content.
- Locale: all number formatting goes through `_pdfio_vsnprintf` so output always uses `.` decimals regardless of locale; numbers always formatted `%.6f`.
- Unsupported write filters (`ASCII85`, `LZW`, `DCT`… in `pdfioObjCreateStream`) → hard error; arbitrary custom filters can be simulated by setting `/Filter` in the dict yourself before creating the stream with PDFIO_FILTER_NONE.

---

## 6. Replication checklist (condensed core)

To build a functionally equivalent reader:

1. Buffered byte I/O with peek/consume/seek/tell semantics; 8192-byte buffer; SEEK_CUR folded into absolute via `bufpos`.
2. Open: read first line, require `%PDF-1.` or `%PDF-2.` + digit; read last 1024 bytes; locate `startxref`; parse `strtol`.
3. xref loader with dual mode (table/stream) + `Prev` chain (≤100, no recursion) + trailer `ID`, `Encrypt`, `Root`, `Info`, `Pages`.
4. Repair fallback: full scan for `^\s*[1-9][0-9]*\s+[0-9]+\s+obj`, tokenize values, find trailer or synthesize from Catalog; missing Root/Pages = hard fail.
5. Standard security handler unlock (V2R2, V2R3, V4R4; V6R6 unsupported), password callback with ≤4 attempts, all key derivations as in §2.4 (note exact padding bytes, P as unsigned-32→signed, EncryptMetadata, 50×/20× RC4 loops, per-object key with 3-byte object number + 2-byte generation + `sAlT`, AES-CBC IV prefix, PKCS#7 unpad, RC4-40 10-byte digest key).
6. Tokenizer: the full state machine from §2.5 (nested parens, octal escapes, UTF-16 detection, NUL→binary, `<>`→`()`, `#XX` names, 2048-byte comment/whitespace cap, 128KB token cap).
7. Value/dict/array parser with depth 32, duplicate-key discard, date detection (`D:...Z|±HH'mm'`), `N G R` indirect lookahead, locale-safe number parse.
8. Lazy object loading with exact `N G obj` header verification, stream offset capture, value decryption.
9. Object streams: parse `N` pairs then N values; generation 0; eager in-memory.
10. Stream layer: length (direct or indirect), per-object decrypt, Flate (zlib), LZW (8-bit MSB-first, EarlyChange default 1), ASCII85 compound (z, `~>`, 5→4 with `u` padding), TIFF predictor 2, PNG predictors 0–4/10–14 with per-row filter bytes, whole-row reads enforced; one open stream at a time.
11. Page tree recursion (`Kids` arrays, depth ≤32, Type tolerant), page accessor inheritance through `Parent`, `Contents` indirect or array-of-indirect resolution.
12. Error callback honoring "WARNING:" ⇒ continue; otherwise abort; memory ownership pools (strings deduped, arrays/dicts owned by file, object placeholders).
13. On versions: accept any 1.x/2.0 header; support both xref styles and object streams independent of declared version; keep the declared version string for the API.

To build a functionally equivalent **writer**, additionally:

1. Create: header (`%PDF-<v>\n%<binary comment>\n`, PCLm variant), Pages/Info/Root objects as 1/2/3, optional ObjStm dict (v≥1.5 + file mode), random 16-byte ID ×2, SHA-256 file_id.
2. Value serializer with exact escaping: names `#XX` for <0x21/>0x7e/#; strings `(`...`)` with `\\ ( )` and `\NNN` octal; binary `<hex>`; numbers `%.6f`; dates `(D:YYYYMMDDhhmmssZ)`; dicts sorted by key; indirects `N G R`.
3. Object numbering: sequential allocation, never reused; object streams for dict/array values (≤65536, not the encrypt object) with `N G F` header (offsets relative to `/First`), Flate; type-2 xref entries.
4. Streams: `/Length 0` placeholder → serialized as literal `9999999999` and patched via seek (`%-10lu`) at close; or indirect length object in callback mode; `stream\n` … `\nendstream\nendobj\n`; Flate level 9 with 4096-byte output; PNG predictor encoding (filter byte per row, Paeth for auto); whole-row writes enforced.
5. Finalization order: OutputIntent (PDF/A CMYK), XMP metadata (and Info-key removal for 2.0), close Info, object stream, Pages (Count+Kids), close catalog, xref+trailer+`startxref`+`%%EOF`, flush, close fd.
6. Xref stream (v≥1.5, file mode only): `/W [1 offsize idxsize]`, types 0/1/2, Flate, unencrypted; else classic table: `xref\n0 <count> \n`, free entry `0000000000 65535 f \n`, then 20-byte records `%010lu %05u n \n` per object, `trailer\n<<...>>` with `/Size /Root /Info /ID /Encrypt`.`
7. Encryption write: RC4-128 (V2/R3) and AES-128 (V4/R4) only; key derivation mirrored from reader; per-object key + optional `sAlT`; AES random IV prefix + PKCS#7; must be called before creating objects (num_objs ≤ 3); forbidden for PDF/A.
8. Content layer (if needed): base-14 fonts (Type1 + shared WinAnsi Encoding, banned in PDF/A), embedded TrueType (descriptor + FontFile2 + CID or simple widths), ICC (`/N` 1/3/4 + Flate), color-space arrays, image objects (DCT passthrough for JPEG, Flate+PNG-predictor for raster, SMask for alpha), content operators with `%.6f` formatting, `/Resources` sub-dicts created on demand.

*(Verification note: the interior of `pdfio-content.c` (content operators, fonts, images, color spaces) was inspected at the level needed for the summary in §5.10; deeper TTF metrics / cmap generation and the `ttf.*` helper library internals were not fully deep-dived. `pdfio-cgats001-compat.h`/`pdfio-base-font-widths.h` support the higher-level content API only.)*
