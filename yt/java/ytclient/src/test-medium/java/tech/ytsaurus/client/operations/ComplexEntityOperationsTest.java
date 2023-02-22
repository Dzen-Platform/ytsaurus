package tech.ytsaurus.client.operations;

import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;

import javax.persistence.Entity;

import org.junit.Assert;
import org.junit.Test;
import tech.ytsaurus.client.TableReader;
import tech.ytsaurus.client.TableWriter;
import tech.ytsaurus.client.YtClient;
import tech.ytsaurus.client.YtClientTestBase;
import tech.ytsaurus.client.request.MapOperation;
import tech.ytsaurus.client.request.MapReduceOperation;
import tech.ytsaurus.client.request.ReadTable;
import tech.ytsaurus.client.request.ReduceOperation;
import tech.ytsaurus.client.request.SerializationContext;
import tech.ytsaurus.client.request.SortOperation;
import tech.ytsaurus.client.request.WriteTable;
import tech.ytsaurus.core.cypress.YPath;
import tech.ytsaurus.core.operations.OperationContext;
import tech.ytsaurus.core.operations.Yield;
import tech.ytsaurus.ysontree.YTree;

public class ComplexEntityOperationsTest extends YtClientTestBase {
    public static class MapperOfComplexEntity implements Mapper<Product, NewProduct> {
        @Override
        public void map(Product entry, Yield<NewProduct> yield,
                        Statistics statistics,
                        OperationContext context) {
            String name = entry.name;
            Stats stats = entry.statistics;

            var outputType = new NewProduct();
            outputType.name = name;
            outputType.newStatistics = new Stats();
            outputType.newStatistics.count = stats.count * 2;
            outputType.newStatistics.size = stats.size * 3;
            outputType.newStatistics.info = new Info();
            outputType.newStatistics.info.data = stats.info.data.toUpperCase();

            yield.yield(outputType);
        }
    }

    public static class ReduceCombinerOfComplexEntity implements ReducerWithKey<NewProduct, NewProduct, String> {
        @Override
        public String key(NewProduct entry) {
            return entry.name;
        }

        @Override
        public void reduce(String key, Iterator<NewProduct> input, Yield<NewProduct> yield, Statistics statistics) {
            int sumCount = 0;
            long sumSize = 0;
            while (input.hasNext()) {
                NewProduct product = input.next();
                sumCount += product.newStatistics.count;
                sumSize += product.newStatistics.size;
            }

            yield.yield(new NewProduct(key, new Stats(sumCount, sumSize, new Info(key))));
        }
    }

    public static class ReducerOfComplexEntity implements ReducerWithKey<NewProduct, Stats, String> {
        @Override
        public String key(NewProduct entry) {
            return entry.name;
        }

        @Override
        public void reduce(String key, Iterator<NewProduct> input, Yield<Stats> yield, Statistics statistics) {
            int sumCount = 0;
            long sumSize = 0;
            while (input.hasNext()) {
                NewProduct product = input.next();
                sumCount += product.newStatistics.count;
                sumSize += product.newStatistics.size;
            }

            yield.yield(new Stats(sumCount, sumSize, new Info(key)));

        }
    }

    @Test
    public void testMap() {
        var ytFixture = createYtFixture();
        var yt = ytFixture.getYt();
        var inputTable = ytFixture.getTestDirectory().child("map-input-table");
        var outputTable = ytFixture.getTestDirectory().child("map-output-table");

        write(yt, inputTable, List.of(
                        new Product("a", new Stats(1, 2, new Info("data 1"))),
                        new Product("b", new Stats(3, 4, new Info("data 2")))),
                Product.class
        );

        Operation op = yt.map(MapOperation.builder()
                .setSpec(MapSpec.builder()
                        .setMapperSpec(new MapperSpec(new MapperOfComplexEntity()))
                        .setInputTables(inputTable)
                        .setOutputTables(outputTable)
                        .build())
                .build()).join();

        Assert.assertEquals(OperationStatus.COMPLETED, op.getStatus().join());

        Set<NewProduct> result = read(yt, outputTable, NewProduct.class);

        var expected = Set.of(
                new NewProduct("a", new Stats(2, 6, new Info("DATA 1"))),
                new NewProduct("b", new Stats(6, 12, new Info("DATA 2")))
        );

        Assert.assertEquals(expected, result);
    }

    @Test
    public void testReduce() {
        var ytFixture = createYtFixture();
        var yt = ytFixture.getYt();
        var inputTable = ytFixture.getTestDirectory().child("reduce-input-table");
        var outputTable = ytFixture.getTestDirectory().child("reduce-output-table");

        write(yt, inputTable, List.of(
                        new NewProduct("a", new Stats(1, 2, new Info("data 1"))),
                        new NewProduct("b", new Stats(3, 4, new Info("data 2"))),
                        new NewProduct("b", new Stats(5, 6, new Info("data 3")))),
                NewProduct.class
        );

        Operation sortOp = yt.sort(SortOperation.builder()
                .setSpec(SortSpec.builder()
                        .setInputTables(inputTable)
                        .setOutputTable(inputTable)
                        .setSortBy("name")
                        .build())
                .build()).join();

        Assert.assertEquals(OperationStatus.COMPLETED, sortOp.getStatus().join());

        Operation op = yt.reduce(ReduceOperation.builder()
                .setSpec(ReduceSpec.builder()
                        .setReducerSpec(new ReducerSpec(new ReducerOfComplexEntity()))
                        .setInputTables(inputTable)
                        .setOutputTables(outputTable)
                        .setReduceBy("name")
                        .build())
                .build()
        ).join();

        Assert.assertEquals(OperationStatus.COMPLETED, op.getStatus().join());

        Set<Stats> result = read(yt, outputTable, Stats.class);

        Set<Stats> expected = Set.of(
                new Stats(1, 2, new Info("a")),
                new Stats(8, 10, new Info("b"))
        );

        Assert.assertEquals(expected, result);
    }

