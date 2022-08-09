package ru.yandex.yt.ytclient.proxy.request;

import ru.yandex.inside.yt.kosher.cypress.YPath;

public class PutFileToCacheResult {
    private final YPath path;

    public PutFileToCacheResult(YPath path) {
        this.path = path;
    }

    public YPath getPath() {
        return path;
    }
}
