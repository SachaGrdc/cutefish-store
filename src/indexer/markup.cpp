#include "markup.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <memory>

namespace cutefish::store::indexer {

namespace {

bool isSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string nodeText(xmlNode* node)
{
    xmlChar* content = xmlNodeGetContent(node);
    std::string text = content ? normalizeSpace(reinterpret_cast<const char*>(content)) : std::string();
    xmlFree(content);
    return text;
}

bool isElement(xmlNode* node, const char* name)
{
    return node->type == XML_ELEMENT_NODE && xmlStrcmp(node->name, reinterpret_cast<const xmlChar*>(name)) == 0;
}

void add(std::vector<DescriptionBlock>& blocks, DescriptionBlockType type, std::string text)
{
    if (!text.empty())
        blocks.push_back({type, std::move(text)});
}

std::string stripTags(std::string_view markup)
{
    std::string out;
    bool inTag = false;
    for (const char c : markup) {
        if (c == '<')
            inTag = true;
        else if (c == '>')
            inTag = false;
        else if (!inTag)
            out += c;
    }
    return normalizeSpace(out);
}

} // namespace

std::string normalizeSpace(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    for (const char c : text) {
        if (isSpace(c)) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace)
            out += ' ';
        pendingSpace = false;
        out += c;
    }
    return out;
}

std::vector<DescriptionBlock> descriptionBlocks(std::string_view markup)
{
    std::vector<DescriptionBlock> blocks;
    if (markup.find_first_not_of(" \t\r\n") == std::string_view::npos)
        return blocks;
    std::string wrapped;
    wrapped.reserve(markup.size() + 7);
    wrapped.append("<d>").append(markup).append("</d>");
    const std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(
            xmlReadMemory(wrapped.data(), static_cast<int>(wrapped.size()), nullptr, "UTF-8",
                          XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING),
            &xmlFreeDoc);
    xmlNode* root = doc ? xmlDocGetRootElement(doc.get()) : nullptr;
    if (!root) {
        add(blocks, DescriptionBlockType::Paragraph, stripTags(markup));
        return blocks;
    }
    for (xmlNode* node = root->children; node; node = node->next) {
        if (isElement(node, "ul") || isElement(node, "ol")) {
            const auto type = isElement(node, "ol") ? DescriptionBlockType::OrderedListItem : DescriptionBlockType::ListItem;
            for (xmlNode* item = node->children; item; item = item->next)
                if (item->type == XML_ELEMENT_NODE)
                    add(blocks, type, nodeText(item));
        } else if (node->type == XML_ELEMENT_NODE || node->type == XML_TEXT_NODE || node->type == XML_CDATA_SECTION_NODE) {
            add(blocks, DescriptionBlockType::Paragraph, nodeText(node));
        }
    }
    return blocks;
}

} // namespace cutefish::store::indexer
