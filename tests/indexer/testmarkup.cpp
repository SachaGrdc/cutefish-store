#include "markup.h"

#include <gtest/gtest.h>

using namespace cutefish::store;
using namespace cutefish::store::indexer;

TEST(NormalizeSpace, CollapsesAndTrims)
{
    EXPECT_EQ(normalizeSpace("  Edit\ttext \n  files  "), "Edit text files");
    EXPECT_EQ(normalizeSpace(""), "");
    EXPECT_EQ(normalizeSpace(" \n\t "), "");
    EXPECT_EQ(normalizeSpace("éditeur"), "éditeur");
}

TEST(DescriptionBlocks, ParagraphsAndLists)
{
    const auto blocks = descriptionBlocks("<p>An editor for <em>plain</em> text.</p>\n"
                                          "<ul><li>Fast</li><li> Small </li></ul><ol><li>One</li></ol>");
    ASSERT_EQ(blocks.size(), 4u);
    EXPECT_EQ(blocks[0], (DescriptionBlock{DescriptionBlockType::Paragraph, "An editor for plain text."}));
    EXPECT_EQ(blocks[1], (DescriptionBlock{DescriptionBlockType::ListItem, "Fast"}));
    EXPECT_EQ(blocks[2], (DescriptionBlock{DescriptionBlockType::ListItem, "Small"}));
    EXPECT_EQ(blocks[3], (DescriptionBlock{DescriptionBlockType::OrderedListItem, "One"}));
}

TEST(DescriptionBlocks, EntitiesAndLooseText)
{
    const auto blocks = descriptionBlocks("Loose &amp; text<p>R&amp;D</p>");
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0].text, "Loose & text");
    EXPECT_EQ(blocks[1].text, "R&D");
}

TEST(DescriptionBlocks, MalformedMarkupKeepsTheText)
{
    const auto blocks = descriptionBlocks("<p>Unclosed <em>markup</p>");
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].type, DescriptionBlockType::Paragraph);
    EXPECT_EQ(blocks[0].text, "Unclosed markup");
}

TEST(DescriptionBlocks, EmptyInput)
{
    EXPECT_TRUE(descriptionBlocks("").empty());
    EXPECT_TRUE(descriptionBlocks("  <p> </p> ").empty());
}
