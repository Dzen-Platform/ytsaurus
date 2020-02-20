package ru.yandex.yt.ytclient.proxy.internal;

import java.util.List;
import java.util.Random;
import java.util.concurrent.CompletableFuture;

import org.junit.Test;

import ru.yandex.bolts.collection.Cf;
import ru.yandex.inside.yt.kosher.ytree.YTreeNode;
import ru.yandex.yt.TError;
import ru.yandex.yt.ytclient.proxy.ApiServiceClient;
import ru.yandex.yt.ytclient.rpc.RpcClient;
import ru.yandex.yt.ytclient.rpc.RpcClientRequest;
import ru.yandex.yt.ytclient.rpc.RpcClientRequestControl;
import ru.yandex.yt.ytclient.rpc.RpcClientResponseHandler;
import ru.yandex.yt.ytclient.rpc.RpcClientTestStubs;
import ru.yandex.yt.ytclient.rpc.RpcError;
import ru.yandex.yt.ytclient.rpc.RpcOptions;

import static org.hamcrest.MatcherAssert.assertThat;
import static org.hamcrest.Matchers.is;
import static ru.yandex.yt.ytclient.rpc.RpcClientTestStubs.RpcClientFactoryStub;

public class DataCenterTest {
    private HostPort hp(int port) {
        return HostPort.parse("host:" + port);
    }

    @Test
    public void testSetProxies() {
        RpcOptions options = new RpcOptions().setChannelPoolSize(2);
        DataCenter dc = new DataCenter("test", -1.0, options);
        RpcClientFactory factory = new RpcClientFactoryStub();
        Random rnd = new Random(0);
        List<RpcClient> res;

        dc.setProxies(Cf.set(hp(1), hp(2)), factory, rnd);

        res = dc.selectDestinations(10, rnd); // get all available slots
        assertThat(res.toString(), is("[test/2, test/1]"));

        dc.setProxies(Cf.set(hp(1), hp(2), hp(3), hp(4)), factory, rnd);

        res = dc.selectDestinations(10, rnd); // get all available slots
        assertThat(res.toString(), is("[test/2, test/1]"));

        dc.setProxies(Cf.set(hp(3), hp(4)), factory, rnd);

        res = dc.selectDestinations(10, rnd); // get all available slots
        assertThat(res.toString(), is("[test/4, test/3]"));
    }

    @Test
    public void testProxyFail() throws Exception {
        RpcOptions options = new RpcOptions().setChannelPoolSize(3);
        DataCenter dc = new DataCenter("test", -1.0, options);
        RpcClientFactory factory = new RpcClientTestStubs.RpcClientFactoryStub();
        RpcClientFactory failFactory = new RpcClientFactoryStub((name) -> new RpcClientTestStubs.RpcClientStub(name) {
            @Override
            public RpcClientRequestControl send(RpcClient sender, RpcClientRequest request, RpcClientResponseHandler handler) {
                handler.onError(new RuntimeException("Rpc fail"));
                return null;
            }
        });
        Random rnd = new Random(0);
        List<RpcClient> res;

        dc.setProxies(Cf.set(hp(1), hp(2), hp(3)), failFactory, rnd);

        res = dc.selectDestinations(2, rnd);
        assertThat(res.toString(), is("[test/3, test/1]"));

        RpcClient client = res.get(0);
        ApiServiceClient service = new ApiServiceClient(client);
        CompletableFuture<YTreeNode> response = service.listNode("//");
        while (!response.isDone()) {
            Thread.sleep(100);
        }

        assertThat(response.isCompletedExceptionally(), is(true));

        res = dc.selectDestinations(2, rnd);
        assertThat(res.toString(), is("[test/1, test/2]"));

        res = dc.selectDestinations(3, rnd);
        assertThat(res.toString(), is("[test/2, test/1, test/3]")); // return test/3 even is it's broken

        dc.setProxies(Cf.set(hp(1), hp(2), hp(3), hp(4), hp(5)), factory, rnd);

        res = dc.selectDestinations(3, rnd);
        assertThat(res.toString(), is("[test/2, test/1, test/5]")); // replace test/3 with new proxy
    }

    @Test
    public void testProxyFailRecoverable() throws Exception {
        RpcOptions options = new RpcOptions().setChannelPoolSize(3);
        DataCenter dc = new DataCenter("test", -1.0, options);
        RpcClientFactory factory = new RpcClientTestStubs.RpcClientFactoryStub();
        RpcClientFactory failFactory = new RpcClientFactoryStub((name) -> new RpcClientTestStubs.RpcClientStub(name) {
            @Override
            public RpcClientRequestControl send(RpcClient sender, RpcClientRequest request, RpcClientResponseHandler handler) {
                TError error = TError.newBuilder().setCode(1234).build();
                handler.onError(new RpcError(error));
                return null;
            }
        });
        Random rnd = new Random(0);
        List<RpcClient> res;

        dc.setProxies(Cf.set(hp(1), hp(2), hp(3)), failFactory, rnd);

        res = dc.selectDestinations(2, rnd);
        assertThat(res.toString(), is("[test/3, test/1]"));

        RpcClient client = res.get(0);
        ApiServiceClient service = new ApiServiceClient(client);
        CompletableFuture<YTreeNode> response = service.listNode("//");
        while (!response.isDone()) {
            Thread.sleep(100);
        }

        assertThat(response.isCompletedExceptionally(), is(true));

        res = dc.selectDestinations(2, rnd);
        assertThat(res.toString(), is("[test/3, test/1]")); // Request failed, but not unrecoverable, proxy is still good
    }

    @Test
    public void testUseBeforeSetProxies() {
        RpcOptions options = new RpcOptions().setChannelPoolSize(2);
        DataCenter dc = new DataCenter("test", -1.0, options);
        RpcClientFactory factory = new RpcClientFactoryStub();
        Random rnd = new Random(0);
        List<RpcClient> res;

        Thread discovery = new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    Thread.sleep(2000);
                } catch (InterruptedException e) {
                    throw new RuntimeException(e);
                }
                dc.setProxies(Cf.set(hp(1), hp(2)), factory, rnd);
            }
        });
        discovery.start();

        res = dc.selectDestinations(10, rnd); // get all available slots
        assertThat(res.toString(), is("[test/2, test/1]"));
    }
}
