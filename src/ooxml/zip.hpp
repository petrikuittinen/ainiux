#pragma once

// The OOXML ZIP implementation originated in the DOCX converter.  Keep this
// compatibility facade small while DOCX, XLSX, and PPTX share the same bounded
// archive reader and deterministic writer.
#include "docx/zip.hpp"

namespace ainiux::ooxml {

using ZipLimits = docx::detail::ZipLimits;
using ZipArchive = docx::detail::ZipArchive;
using ZipWriteEntry = docx::detail::ZipWriteEntry;
using docx::detail::resolve_part_name;
using docx::detail::safe_part_name;
using docx::detail::write_zip;

}  // namespace ainiux::ooxml
