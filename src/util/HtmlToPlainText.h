#pragma once

#include <string>

// Convert an HTML fragment to readable plain text. This intentionally ignores
// styling; block elements become line breaks and HTML entities are decoded.
std::string htmlToPlainText(const std::string& html);

// Decode &lt; &gt; &amp; &#...; etc. without stripping tags. Use when Calibre
// stores a description as entity-encoded HTML inside dc:description.
std::string decodeHtmlEntities(const std::string& input);
