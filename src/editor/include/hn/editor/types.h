#pragma once
namespace hn::editor {
enum class InlineStyle { Bold, Italic, Strike, Code };
enum class BlockStyle { Paragraph, H1, H2, H3, H4, H5, H6, Quote, Code };
enum class ListKind { None, Bullet, Ordered, Check };
enum class Mode { Visual, Source };
} // namespace hn::editor
