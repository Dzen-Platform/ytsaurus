package ru.yandex.yt.ytclient.proxy.request;

import tech.ytsaurus.core.cypress.YPath;

import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;

@NonNullApi
@NonNullFields
public class FreezeTable extends tech.ytsaurus.client.request.FreezeTable.BuilderBase<FreezeTable> {
    public FreezeTable(YPath path) {
        setPath(path.justPath());
    }

    /**
     * @deprecated Use {@link #FreezeTable(YPath path)} instead.
     */
    @Deprecated
    public FreezeTable(String path) {
        setPath(path);
    }

    @Override
    protected FreezeTable self() {
        return this;
    }
}


