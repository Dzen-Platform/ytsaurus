package tech.ytsaurus.client.operations;

import java.io.InputStream;
import java.io.OutputStream;
import java.io.Serializable;

import tech.ytsaurus.core.operations.CloseableIterator;
import tech.ytsaurus.core.operations.OperationContext;
import tech.ytsaurus.core.operations.Yield;
import tech.ytsaurus.ysontree.YTreeStringNode;


public interface YTableEntryType<T> extends Serializable {

    YTreeStringNode format();

    // either one should be implemented
    default CloseableIterator<T> iterator(InputStream input) {
        return iterator(input, new OperationContext());
    }

    default CloseableIterator<T> iterator(InputStream input, OperationContext context) {
        return iterator(input);
    }

    Yield<T> yield(OutputStream[] output);

}