    @Test
    public void testMapReduce() {
        var ytFixture = createYtFixture();
        var yt = ytFixture.getYt();
        var inputTable = ytFixture.getTestDirectory().child("mapreduce-input-table");
        var outputTable = ytFixture.getTestDirectory().child("mapreduce-output-table");

        write(yt, inputTable, List.of(
                        new Product("a", new Stats(1, 2, new Info("data 1"))),
                        new Product("b", new Stats(3, 4, new Info("data 2"))),
                        new Product("b", new Stats(5, 6, new Info("data 3")))),
                Product.class
        );

        Operation op = yt.mapReduce(MapReduceOperation.builder()
                .setSpec(MapReduceSpec.builder()
                        .setReducerSpec(new ReducerSpec(new ReducerOfComplexEntity()))
                        .setMapperSpec(new MapperSpec(new MapperOfComplexEntity()))
                        .setReduceCombinerSpec(new ReducerSpec(new ReduceCombinerOfComplexEntity()))
                        .setInputTables(inputTable)
                        .setOutputTables(outputTable)
                        .setReduceBy("name")
                        .setSortBy("name")
                        .setAdditionalSpecParameters(Map.of(
                                "force_reduce_combiners", YTree.booleanNode(true)))
                        .build())
                .build()
        ).join();

        Assert.assertEquals(OperationStatus.COMPLETED, op.getStatus().join());

        Set<Stats> result = read(yt, outputTable, Stats.class);

        Set<Stats> expected = Set.of(
                new Stats(2, 6, new Info("a")),
                new Stats(16, 30, new Info("b"))
        );

        Assert.assertEquals(expected, result);
    }

    private static <T> Set<T> read(YtClient yt, YPath path, Class<T> clazz) {
        TableReader<T> reader = yt.readTable(
                ReadTable.<T>builder()
                        .setPath(path)
                        .setSerializationContext(new SerializationContext<>(clazz))
                        .build()).join();

        Set<T> result = new HashSet<>();
        List<T> currentRows;
        try {
            while (reader.canRead()) {
                while ((currentRows = reader.read()) != null) {
                    result.addAll(currentRows);
                }
                reader.readyEvent().join();
            }
        } catch (Exception ex) {
            throw new RuntimeException("Failed to read", ex);
        } finally {
            reader.close().join();
        }
        return result;
    }

    private static <T> void write(YtClient yt, YPath path, List<T> rows, Class<T> clazz) {
        TableWriter<T> writer = yt.writeTable(
                WriteTable.<T>builder()
                        .setPath(path)
                        .setSerializationContext(new SerializationContext<>(clazz))
                        .setNeedRetries(true)
                        .build()).join();

        try {
            while (true) {
                writer.readyEvent().join();

                if (writer.write(rows)) {
                    break;
                }
            }
        } catch (Exception ex) {
            throw new RuntimeException("Failed to write", ex);
        } finally {
            writer.close().join();
        }
    }

    @Entity
    private static class Product {
        String name;
        Stats statistics;

        Product() {
        }

        Product(String name, Stats statistics) {
            this.name = name;
            this.statistics = statistics;
        }
    }

    @Entity
    private static class Stats {
        int count;
        long size;
        Info info;

        Stats() {
        }

        Stats(int count, long size, Info info) {
            this.count = count;
            this.size = size;
            this.info = info;
        }

        @Override
        public boolean equals(Object o) {
            if (this == o) {
                return true;
            }
            if (o == null || getClass() != o.getClass()) {
                return false;
            }
            Stats that = (Stats) o;
            return count == that.count && size == that.size && Objects.equals(info, that.info);
        }

        @Override
        public int hashCode() {
            return Objects.hash(count, size, info);
        }
    }

    @Entity
    private static class Info {
        String data;

        Info() {
        }

        Info(String data) {
            this.data = data;
        }

        @Override
        public boolean equals(Object o) {
            if (this == o) {
                return true;
            }
            if (o == null || getClass() != o.getClass()) {
                return false;
            }
            Info info = (Info) o;
            return Objects.equals(data, info.data);
        }

        @Override
        public int hashCode() {
            return Objects.hash(data);
        }
    }

    @Entity
    private static class NewProduct {
        String name;
        Stats newStatistics;

        NewProduct() {
        }

        NewProduct(String name, Stats newStatistics) {
            this.name = name;
            this.newStatistics = newStatistics;
        }

        @Override
        public boolean equals(Object o) {
            if (this == o) {
                return true;
            }
            if (o == null || getClass() != o.getClass()) {
                return false;
            }
            NewProduct that = (NewProduct) o;
            return Objects.equals(name, that.name) && Objects.equals(newStatistics, that.newStatistics);
        }

        @Override
        public int hashCode() {
            return Objects.hash(name, newStatistics);
        }
    }
}
