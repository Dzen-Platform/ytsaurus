package ru.yandex.yt.ytclient.rpc;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;

import org.apache.commons.lang3.RandomUtils;
import org.junit.Assert;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.Parameterized;

import ru.yandex.yt.ytclient.rpc.internal.Codec;
import ru.yandex.yt.ytclient.rpc.internal.Compression;

@RunWith(Parameterized.class)
public class CodecTest {

    @Parameterized.Parameters(name = "{0}: {1}")
    public static Object[] parameters() {
        final Collection<Object[]> params = new ArrayList<>();
        for (Compression compression : Compression.values()) {
            for (int blockSize : new int[]{0, 127, 1024, 32798, 1024 * 1024}) {
                params.add(new Object[]{compression, blockSize});
            }
        }
        return params.toArray();
    }

    @Parameterized.Parameter(0)
    public Compression compression;

    @Parameterized.Parameter(1)
    public int blockSize;

    @Test
    public void testIncompressible() {
        testCompression(RandomUtils.nextBytes(blockSize));
    }

    @Test
    public void testCompressible() {
        final byte[] bytes = new byte[blockSize];
        Arrays.fill(bytes, (byte) 'A');
        testCompression(bytes);
    }

    private void testCompression(byte[] bytes) {
        final Codec codec = Codec.codecFor(compression);
        Assert.assertNotNull(codec);
        final byte[] compressed = codec.compress(bytes);
        final byte[] decompressed = codec.decompress(compressed);
        Assert.assertArrayEquals(bytes, decompressed);
    }
}
