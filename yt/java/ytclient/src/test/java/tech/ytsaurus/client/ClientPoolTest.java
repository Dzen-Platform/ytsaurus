package tech.ytsaurus.client;

import java.util.Arrays;
import java.util.List;
import java.util.Random;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.stream.Collectors;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import tech.ytsaurus.client.rpc.RpcClient;

import static org.hamcrest.CoreMatchers.containsString;
import static org.hamcrest.CoreMatchers.hasItem;
import static org.hamcrest.CoreMatchers.is;
import static org.hamcrest.CoreMatchers.not;
import static org.hamcrest.MatcherAssert.assertThat;
import static org.hamcrest.Matchers.oneOf;
import static ru.yandex.yt.testlib.FutureUtils.getError;
import static ru.yandex.yt.testlib.FutureUtils.waitFuture;
import static ru.yandex.yt.testlib.FutureUtils.waitOkResult;
import static ru.yandex.yt.testlib.Matchers.isCausedBy;

class CustomException extends Exception {
    CustomException(String message) {
        super(message);
    }
}

public class ClientPoolTest extends ClientPoolTestBase {
    @Test
    public void testSimple() {
        ClientPool clientPool = newClientPool();

        CompletableFuture<Void> done = new CompletableFuture<>();
        try {

            var clientFuture1 = clientPool.peekClient(done);
            assertThat(clientFuture1.isDone(), is(false));
            clientPool.updateClients(List.of(HostPort.parse("localhost:1")));

            waitFuture(clientFuture1, 100);
            assertThat(clientFuture1.join().getAddressString(), is("localhost:1"));

            var clientFuture2 = clientPool.peekClient(done);
            assertThat(clientFuture2.isDone(), is(true));
            assertThat(clientFuture2.join().getAddressString(), is("localhost:1"));
        } finally {
            done.complete(null);
        }
    }

    @Test
    public void testUpdateEmpty() {
        ClientPool clientPool = newClientPool();

        CompletableFuture<Void> done = new CompletableFuture<>();
        try {
            var clientFuture1 = clientPool.peekClient(done);
            assertThat(clientFuture1.isDone(), is(false));
            clientPool.updateClients(List.of());

            waitFuture(clientFuture1, 1000);
            assertThat(getError(clientFuture1).getMessage(), containsString("Cannot get rpc proxies"));
        } finally {
            done.complete(null);
        }
    }

    @Test
    public void testLingeringConnection() {
        ClientPool clientPool = newClientPool();

        CompletableFuture<Void> done = new CompletableFuture<>();
        try {
            waitFuture(
                    clientPool.updateClients(List.of(HostPort.parse("localhost:1"))),
                    100);
            var clientFuture1 = clientPool.peekClient(done);
            assertThat(clientFuture1.isDone(), is(true));
            assertThat(clientFuture1.join().getAddressString(), is("localhost:1"));
            assertThat(mockRpcClientFactory.isConnectionOpened("localhost:1"), is(true));

            waitFuture(clientPool.updateClients(List.of()), 100);

            assertThat(mockRpcClientFactory.isConnectionOpened("localhost:1"), is(true));
        } finally {
            done.complete(null);
        }
        assertThat(mockRpcClientFactory.isConnectionOpened("localhost:1"), is(false));
    }

    @Test
    public void testCanceledConnection() {
        ClientPool clientPool = newClientPool();

        CompletableFuture<Void> done = new CompletableFuture<>();
        try {
            var clientFuture1 = clientPool.peekClient(done);
            assertThat(clientFuture1.isDone(), is(false));
            clientFuture1.cancel(true);

            waitFuture(
                    clientPool.updateClients(List.of(HostPort.parse("localhost:1"))),
                    100);

            assertThat(mockRpcClientFactory.isConnectionOpened("localhost:1"), is(true));

            waitFuture(clientPool.updateClients(List.of()), 100);

            assertThat(mockRpcClientFactory.isConnectionOpened("localhost:1"), is(false));
        } finally {
            done.complete(null);
        }
    }

    @Test
    public void testUpdateWithError() {
        ClientPool clientPool = newClientPool();

        CompletableFuture<Void> done = new CompletableFuture<>();
        try {
            var clientFuture1 = clientPool.peekClient(done);
            assertThat(clientFuture1.isDone(), is(false));

            waitFuture(clientPool.updateWithError(new CustomException("error update")), 100);

            assertThat(getError(clientFuture1), isCausedBy(CustomException.class));
        } finally {
            done.complete(null);
        }
    }

