package tech.ytsaurus.client.operations;

import java.io.BufferedOutputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.OutputStream;
import java.io.PrintStream;
import java.util.Arrays;

public class YtMainUtils {
    protected YtMainUtils() {
    }

    public static void setTempDir() {
        String tmpDir = System.getenv("TMPDIR");
        if (tmpDir == null || tmpDir.length() == 0) {
            throw new IllegalArgumentException("tmp dir not set");
        }
        System.setProperty("java.io.tmpdir", tmpDir);
    }

    public static void disableSystemOutput() {
        System.setOut(new PrintStream(new OutputStream() {

            @Override
            public void write(int b) throws IOException {

            }

            @Override
            public void write(byte[] b) throws IOException {

            }

            @Override
            public void write(byte[] b, int off, int len) throws IOException {

            }

        }));
    }

    public static OutputStream[] buildOutputStreams(int outputTables) throws FileNotFoundException {
        OutputStream[] result = new OutputStream[outputTables];
        for (int i = 0; i < outputTables; ++i) {
            result[i] = new BufferedOutputStream(YtUtils.outputStreamById(1 + 3 * i));
        }
        return result;
    }

    public static OutputStream[] buildOutputStreams(String[] args) throws FileNotFoundException {
        return buildOutputStreams(Integer.parseInt(args[0]));
    }

    public static MapperOrReducer construct(String[] args) {
        if (args[1].equals("simple")) {
            try {
                return (MapperOrReducer<?, ?>) Class.forName(JavaYtRunner.denormalizeClassName(args[2])).newInstance();
            } catch (Exception ex) {
                throw new RuntimeException(ex);
            }
        } else {
            throw new IllegalArgumentException("Can't construct mapper or reducer for args: " + Arrays.toString(args));
        }
    }

}
