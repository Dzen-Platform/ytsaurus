package ru.yandex.yt.ytclient.examples;

import ru.yandex.inside.yt.kosher.common.GUID;
import ru.yandex.inside.yt.kosher.impl.ytree.YTreeMapNodeImpl;
import ru.yandex.inside.yt.kosher.ytree.YTreeMapNode;
import ru.yandex.yt.ytclient.bus.BusConnector;
import ru.yandex.yt.ytclient.proxy.YtClient;
import ru.yandex.yt.ytclient.proxy.request.AbortJob;
import ru.yandex.yt.ytclient.proxy.request.GetJob;

import static ru.yandex.yt.ytclient.examples.ExamplesUtil.createConnector;
import static ru.yandex.yt.ytclient.examples.ExamplesUtil.getCredentials;

public class JobsExample {
    static void printState(GUID job, YTreeMapNode n) {
        System.out.println("Job " + job + " state: " + n.getString("state"));
    }

    public static void main(String[] args) throws InterruptedException {
        GUID operation = GUID.valueOf(args[0]);
        GUID job = GUID.valueOf(args[1]);
        try (BusConnector connector = createConnector()) {
            try (YtClient client = new YtClient(connector, "hahn", getCredentials())){
                GetJob j = new GetJob(operation, job);
                YTreeMapNodeImpl n = (YTreeMapNodeImpl) client.getJob(j).join();
                printState(job, n);
                AbortJob aj = new AbortJob(job, null);
                client.abortJob(aj).join();
                while (true) {
                    n = (YTreeMapNodeImpl) client.getJob(j).join();
                    printState(job, n);
                    if ("aborted".equals(n.getString("state")))
                        break;
                    Thread.sleep(1000L);
                }
            }
        }
    }
}