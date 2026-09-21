<?php
declare(strict_types=1);

if ($argc < 5) {
    fwrite(STDERR, "usage: xlsx_phpspreadsheet_worker.php AUTOLOAD DIRECTION INPUT OUTPUT\n");
    exit(2);
}

$autoload = $argv[1];
$direction = $argv[2];
$input = $argv[3];
$output = $argv[4];
if (!is_file($autoload)) {
    fwrite(STDERR, "missing PhpSpreadsheet autoload: $autoload\n");
    exit(2);
}
require $autoload;

use PhpOffice\PhpSpreadsheet\Cell\Coordinate;
use PhpOffice\PhpSpreadsheet\Reader\Xlsx as XlsxReader;
use PhpOffice\PhpSpreadsheet\RichText\RichText;
use PhpOffice\PhpSpreadsheet\Spreadsheet;
use PhpOffice\PhpSpreadsheet\Writer\Xlsx as XlsxWriter;

function escape_gfm(string $text): string
{
    $text = str_replace(["\r\n", "\r"], "\n", $text);
    return str_replace(["|", "\n"], ["\\|", "<br>"], $text);
}

function cell_string(mixed $value): string
{
    if ($value === null) {
        return "";
    }
    if ($value instanceof RichText) {
        return $value->getPlainText();
    }
    if (is_bool($value)) {
        return $value ? "TRUE" : "FALSE";
    }
    if ($value instanceof DateTimeInterface) {
        return $value->format("Y-m-d");
    }
    if (is_int($value) || is_float($value)) {
        if (is_float($value) && is_finite($value) && floor($value) == $value && abs($value) < 1e15) {
            return (string) (int) $value;
        }
        $text = rtrim(rtrim(sprintf("%.15g", $value), "0"), ".");
        return $text === "" || $text === "-" ? "0" : $text;
    }
    return (string) $value;
}

function split_row(string $line): array
{
    $line = trim($line);
    if (str_starts_with($line, "|")) {
        $line = substr($line, 1);
    }
    if (str_ends_with($line, "|")) {
        $line = substr($line, 0, -1);
    }
    $cells = [];
    $current = "";
    $escape = false;
    $length = strlen($line);
    for ($i = 0; $i < $length; $i++) {
        $ch = $line[$i];
        if ($escape) {
            $current .= $ch;
            $escape = false;
            continue;
        }
        if ($ch === "\\") {
            $escape = true;
            continue;
        }
        if ($ch === "|") {
            $cells[] = trim(str_replace("<br>", "\n", $current));
            $current = "";
            continue;
        }
        $current .= $ch;
    }
    $cells[] = trim(str_replace("<br>", "\n", $current));
    return $cells;
}

function is_separator(string $line): bool
{
    $cells = split_row($line);
    if (count($cells) < 1) {
        return false;
    }
    foreach ($cells as $cell) {
        $stripped = str_replace([" ", ":"], "", $cell);
        if ($stripped === "" || strspn($stripped, "-") !== strlen($stripped)) {
            return false;
        }
    }
    return true;
}

function parse_markdown_tables(string $text): array
{
    $lines = preg_split("/\r\n|\n|\r/", $text);
    $tables = [];
    $pending = "Sheet";
    $count = count($lines);
    for ($i = 0; $i < $count; $i++) {
        $stripped = trim($lines[$i]);
        if (str_starts_with($stripped, "#")) {
            $pending = trim(ltrim($stripped, "#")) ?: "Sheet";
            continue;
        }
        if (!str_contains($lines[$i], "|")) {
            continue;
        }
        $header = split_row($lines[$i]);
        if ($i + 1 >= $count || !is_separator($lines[$i + 1])) {
            continue;
        }
        $rows = [$header];
        $i += 2;
        while ($i < $count && str_contains($lines[$i], "|") && trim($lines[$i]) !== "") {
            if (is_separator($lines[$i])) {
                $i++;
                continue;
            }
            $rows[] = split_row($lines[$i]);
            $i++;
        }
        $i--;
        $tables[] = [$pending, $rows];
        $pending = "Sheet";
    }
    return $tables;
}

