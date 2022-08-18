package ru.yandex.yt.ytclient.operations;

import java.io.InputStream;
import java.io.OutputStream;
import java.util.Iterator;

import ru.yandex.inside.yt.kosher.operations.OperationContext;
import ru.yandex.inside.yt.kosher.operations.Yield;

public class ReduceMain {
    private ReduceMain() {
    }

    public static void main(String[] args) throws Exception {
        YtMainUtils.setTempDir();
        YtMainUtils.disableSystemOutput();
        OutputStream[] output = YtMainUtils.buildOutputStreams(args);

        Reducer reducer = (Reducer) YtMainUtils.construct(args);

        try {
            applyReducer(reducer, System.in, output, new StatisticsImpl());
        } catch (Throwable e) {
            e.printStackTrace(System.err);
            System.exit(2);
        } finally {
            System.exit(0);
        }
    }

    public static <I, O> void applyReducer(Reducer<I, O> reducer, InputStream in,
                                           OutputStream[] output, Statistics statistics) throws Exception {
        YTableEntryType<I> inputType = reducer.inputType();
        YTableEntryType<O> outputType = reducer.outputType();
        Yield<O> yield = outputType.yield(output);

        OperationContext context = new OperationContext();

        try {
            Iterator<I> it = inputType.iterator(in, context);
            reducer.start(yield, statistics);
            reducer.reduce(it, yield, statistics, context);
            it.forEachRemaining(tmp -> {
            });
            reducer.finish(yield, statistics);
        } finally {
            yield.close();
            statistics.close();
        }
    }
}