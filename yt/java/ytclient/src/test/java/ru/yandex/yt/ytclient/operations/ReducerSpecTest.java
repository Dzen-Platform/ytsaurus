package ru.yandex.yt.ytclient.operations;

import java.time.Duration;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Set;

import org.junit.Assert;
import org.junit.Test;

import ru.yandex.inside.yt.kosher.common.DataSize;
import ru.yandex.inside.yt.kosher.cypress.YPath;
import ru.yandex.inside.yt.kosher.impl.ytree.builder.YTree;
import ru.yandex.inside.yt.kosher.impl.ytree.builder.YTreeBuilder;
import ru.yandex.inside.yt.kosher.operations.OperationContext;
import ru.yandex.inside.yt.kosher.operations.Yield;
import ru.yandex.inside.yt.kosher.ytree.YTreeMapNode;
import ru.yandex.inside.yt.kosher.ytree.YTreeNode;
import ru.yandex.yt.ytclient.proxy.MockYtClient;

public class ReducerSpecTest {
    private final MockYtClient client = new MockYtClient("test");

    @Test
    public void testBasic() {
        int jobCount = 5;
        double cpuLimit = 2.31;
        Set<YPath> additionalFiles = Set.of(YPath.simple("//tmp/123"), YPath.simple("//tmp/592"));
        String networkProject = "network_project_123";
        int customStatisticsCountLimit = 7;
        Map<String, String> environment = Map.of("KEY", "VALUE");
        long jobTimeLimit = 100L;
        List<YPath> layerPaths = List.of(YPath.simple("//tmp/345"));
        DataSize memoryLimit = DataSize.fromMegaBytes(300);
        double memoryReserveFactor = 1.2;
        Duration prepareTimeLimit = Duration.ofSeconds(30);
        boolean useTmpfs = true;
        DataSize tmpfsSize = DataSize.fromMegaBytes(400);

        ReducerSpec spec = ReducerSpec.builder()
                .setReducer(new DummyReducer())
                .setMemoryLimit(memoryLimit)
                .setJobCount(jobCount)
                .setCpuLimit(cpuLimit)
                .setAdditionalFiles(additionalFiles)
                .setNetworkProject(networkProject)
                .setCustomStatisticsCountLimit(customStatisticsCountLimit)
                .setEnvironment(environment)
                .setJobTimeLimit(jobTimeLimit)
                .setLayerPaths(layerPaths)
                .setMemoryReserveFactor(memoryReserveFactor)
                .setPrepareTimeLimit(prepareTimeLimit)
                .setUseTmpfs(useTmpfs)
                .setTmpfsSize(tmpfsSize)
                .build();

        YTreeBuilder builder = YTree.builder();
        Map<String, YTreeNode> node = spec.prepare(
                builder,
                client,
                SpecPreparationContext.builder()
                        .setJarsProcessor(new DummyJarsProcessor())
                        .setJavaBinary("java")
                        .build(),
                1
        ).build().mapNode().asMap();

        Assert.assertTrue(
                node.get("command").stringValue().contains("ru.yandex.yt.ytclient.operations.ReduceMain 1 simple "
                        .concat("ru.yandex.yt.ytclient.operations.ReducerSpecTestdollar_charDummyReducer")));

        Assert.assertEquals("yson", node.get("input_format").stringValue());
        Assert.assertEquals("binary", node.get("input_format").getAttributeOrThrow("format").stringValue());
        Assert.assertEquals("yson", node.get("output_format").stringValue());
        Assert.assertEquals("binary", node.get("output_format").getAttributeOrThrow("format").stringValue());

        Assert.assertEquals(memoryLimit.toBytes(), node.get("memory_limit").intValue());
        Assert.assertEquals(jobCount, node.get("job_count").intValue());
        Assert.assertEquals(cpuLimit, node.get("cpu_limit").doubleValue(), 0.001);
        Assert.assertEquals(2, node.get("file_paths").asList().size());
        node.get("file_paths").asList().forEach(
                p -> Assert.assertTrue(additionalFiles.contains(YPath.simple(p.stringValue()))));
        Assert.assertEquals(networkProject, node.get("network_project").stringValue());
        Assert.assertEquals(customStatisticsCountLimit, node.get("custom_statistics_count_limit").intValue());
        Assert.assertEquals("VALUE", node.get("environment").asMap().get("KEY").stringValue());
        Assert.assertEquals(jobTimeLimit, node.get("job_time_limit").intValue());
        Assert.assertEquals(layerPaths.get(0).toString(), node.get("layer_paths").asList().get(0).stringValue());
        Assert.assertEquals(memoryReserveFactor, node.get("memory_reserve_factor").doubleValue(), 0.001);
        Assert.assertEquals(prepareTimeLimit.toMillis(), node.get("prepare_time_limit").intValue());
        Assert.assertEquals(".", node.get("tmpfs_path").stringValue());
        Assert.assertTrue(node.get("copy_files").boolValue());
        Assert.assertEquals(tmpfsSize.toBytes(), node.get("tmpfs_size").intValue());
    }

    private static class DummyReducer implements Reducer<YTreeMapNode, YTreeMapNode> {
        @Override
        public void reduce(Iterator<YTreeMapNode> entries, Yield<YTreeMapNode> yield, Statistics statistics,
                           OperationContext context) {
        }
    }
}