    @Test
    public void testBanUnban() {
        ClientPool clientPool = newClientPool();

        CompletableFuture<Void> done1 = new CompletableFuture<>();
        CompletableFuture<Void> done2 = new CompletableFuture<>();
        try {
            waitOkResult(
                    clientPool.updateClients(List.of(HostPort.parse("localhost:1"))),
                    100);
            var clientFuture1 = clientPool.peekClient(done1);
            waitFuture(clientFuture1, 100);
            assertThat(clientFuture1.join().getAddressString(), is("localhost:1"));

            var banResult = clientPool.banErrorClient(HostPort.parse("localhost:1"));
            waitFuture(banResult, 100);

            assertThat(mockRpcClientFactory.isConnectionOpened("localhost:1"), is(true));

            done1.complete(null);

            assertThat(mockRpcClientFactory.isConnectionOpened("localhost:1"), is(false));

            var clientFuture2 = clientPool.peekClient(done2);
            assertThat(clientFuture2.isDone(), is(false));

            waitOkResult(
                    clientPool.updateClients(List.of(HostPort.parse("localhost:1"))),
                    100);

            waitFuture(clientFuture2, 100);
            assertThat(clientFuture2.join().getAddressString(), is("localhost:1"));
        } finally {
            done1.complete(null);
            done2.complete(null);
        }
    }

    @Test
    public void testChangedProxyList() {
        ClientPool clientPool = newClientPool();

        waitOkResult(
                clientPool.updateClients(List.of(HostPort.parse("localhost:1"))),
                100);

        assertThat(mockRpcClientFactory.isConnectionOpened("localhost:1"), is(true));

        waitOkResult(
                clientPool.updateClients(List.of(HostPort.parse("localhost:2"), HostPort.parse("localhost:3"))),
                100);

        assertThat(mockRpcClientFactory.isConnectionOpened("localhost:1"), is(false));
        assertThat(mockRpcClientFactory.isConnectionOpened("localhost:2"), is(true));
        assertThat(mockRpcClientFactory.isConnectionOpened("localhost:3"), is(true));

        waitOkResult(
                clientPool.updateClients(List.of(HostPort.parse("localhost:3"), HostPort.parse("localhost:4"))),
                100);

        assertThat(mockRpcClientFactory.isConnectionOpened("localhost:1"), is(false));
        assertThat(mockRpcClientFactory.isConnectionOpened("localhost:2"), is(false));
        assertThat(mockRpcClientFactory.isConnectionOpened("localhost:3"), is(true));
        assertThat(mockRpcClientFactory.isConnectionOpened("localhost:4"), is(true));
    }

    private static boolean tryWaitFuture(CompletableFuture<?> f) {
        try {
            f.get(100, TimeUnit.MILLISECONDS);
        } catch (ExecutionException ignored) {
        } catch (TimeoutException ignored) {
            return false;
        } catch (InterruptedException ex) {
            throw new RuntimeException(ex);
        }
        return true;
    }

    @Test
    public void testOnAllBannedCallback() throws InterruptedException, ExecutionException, TimeoutException {
        var onAllBannedCallback = new Runnable() {
            volatile CompletableFuture<Void> called = new CompletableFuture<>();
            final AtomicInteger counter = new AtomicInteger();

            @Override
            public void run() {
                counter.incrementAndGet();
                var oldCalled = called;
                called = new CompletableFuture<>();
                oldCalled.complete(null);
            }
        };

        ClientPool clientPool = newClientPool();
        clientPool.setOnAllBannedCallback(onAllBannedCallback);

        waitOkResult(
                clientPool.updateClients(
                        List.of(
                                HostPort.parse("localhost:1"),
                                HostPort.parse("localhost:2"),
                                HostPort.parse("localhost:3")
                        )),
                100);

        var f = onAllBannedCallback.called;
        assertThat(onAllBannedCallback.counter.get(), is(0));

        clientPool.banClient("localhost:1");
        assertThat(tryWaitFuture(f), is(false));
        assertThat(onAllBannedCallback.counter.get(), is(0));

        clientPool.banClient("localhost:2");
        assertThat(tryWaitFuture(f), is(false));
        assertThat(onAllBannedCallback.counter.get(), is(0));

        clientPool.banClient("localhost:3");
        // assert no exception
        f.get(1000, TimeUnit.MILLISECONDS);
        assertThat(onAllBannedCallback.counter.get(), is(1));

        waitOkResult(
                clientPool.updateClients(
                        List.of(
                                HostPort.parse("localhost:4"),
                                HostPort.parse("localhost:5")
                        )),
                100);

        f = onAllBannedCallback.called;
        assertThat(onAllBannedCallback.counter.get(), is(1));

        clientPool.banClient("localhost:1");
        assertThat(tryWaitFuture(f), is(false));
        assertThat(onAllBannedCallback.counter.get(), is(1));

        clientPool.banClient("localhost:4");
        assertThat(tryWaitFuture(f), is(false));
        assertThat(onAllBannedCallback.counter.get(), is(1));

        clientPool.banClient("localhost:5");
        // assert no exception
        f.get(1000, TimeUnit.MILLISECONDS);
        assertThat(onAllBannedCallback.counter.get(), is(2));
    }

