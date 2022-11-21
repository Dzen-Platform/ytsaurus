package ru.yandex.yt.ytclient.examples;

import java.util.Arrays;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import tech.ytsaurus.client.ApiServiceTransaction;
import tech.ytsaurus.client.ApiServiceTransactionOptions;
import tech.ytsaurus.client.LookupRowsRequest;
import tech.ytsaurus.client.ModifyRowsRequest;
import tech.ytsaurus.client.rows.UnversionedRowset;

import ru.yandex.yt.rpcproxy.ETransactionType;
import ru.yandex.yt.ytclient.tables.ColumnValueType;
import ru.yandex.yt.ytclient.tables.TableSchema;

public class ModifyRowsExample2 {
    private static final Logger logger = LoggerFactory.getLogger(ModifyRowsExample.class);

    public static void main(String[] args) {
        String path = "//home/direct/tmp/snaury/dyn-key-value";
        TableSchema schema = new TableSchema.Builder()
                .addKey("key", ColumnValueType.STRING)
                .addValue("value", ColumnValueType.STRING)
                .build();
        ExamplesUtil.runExampleWithBalancing(client -> {
            ApiServiceTransactionOptions transactionOptions =
                    new ApiServiceTransactionOptions(ETransactionType.TT_TABLET)
                            .setSticky(true);
            try (ApiServiceTransaction transaction = client.startTransaction(transactionOptions).join()) {
                logger.info("Transaction started: {} (timestamp={}, ping={}, sticky={})",
                        transaction.getId(),
                        transaction.getStartTimestamp(),
                        transaction.isPing(),
                        transaction.isSticky());

                transaction.ping().join();
                logger.info("Transaction ping succeeded!");

                {
                    ModifyRowsRequest request = new ModifyRowsRequest(path, schema)
                            .addUpdate(Arrays.asList("key1", "value111"))
                            .addUpdate(Arrays.asList("key2", "value222"));
                    long t0 = System.nanoTime();
                    transaction.modifyRows(request).join();
                    long t1 = System.nanoTime();
                    logger.info("Modify time: {}ms", (t1 - t0) / 1000000.0);
                }

                {
                    LookupRowsRequest request = new LookupRowsRequest(path, schema.toLookup())
                            .addFilter("key1")
                            .addFilter("key2");
                    UnversionedRowset rowset = transaction.lookupRows(request).join();
                    logger.info("LookupRows result: {}", rowset);
                }

                try (ApiServiceTransaction t2 = client.startTransaction(transactionOptions).join()) {
                    logger.info("Started second transaction: {} (timestamp={})", t2.getId(), t2.getStartTimestamp());

                    {
                        LookupRowsRequest request = new LookupRowsRequest(path, schema.toLookup())
                                .addFilter("key1")
                                .addFilter("key2");
                        UnversionedRowset rowset = t2.lookupRows(request).join();
                        logger.info("LookupRows result: {}", rowset);
                    }
                }

                transaction.commit().join();
                logger.info("Transaction committed!");
            }
        });
    }
}
