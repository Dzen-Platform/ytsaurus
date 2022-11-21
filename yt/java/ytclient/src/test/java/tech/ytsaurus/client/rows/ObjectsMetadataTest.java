package tech.ytsaurus.client.rows;

import java.util.ArrayList;
import java.util.List;
import java.util.Random;

import org.junit.Assert;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.Parameterized;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

@RunWith(Parameterized.class)
public class ObjectsMetadataTest<T> {

    private static final Logger LOGGER = LoggerFactory.getLogger(ObjectsMetadataTest.class);

    @Parameterized.Parameters(name = "{index}: {0}")
    public static Object[][] parameters() {
        return new Object[][]{
                {SmallObjectClass.class},
                {SmallPrimitiveClass.class},
                {LargeObjectClass.class},
                {LargePrimitiveClass.class},
                {LargeFlattenObjectClass.class},
                {LargeFlattenPrimitiveClass.class},
                {LargeUnflattenObjectClass.class},
                {LargeUnflattenPrimitiveClass.class},
                {LargeWithAllSupportedSerializersClass.class},
                {BrokenIntEnumClass.class}
        };
    }

    @Parameterized.Parameter
    public Class<T> clazz;

    protected List<T> list;
    protected ObjectsMetadata<T> metadata;

    @Before
    public void init() {
        list = new ArrayList<>();
        metadata = ObjectsMetadata.getMetadata(clazz, list::add);
    }

    @Test
    public void testToString() {
        LOGGER.info("{}", metadata.generateObjects(1, new Random(54)).get(0));
    }

    @Test
    public void testGenerate0() {
        Assert.assertEquals(0, metadata.generateObjects(0, new Random(42)).size());
    }

    @Test
    public void testGenerate3() {
        final Random random = new Random(42);
        final List<T> items = metadata.generateObjects(3, random);
        Assert.assertEquals(3, items.size());
        Assert.assertNotEquals(items.get(0), items.get(1));
        Assert.assertNotEquals(items.get(0), items.get(2));
    }

    @Test
    public void testSerializeDeserializeMapped() {
        final Random random = new Random(42);

        final List<T> expect = metadata.generateObjects(3, random);

        final List<byte[]> serialized = metadata.serializeMappedObjects(expect);

        list.clear();
        metadata.deserializeMappedObjects(serialized);
        Assert.assertEquals(expect, list);

        // Список не распухает
        list.clear();
        metadata.deserializeMappedObjects(serialized);
        Assert.assertEquals(expect, list);

    }

    @Test
    public void testSerializeDeserializeUnversioned() {
        final Random random = new Random(42);

        final List<T> expect = metadata.generateObjects(3, random);

        final List<byte[]> serialized = metadata.serializeMappedObjects(expect);

        list.clear();
        metadata.deserializeMappedObjects(serialized);
        Assert.assertEquals(expect, list);

        final UnversionedRowset actual = metadata.deserializeUnversionedObjects(serialized);
        Assert.assertEquals(expect.size(), actual.getRows().size());

        // Сериализация/десерализация работает при многократном конвертировании
        list.clear();
        final List<byte[]> serializedAgain = metadata.serializeUnversionedObjects(actual.getRows());
        metadata.deserializeMappedObjects(serializedAgain);
        Assert.assertEquals(expect, this.list);
    }

    @Test
    public void testSerializeDeserializeLegacy() {
        final Random random = new Random(42);
        final List<T> expect = metadata.generateObjects(3, random);
        final List<byte[]> serialized = metadata.serializeLegacyMappedObjects(expect);

        final List<T> actual = metadata.deserializeLegacyMappedObjects(serialized);
        Assert.assertEquals(expect, actual);
    }

    @Test
    public void testCrossSerialization() {
        final Random random = new Random(42);

        final List<T> expect = metadata.generateObjects(1, random);

        LOGGER.info("serializeMappedObjects >>>");
        final List<byte[]> serializedMapped = metadata.serializeMappedObjects(expect);

        LOGGER.info("serializeLegacyMappedObjects >>>");
        final List<byte[]> serializedLegacy = metadata.serializeLegacyMappedObjects(expect);

        Assert.assertEquals(1, serializedMapped.size());
        Assert.assertEquals(1, serializedLegacy.size());

        list.clear();
        metadata.deserializeMappedObjects(serializedLegacy);

        final List<T> actualMapped = list;
        final List<T> actualLegacy = metadata.deserializeLegacyMappedObjects(serializedMapped);

        Assert.assertEquals(actualLegacy, actualMapped);

        // Note: serializeLegacyMappedObjects может сериализовать поля вложенных объектов в любом порядке
        // Сравнивать serializedLegacy и serializedMapped просто нельзя - они могут отличаться

//        System.out.println(Hex.encodeHr(serializedLegacy.get(0)));
//        System.out.println(Hex.encodeHr(serializedMapped.get(0)));
//        Assert.assertArrayEquals(serializedLegacy.get(0), serializedMapped.get(0));
    }


}