    @Test
    public void testFilter() {
        ClientPool clientPool = newClientPool();

        waitOkResult(
                clientPool.updateClients(
                        List.of(
                                HostPort.parse("localhost:1"),
                                HostPort.parse("localhost:2"),
                                HostPort.parse("localhost:3")
                        )),
                100
        );

        CompletableFuture<Void> releaseFuture = new CompletableFuture<>();
        var client1 = clientPool.peekClient(
                releaseFuture,
                c -> !List.of("localhost:1", "localhost:3").contains(c.getAddressString())
        );
        assertThat(client1.isDone(), is(true));
        assertThat(client1.join().getAddressString(), is("localhost:2"));

        var client2 = clientPool.peekClient(
                releaseFuture,
                c -> !List.of("localhost:1", "localhost:2", "localhost:3")
                        .contains(c.getAddressString())
        );
        assertThat(client2.isDone(), is(true));
        assertThat(client2.join().getAddressString(), is(oneOf("localhost:1", "localhost:2", "localhost:3")));
    }

    @Test
    public void testWaitWhenAllBanned() {
        ClientPool clientPool = newClientPool();

        waitOkResult(
                clientPool.updateClients(
                        List.of(
                                HostPort.parse("localhost:2"),
                                HostPort.parse("localhost:3"),
                                HostPort.parse("localhost:4")
                        )),
                100
        );
        assertThat(clientPool.banClient("localhost:2").join(), is(1));
        assertThat(clientPool.banClient("localhost:3").join(), is(1));
        assertThat(clientPool.banClient("localhost:4").join(), is(1));

        CompletableFuture<Void> releaseFuture = new CompletableFuture<>();
        var client1 = clientPool.peekClient(releaseFuture);
        tryWaitFuture(client1);
        assertThat(client1.isDone(), is(false));
        waitOkResult(
                clientPool.updateClients(List.of(HostPort.parse("localhost:2"))),
                100
        );
        tryWaitFuture(client1);
        assertThat(client1.isDone(), is(true));
        assertThat(client1.join().getAddressString(), is("localhost:2"));
    }

    @Test
    public void testProxySelector() {
        ClientPool clientPool = newClientPool(ProxySelector.pessimizing(DC.MAN));

        waitOkResult(
                clientPool.updateClients(
                        List.of(
                                HostPort.parse("man-host:2"),
                                HostPort.parse("sas-host:3"),
                                HostPort.parse("vla-host:4"),
                                HostPort.parse("iva-host:5")
                        )),
                100
        );

        List<String> selectedClients = Arrays.stream(clientPool.getAliveClients())
                .map(RpcClient::getAddressString)
                .collect(Collectors.toList());

        assertThat(selectedClients, not(hasItem("man-host:2")));
    }
}

class ClientPoolTestBase {
    ExecutorService executorService;
    MockRpcClientFactory mockRpcClientFactory;

    @Before
    public void before() {
        executorService = Executors.newFixedThreadPool(1);
        mockRpcClientFactory = new MockRpcClientFactory();
    }

    @After
    public void after() {
        executorService.shutdownNow();
    }

    ClientPool newClientPool() {
        return new ClientPool(
                "testDc",
                5,
                mockRpcClientFactory,
                executorService,
                new Random());
    }

    ClientPool newClientPool(ProxySelector proxySelector) {
        return new ClientPool(
                "testDc",
                3,
                mockRpcClientFactory,
                executorService,
                new Random(),
                proxySelector);
    }
}
