package ru.yandex.inside.yt.kosher.impl.ytree.serialization;

import java.io.ByteArrayInputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

import tech.ytsaurus.yson.ClosableYsonConsumer;
import tech.ytsaurus.yson.YsonParser;
import tech.ytsaurus.yson.YsonTextWriter;

import ru.yandex.inside.yt.kosher.impl.ytree.YTreeNodeUtils;
import ru.yandex.inside.yt.kosher.impl.ytree.builder.YTree;
import ru.yandex.inside.yt.kosher.impl.ytree.builder.YTreeBuilder;
import ru.yandex.inside.yt.kosher.ytree.YTreeNode;

/**
 * @author sankear
 */
public final class YTreeTextSerializer {
    private YTreeTextSerializer() {
    }

    public static String serialize(YTreeNode node) {
        StringBuilder sb = new StringBuilder();
        try (ClosableYsonConsumer ysonConsumer = new YsonTextWriter(sb)) {
            YTreeNodeUtils.walk(node, ysonConsumer, false);
        }
        return sb.toString();
    }

    public static String stableSerialize(YTreeNode node) {
        StringBuilder sb = new StringBuilder();
        try (ClosableYsonConsumer ysonConsumer = new YsonTextWriter(sb)) {
            YTreeNodeUtils.walk(node, ysonConsumer, false, true);
        }
        return sb.toString();
    }

    public static YTreeNode deserialize(InputStream input) {
        YTreeBuilder builder = YTree.builder();
        YsonParser ysonParser = new YsonParser(input);
        ysonParser.parseNode(builder);
        return builder.build();
    }

    public static YTreeNode deserialize(String text) {
        return deserialize(new ByteArrayInputStream(text.getBytes(StandardCharsets.UTF_8)));
    }
}