function emit_table(string $name, array $rows): string
{
    $columns = 0;
    foreach ($rows as $row) {
        $columns = max($columns, count($row));
    }
    if ($columns === 0) {
        return "";
    }
    if ($columns === 1) {
        $columns = 2;
    }
    $out = "# " . $name . "\n\n";
    $emit = static function (array $row) use ($columns): string {
        $line = "|";
        for ($i = 0; $i < $columns; $i++) {
            $line .= " " . ($row[$i] ?? "") . " |";
        }
        return $line . "\n";
    };
    $out .= $emit($rows[0]);
    $out .= "|";
    for ($i = 0; $i < $columns; $i++) {
        $out .= " --- |";
    }
    $out .= "\n";
    $n = count($rows);
    for ($r = 1; $r < $n; $r++) {
        $out .= $emit($rows[$r]);
    }
    return $out . "\n";
}

function xlsx_to_markdown(string $input, string $output): void
{
    $reader = new XlsxReader();
    $reader->setReadDataOnly(true);
    $spreadsheet = $reader->load($input);
    $markdown = "";
    foreach ($spreadsheet->getAllSheets() as $sheet) {
        $highestRow = (int) $sheet->getHighestDataRow();
        $highestColumn = $sheet->getHighestDataColumn();
        $highestCol = Coordinate::columnIndexFromString($highestColumn);
        if ($highestRow < 1 || $highestCol < 1) {
            continue;
        }
        $rows = [];
        for ($r = 1; $r <= $highestRow; $r++) {
            $row = [];
            for ($c = 1; $c <= $highestCol; $c++) {
                $cell = $sheet->getCell(Coordinate::stringFromColumnIndex($c) . $r);
                $value = $cell->getValue();
                if (is_string($value) && $value !== "" && $value[0] === "=") {
                    $cached = $cell->getOldCalculatedValue();
                    if ($cached !== null) {
                        $value = $cached;
                    }
                }
                $row[] = escape_gfm(cell_string($value));
            }
            $rows[] = $row;
        }
        $markdown .= emit_table($sheet->getTitle() !== "" ? $sheet->getTitle() : "Sheet", $rows);
    }
    $spreadsheet->disconnectWorksheets();
    unset($spreadsheet);
    if ($markdown === "") {
        fwrite(STDERR, "PhpSpreadsheet produced empty Markdown\n");
        exit(6);
    }
    if (file_put_contents($output, $markdown) === false) {
        fwrite(STDERR, "could not write Markdown: $output\n");
        exit(5);
    }
}

function markdown_to_xlsx(string $input, string $output): void
{
    $text = file_get_contents($input);
    if ($text === false) {
        fwrite(STDERR, "could not read Markdown: $input\n");
        exit(5);
    }
    $tables = parse_markdown_tables($text);
    if ($tables === []) {
        fwrite(STDERR, "Markdown does not contain a valid table for XLSX output\n");
        exit(2);
    }
    $spreadsheet = new Spreadsheet();
    $spreadsheet->removeSheetByIndex(0);
    foreach ($tables as $index => [$name, $rows]) {
        $sheet = $spreadsheet->createSheet();
        $title = $name !== "" ? $name : ("Sheet" . ($index + 1));
        $sheet->setTitle(mb_substr($title, 0, 31));
        foreach ($rows as $r => $row) {
            foreach ($row as $c => $value) {
                $sheet->setCellValue([$c + 1, $r + 1], $value);
            }
        }
    }
    $writer = new XlsxWriter($spreadsheet);
    $writer->save($output);
    $spreadsheet->disconnectWorksheets();
}

if ($direction === "xlsx_to_md") {
    xlsx_to_markdown($input, $output);
} elseif ($direction === "md_to_xlsx") {
    markdown_to_xlsx($input, $output);
} else {
    fwrite(STDERR, "unknown direction: $direction\n");
    exit(2);
}
