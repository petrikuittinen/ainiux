# Rejected DOCX fixtures

Every `.docx` in this directory must fail conversion without returning partial
Markdown. The standard-library generator in the parent directory creates cases
for missing OPC parts, traversal and duplicate member names, unsupported
compression, encryption flags, malformed/deep XML, bad CRC, malformed Zip64,
zip-bomb metadata, and truncation. They remain compact so adversarial tests do
not allocate attacker-declared sizes.
