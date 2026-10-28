#include "core/rev/node_kind.h"

namespace heditor {

bool isContainer(NodeKind kind) {
    switch (kind) {
        case NodeKind::Document:
        case NodeKind::Section:
        case NodeKind::Table:
        case NodeKind::TableRow:
        case NodeKind::TableCell:
        case NodeKind::ContentControl:
        case NodeKind::Quote:
        case NodeKind::CodeBlock:
        case NodeKind::ListItem:
        case NodeKind::Link:
        case NodeKind::HeaderFooter:
            return true;
        case NodeKind::Image:
        case NodeKind::Formula:
        case NodeKind::Break:
        case NodeKind::PageBreak:
        case NodeKind::Tab:
        case NodeKind::Field:
        case NodeKind::TextRun:
        case NodeKind::ParagraphMark:
            return false;
    }
    return false;
}

bool isText(NodeKind kind) { return kind == NodeKind::TextRun; }

bool isParagraphMark(NodeKind kind) { return kind == NodeKind::ParagraphMark; }

bool isAtom(NodeKind kind) {
    switch (kind) {
        case NodeKind::Image:
        case NodeKind::Formula:
        case NodeKind::Break:
        case NodeKind::PageBreak:
        case NodeKind::Tab:
        case NodeKind::Field:
            return true;
        case NodeKind::Document:
        case NodeKind::Section:
        case NodeKind::Table:
        case NodeKind::TableRow:
        case NodeKind::TableCell:
        case NodeKind::ContentControl:
        case NodeKind::Quote:
        case NodeKind::CodeBlock:
        case NodeKind::ListItem:
        case NodeKind::Link:
        case NodeKind::HeaderFooter:
        case NodeKind::TextRun:
        case NodeKind::ParagraphMark:
            return false;
    }
    return false;
}

const char* nodeKindName(NodeKind kind) {
    switch (kind) {
        case NodeKind::Document:
            return "Document";
        case NodeKind::Section:
            return "Section";
        case NodeKind::Table:
            return "Table";
        case NodeKind::TableRow:
            return "TableRow";
        case NodeKind::TableCell:
            return "TableCell";
        case NodeKind::ContentControl:
            return "ContentControl";
        case NodeKind::Quote:
            return "Quote";
        case NodeKind::CodeBlock:
            return "CodeBlock";
        case NodeKind::ListItem:
            return "ListItem";
        case NodeKind::Link:
            return "Link";
        case NodeKind::HeaderFooter:
            return "HeaderFooter";
        case NodeKind::Image:
            return "Image";
        case NodeKind::Formula:
            return "Formula";
        case NodeKind::Break:
            return "Break";
        case NodeKind::PageBreak:
            return "PageBreak";
        case NodeKind::Tab:
            return "Tab";
        case NodeKind::Field:
            return "Field";
        case NodeKind::TextRun:
            return "TextRun";
        case NodeKind::ParagraphMark:
            return "ParagraphMark";
    }
    return "（未知）";
}

bool nodeKindFromName(const std::string& name, NodeKind* out) {
    if (out == nullptr) return false;
    const NodeKind all[] = {
        NodeKind::Document,  NodeKind::Section,        NodeKind::Table,         NodeKind::TableRow,
        NodeKind::TableCell, NodeKind::ContentControl, NodeKind::Quote,         NodeKind::CodeBlock,
        NodeKind::ListItem,  NodeKind::Link,           NodeKind::HeaderFooter,  NodeKind::Image,
        NodeKind::Formula,   NodeKind::Break,          NodeKind::PageBreak,     NodeKind::Tab,
        NodeKind::Field,     NodeKind::TextRun,        NodeKind::ParagraphMark,
    };
    for (NodeKind kind : all) {
        if (name == nodeKindName(kind)) {
            *out = kind;
            return true;
        }
    }
    return false;
}

bool acceptsChild(NodeKind parent, NodeKind child) {
    switch (parent) {
        case NodeKind::Document:
            return child == NodeKind::Section;
        case NodeKind::Table:
            return child == NodeKind::TableRow;
        case NodeKind::TableRow:
            return child == NodeKind::TableCell;
        default:
            return true;
    }
}

}  // namespace heditor
